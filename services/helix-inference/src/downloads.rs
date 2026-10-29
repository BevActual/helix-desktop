//! Getting models: the gateway fetches GGUF files from Hugging Face into the models folder, so every app sees them and none of them needs the network
//! or write access there. A download is chosen by the person in Helix Models; nothing is fetched otherwise.
//!
//! * What a repository offers comes from the hub's file listing (`/api/models/<repo>/tree/main`), which also gives each file's size and SHA-256.
//! * A model split into shards is fetched whole; a vision projector (`mmproj-*.gguf`) can come with it, saved under a name the engine pairs with the
//!   model (`mmproj-<model base>-…`), whatever the repository called it.
//! * Each file goes to `<models>/.downloads/<name>.part`, resumes where it stopped (HTTP Range), is checked against its SHA-256 and size, and only then
//!   is moved into the folder. There must be room for it first. Only `https` to the hub (a test may point at a local `http` hub).

use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde_json::{json, Value};
use sha2::{Digest, Sha256};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HubFile {
    pub path: String,
    pub size: u64,
    pub sha256: String,
}

/// One file to fetch, and the plain name it is saved under.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Item {
    pub path: String,
    pub size: u64,
    pub sha256: String,
    pub save_as: String,
}

fn hub() -> String {
    std::env::var("HELIX_MODEL_HUB")
        .ok()
        .filter(|v| !v.is_empty())
        .unwrap_or_else(|| "https://huggingface.co".into())
}

fn hub_ok(base: &str) -> bool {
    base.starts_with("https://")
        || base.starts_with("http://127.0.0.1:")
        || base.starts_with("http://localhost:")
}

/// `owner/name`, letters, digits, `.`, `_`, `-` only.
pub fn repo_ok(repo: &str) -> bool {
    let mut parts = repo.split('/');
    let ok = |p: Option<&str>| {
        p.is_some_and(|p| {
            !p.is_empty()
                && p.len() <= 96
                && !p.starts_with('.')
                && p.chars()
                    .all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '_' | '-'))
        })
    };
    ok(parts.next()) && ok(parts.next()) && parts.next().is_none()
}

fn agent(timeout: Option<Duration>) -> ureq::Agent {
    ureq::Agent::config_builder()
        .timeout_global(timeout)
        .http_status_as_error(false)
        .build()
        .into()
}

/// The GGUF files in a repository, from the hub's listing (folders are not followed).
pub fn list(repo: &str) -> Result<Vec<HubFile>, String> {
    if !repo_ok(repo) {
        return Err("A repository is written owner/name.".into());
    }
    let base = hub();
    if !hub_ok(&base) {
        return Err("The model hub must be reached over https.".into());
    }
    let mut response = agent(Some(Duration::from_secs(20)))
        .get(&format!("{base}/api/models/{repo}/tree/main"))
        .call()
        .map_err(|e| format!("Hugging Face did not answer: {e}"))?;
    let status = response.status().as_u16();
    if status == 404 || status == 401 {
        return Err(format!(
            "{repo} was not found on Hugging Face (or it needs a login)."
        ));
    }
    if status != 200 {
        return Err(format!("Hugging Face answered {status}."));
    }
    let text = response
        .body_mut()
        .with_config()
        .limit(8 << 20)
        .read_to_string()
        .map_err(|e| e.to_string())?;
    Ok(parse_listing(&text))
}

pub fn parse_listing(text: &str) -> Vec<HubFile> {
    let value: Value = serde_json::from_str(text).unwrap_or(Value::Null);
    let mut out: Vec<HubFile> = value
        .as_array()
        .into_iter()
        .flatten()
        .filter(|f| f["type"] == "file")
        .filter_map(|f| {
            let path = f["path"].as_str()?;
            let sha256 = f.pointer("/lfs/oid").and_then(Value::as_str)?;
            let size = f
                .pointer("/lfs/size")
                .and_then(Value::as_u64)
                .or_else(|| f["size"].as_u64())?;
            let plain = path.ends_with(".gguf")
                && !path.contains('/')
                && !path.starts_with('.')
                && path.len() <= 200;
            let hex = sha256.len() == 64 && sha256.chars().all(|c| c.is_ascii_hexdigit());
            (plain && hex).then(|| HubFile {
                path: path.to_string(),
                size,
                sha256: sha256.to_ascii_lowercase(),
            })
        })
        .collect();
    out.sort_by(|a, b| a.path.cmp(&b.path));
    out
}

/// `name-00001-of-00003.gguf` → (`name`, 3).
fn shard_of(path: &str) -> Option<(&str, u32)> {
    let stem = path.strip_suffix(".gguf")?;
    let (base, tail) = stem.split_at_checked(stem.len().checked_sub(15)?)?;
    let (index, total) = tail.strip_prefix('-')?.split_once("-of-")?;
    let digits = |s: &str| s.len() == 5 && s.bytes().all(|b| b.is_ascii_digit());
    (digits(index) && digits(total))
        .then(|| (base, total.parse().ok()))
        .and_then(|(b, t)| Some((b, t?)))
}

/// What to fetch for `wanted` (a model file the listing offers): all its shards, and, if asked, the repository's vision projector.
pub fn plan(files: &[HubFile], wanted: &str, projector: bool) -> Result<Vec<Item>, String> {
    let model = files
        .iter()
        .find(|f| f.path == wanted && !f.path.starts_with("mmproj"))
        .ok_or("That file is not in the repository.")?;
    let mut items: Vec<Item> = match shard_of(&model.path) {
        Some((base, total)) => {
            let parts: Vec<&HubFile> = files
                .iter()
                .filter(|f| shard_of(&f.path).is_some_and(|(b, t)| b == base && t == total))
                .collect();
            if parts.len() != total as usize {
                return Err(
                    "Some parts of this split model are missing from the repository.".into(),
                );
            }
            parts
                .iter()
                .map(|f| Item {
                    path: f.path.clone(),
                    size: f.size,
                    sha256: f.sha256.clone(),
                    save_as: f.path.clone(),
                })
                .collect()
        }
        None => vec![Item {
            path: model.path.clone(),
            size: model.size,
            sha256: model.sha256.clone(),
            save_as: model.path.clone(),
        }],
    };
    if projector {
        let candidates: Vec<&HubFile> = files
            .iter()
            .filter(|f| f.path.starts_with("mmproj"))
            .collect();
        let pick = ["q8_0", "f16", "bf16"]
            .iter()
            .find_map(|q| {
                candidates
                    .iter()
                    .find(|f| f.path.to_ascii_lowercase().contains(q))
            })
            .or(candidates.first())
            .ok_or("This repository has no vision projector.")?;
        let base = crate::models::base_of(&model.path);
        let rest = pick
            .path
            .strip_prefix("mmproj-")
            .or(pick.path.strip_prefix("mmproj"))
            .unwrap_or(&pick.path)
            .trim_start_matches(['-', '_']);
        // The engine pairs `mmproj-<model base>…` with the model; keep the repository's name when it already says that.
        let save_as = if pick.path.starts_with(&format!("mmproj-{base}")) {
            pick.path.clone()
        } else {
            format!("mmproj-{base}-{rest}")
        };
        items.push(Item {
            path: pick.path.clone(),
            size: pick.size,
            sha256: pick.sha256.clone(),
            save_as,
        });
    }
    Ok(items)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    Running,
    Done,
    Failed,
    Cancelled,
}

pub struct Job {
    pub id: u64,
    pub repo: String,
    pub title: String,
    pub total: u64,
    pub done: Arc<AtomicU64>,
    pub cancel: Arc<AtomicBool>,
    pub state: Arc<Mutex<(State, String)>>,
}

#[derive(Default)]
pub struct Downloads {
    jobs: Mutex<Vec<Job>>,
    next: AtomicU64,
}

/// Free bytes on the file system holding `dir`.
fn free_bytes(dir: &Path) -> Option<u64> {
    let c = std::ffi::CString::new(dir.as_os_str().as_encoded_bytes()).ok()?;
    let mut stat: libc::statvfs = unsafe { std::mem::zeroed() };
    // SAFETY: `c` is a valid C string and `stat` a valid out-pointer for the call.
    (unsafe { libc::statvfs(c.as_ptr(), &mut stat) } == 0)
        .then(|| stat.f_bavail as u64 * stat.f_frsize as u64)
}

/// A name that can only land inside the folder.
fn plain_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 220
        && name.ends_with(".gguf")
        && !name.starts_with('.')
        && !name.contains(['/', '\\', '\0'])
}

impl Downloads {
    pub fn view(&self) -> Value {
        let jobs = self.jobs.lock().unwrap();
        Value::Array(
            jobs.iter()
                .rev()
                .take(20)
                .map(|j| {
                    let (state, message) = j.state.lock().unwrap().clone();
                    json!({
                        "id": j.id, "repo": j.repo, "title": j.title, "total": j.total, "done": j.done.load(Ordering::Relaxed),
                        "state": match state { State::Running => "running", State::Done => "done", State::Failed => "failed", State::Cancelled => "cancelled" },
                        "message": message,
                    })
                })
                .collect(),
        )
    }

    pub fn cancel(&self, id: u64) -> bool {
        let jobs = self.jobs.lock().unwrap();
        match jobs.iter().find(|j| j.id == id) {
            Some(job) if job.state.lock().unwrap().0 == State::Running => {
                job.cancel.store(true, Ordering::Relaxed);
                true
            }
            _ => false,
        }
    }

    /// Starts fetching `items` into `dir` on a thread of its own. Refused when there is not room, or a name is not plain.
    pub fn start(&self, repo: &str, items: Vec<Item>, dir: &Path) -> Result<u64, String> {
        if items
            .iter()
            .any(|i| !plain_name(&i.save_as) || !plain_name(&i.path))
        {
            return Err("A file name in that repository is not usable.".into());
        }
        let total: u64 = items.iter().map(|i| i.size).sum();
        std::fs::create_dir_all(dir.join(".downloads"))
            .map_err(|e| format!("The models folder cannot be written: {e}"))?;
        // Room for the files and a margin, less what is already there from an earlier try.
        let partial: u64 = items
            .iter()
            .filter_map(|i| {
                std::fs::metadata(dir.join(".downloads").join(format!("{}.part", i.save_as))).ok()
            })
            .map(|m| m.len())
            .sum();
        let need = total.saturating_sub(partial) + (1 << 30);
        if let Some(free) = free_bytes(dir) {
            if free < need {
                return Err(format!(
                    "There is not enough room: {:.1} GB free, {:.1} GB needed.",
                    free as f64 / 1e9,
                    need as f64 / 1e9
                ));
            }
        }
        {
            let jobs = self.jobs.lock().unwrap();
            if jobs.iter().any(|j| {
                j.repo == repo
                    && j.title == items[0].save_as
                    && j.state.lock().unwrap().0 == State::Running
            }) {
                return Err("That model is already being downloaded.".into());
            }
        }
        let id = self.next.fetch_add(1, Ordering::Relaxed) + 1;
        let job = Job {
            id,
            repo: repo.to_string(),
            title: items[0].save_as.clone(),
            total,
            done: Arc::new(AtomicU64::new(0)),
            cancel: Arc::new(AtomicBool::new(false)),
            state: Arc::new(Mutex::new((State::Running, String::new()))),
        };
        let (done, cancel, state) = (
            Arc::clone(&job.done),
            Arc::clone(&job.cancel),
            Arc::clone(&job.state),
        );
        self.jobs.lock().unwrap().push(job);
        let (repo, dir) = (repo.to_string(), dir.to_path_buf());
        std::thread::spawn(move || {
            let result = fetch_all(&repo, &items, &dir, &done, &cancel);
            let outcome = match result {
                Ok(()) => (State::Done, String::new()),
                Err(_) if cancel.load(Ordering::Relaxed) => (
                    State::Cancelled,
                    "Cancelled; it will continue where it stopped next time.".into(),
                ),
                Err(e) => (State::Failed, e),
            };
            eprintln!(
                "helix-inference: download {repo} {} {:?}",
                items[0].save_as, outcome.0
            );
            *state.lock().unwrap() = outcome;
        });
        Ok(id)
    }
}

fn fetch_all(
    repo: &str,
    items: &[Item],
    dir: &Path,
    done: &AtomicU64,
    cancel: &AtomicBool,
) -> Result<(), String> {
    for item in items {
        let target = dir.join(&item.save_as);
        if std::fs::metadata(&target).is_ok_and(|m| m.len() == item.size) {
            done.fetch_add(item.size, Ordering::Relaxed);
            continue; // already here
        }
        fetch_one(repo, item, dir, done, cancel)?;
    }
    Ok(())
}

fn fetch_one(
    repo: &str,
    item: &Item,
    dir: &Path,
    done: &AtomicU64,
    cancel: &AtomicBool,
) -> Result<(), String> {
    let base = hub();
    if !hub_ok(&base) {
        return Err("The model hub must be reached over https.".into());
    }
    let part: PathBuf = dir
        .join(".downloads")
        .join(format!("{}.part", item.save_as));
    let mut file = std::fs::OpenOptions::new()
        .create(true)
        .read(true)
        .append(true)
        .open(&part)
        .map_err(|e| e.to_string())?;
    // What is already there counts, and goes into the checksum first.
    let mut hasher = Sha256::new();
    let mut have = 0u64;
    {
        file.seek(SeekFrom::Start(0)).map_err(|e| e.to_string())?;
        let mut buffer = vec![0u8; 1 << 20];
        loop {
            let n = file.read(&mut buffer).map_err(|e| e.to_string())?;
            if n == 0 {
                break;
            }
            hasher.update(&buffer[..n]);
            have += n as u64;
        }
    }
    if have > item.size {
        drop(file);
        let _ = std::fs::remove_file(&part);
        return fetch_one(repo, item, dir, done, cancel);
    }
    done.fetch_add(have, Ordering::Relaxed);
    if have < item.size {
        let url = format!("{base}/{repo}/resolve/main/{}", item.path);
        let mut call = agent(None).get(&url);
        if have > 0 {
            call = call.header("Range", &format!("bytes={have}-"));
        }
        let mut response = call
            .call()
            .map_err(|e| format!("Hugging Face did not answer: {e}"))?;
        let status = response.status().as_u16();
        if status == 200 && have > 0 {
            // The server sent the whole file again: start over.
            drop(file);
            let _ = std::fs::remove_file(&part);
            done.fetch_sub(have, Ordering::Relaxed);
            return fetch_one(repo, item, dir, done, cancel);
        }
        if status != 200 && status != 206 {
            return Err(format!("Hugging Face answered {status} for {}.", item.path));
        }
        let mut reader = response.body_mut().with_config().limit(u64::MAX).reader();
        let mut buffer = vec![0u8; 1 << 20];
        loop {
            if cancel.load(Ordering::Relaxed) {
                return Err("cancelled".into());
            }
            let n = reader
                .read(&mut buffer)
                .map_err(|e| format!("The download broke off: {e}"))?;
            if n == 0 {
                break;
            }
            file.write_all(&buffer[..n]).map_err(|e| e.to_string())?;
            hasher.update(&buffer[..n]);
            have += n as u64;
            done.fetch_add(n as u64, Ordering::Relaxed);
            if have > item.size {
                break;
            }
        }
        file.flush().map_err(|e| e.to_string())?;
    }
    let digest = format!("{:x}", hasher.finalize());
    if have != item.size || digest != item.sha256 {
        drop(file);
        let _ = std::fs::remove_file(&part);
        return Err(format!(
            "{} did not arrive intact (checksum or size mismatch); try again.",
            item.path
        ));
    }
    std::fs::rename(&part, dir.join(&item.save_as)).map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn file(path: &str, size: u64) -> HubFile {
        HubFile {
            path: path.into(),
            size,
            sha256: "a".repeat(64),
        }
    }

    #[test]
    fn the_listing_keeps_plain_gguf_files_with_checksums() {
        let text = r#"[
          {"type":"file","path":"README.md","size":10},
          {"type":"file","path":"m-Q4_K_M.gguf","size":5,"lfs":{"oid":"ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789","size":5}},
          {"type":"file","path":"sub/x.gguf","size":5,"lfs":{"oid":"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789","size":5}},
          {"type":"file","path":"nolfs.gguf","size":5},
          {"type":"directory","path":"dir"}
        ]"#;
        let files = parse_listing(text);
        assert_eq!(files.len(), 1);
        assert_eq!(files[0].path, "m-Q4_K_M.gguf");
        assert_eq!(
            files[0].sha256,
            "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"
        );
    }

    #[test]
    fn a_split_model_comes_whole_and_a_projector_gets_a_name_the_engine_pairs() {
        let files = vec![
            file("big-q4_k_m-00001-of-00002.gguf", 10),
            file("big-q4_k_m-00002-of-00002.gguf", 20),
            file("gemma-3-4b-it-Q4_K_M.gguf", 30),
            file("mmproj-model-f16.gguf", 4),
            file("mmproj-model-q8_0.gguf", 3),
        ];
        let split = plan(&files, "big-q4_k_m-00001-of-00002.gguf", false).unwrap();
        assert_eq!(split.iter().map(|i| i.size).sum::<u64>(), 30);
        let with = plan(&files, "gemma-3-4b-it-Q4_K_M.gguf", true).unwrap();
        assert_eq!(
            with[1].save_as, "mmproj-gemma-3-4b-it-model-q8_0.gguf",
            "Q8_0 preferred, renamed after the model"
        );
        assert!(plan(&files, "missing.gguf", false).is_err());
        assert!(
            plan(&files, "mmproj-model-f16.gguf", false).is_err(),
            "a projector is not a model"
        );
        assert!(
            plan(
                &[file("big-q4-00001-of-00003.gguf", 1)],
                "big-q4-00001-of-00003.gguf",
                false
            )
            .is_err(),
            "an incomplete set is refused"
        );
        assert!(
            plan(&[file("x.gguf", 1)], "x.gguf", true).is_err(),
            "no projector to bring"
        );
    }

    #[test]
    fn repositories_and_names_are_plain() {
        assert!(repo_ok("Qwen/Qwen3-8B-GGUF") && repo_ok("ggml-org/gemma-3-4b-it-GGUF"));
        for bad in ["Qwen", "a/b/c", "../x/y", "a/.b", "a/b c", ""] {
            assert!(!repo_ok(bad), "{bad}");
        }
        assert!(
            plain_name("m.gguf")
                && !plain_name("../m.gguf")
                && !plain_name(".m.gguf")
                && !plain_name("m.bin")
        );
    }

    // --- against a scripted hub ---------------------------------------------------------------------------------------------------------

    const HUB: &str = r#"
import hashlib, json, sys, time
from http.server import BaseHTTPRequestHandler, HTTPServer
blobs = {"m-Q4_K_M.gguf": bytes(range(256)) * 4000, "mmproj-m-f16.gguf": b"projector" * 100, "bad-Q4_K_M.gguf": b"x" * 5000, "slow-Q4_K_M.gguf": b"s" * 3000000}
def entry(name, data, lie=False):
    oid = hashlib.sha256(b"different" if lie else data).hexdigest()
    return {"type": "file", "path": name, "size": len(data), "lfs": {"oid": oid, "size": len(data)}}
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path.startswith("/api/models/o/r/tree/main"):
            body = json.dumps([entry(n, d, lie=n.startswith("bad")) for n, d in blobs.items()]).encode()
            self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body); return
        name = self.path.rsplit("/", 1)[-1]
        data = blobs.get(name)
        if data is None or "/o/r/resolve/main/" not in self.path:
            self.send_response(404); self.end_headers(); return
        start = 0
        rng = self.headers.get("Range")
        if rng: start = int(rng.split("=")[1].split("-")[0])
        part = data[start:]
        self.send_response(206 if rng else 200); self.send_header("Content-Length", str(len(part))); self.end_headers()
        for i in range(0, len(part), 65536):
            self.wfile.write(part[i:i+65536])
            if name.startswith("slow"): time.sleep(0.05)
HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
"#;

    static ENV: Mutex<()> = Mutex::new(());

    fn wait(downloads: &Downloads, id: u64) -> (String, String) {
        for _ in 0..400 {
            let v = downloads.view();
            let job = v
                .as_array()
                .unwrap()
                .iter()
                .find(|j| j["id"] == id)
                .unwrap()
                .clone();
            if job["state"] != "running" {
                return (
                    job["state"].as_str().unwrap().to_string(),
                    job["message"].as_str().unwrap().to_string(),
                );
            }
            std::thread::sleep(Duration::from_millis(50));
        }
        panic!("download did not end");
    }

    #[test]
    fn a_model_is_fetched_checked_resumed_and_cancelled() {
        let _turn = ENV.lock().unwrap_or_else(|e| e.into_inner());
        if std::process::Command::new("python3")
            .arg("--version")
            .output()
            .is_err()
        {
            return;
        }
        let port = std::net::TcpListener::bind("127.0.0.1:0")
            .unwrap()
            .local_addr()
            .unwrap()
            .port();
        let mut stub = std::process::Command::new("python3")
            .args(["-c", HUB, &port.to_string()])
            .spawn()
            .unwrap();
        for _ in 0..100 {
            if std::net::TcpStream::connect(("127.0.0.1", port)).is_ok() {
                break;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
        std::env::set_var("HELIX_MODEL_HUB", format!("http://127.0.0.1:{port}"));
        let dir = std::env::temp_dir().join(format!("helix-downloads-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let downloads = Downloads::default();
        let files = list("o/r").unwrap();
        assert!(files.iter().any(|f| f.path == "m-Q4_K_M.gguf"));

        // Half of the model arrived on an earlier try: it continues from there, and with its projector named after it.
        let whole = (0..=255u8).collect::<Vec<u8>>().repeat(4000);
        std::fs::create_dir_all(dir.join(".downloads")).unwrap();
        std::fs::write(dir.join(".downloads/m-Q4_K_M.gguf.part"), &whole[..400_000]).unwrap();
        let id = downloads
            .start("o/r", plan(&files, "m-Q4_K_M.gguf", true).unwrap(), &dir)
            .unwrap();
        assert_eq!(wait(&downloads, id).0, "done");
        assert_eq!(std::fs::read(dir.join("m-Q4_K_M.gguf")).unwrap(), whole);
        assert!(
            dir.join("mmproj-m-f16.gguf").exists(),
            "the repository's name already pairs"
        );
        assert!(!dir.join(".downloads/m-Q4_K_M.gguf.part").exists());

        // A file that does not match its checksum is thrown away, not installed.
        let id = downloads
            .start("o/r", plan(&files, "bad-Q4_K_M.gguf", false).unwrap(), &dir)
            .unwrap();
        let (state, message) = wait(&downloads, id);
        assert_eq!(state, "failed");
        assert!(message.contains("intact"), "{message}");
        assert!(
            !dir.join("bad-Q4_K_M.gguf").exists()
                && !dir.join(".downloads/bad-Q4_K_M.gguf.part").exists()
        );

        // Cancelling stops it and keeps what arrived for next time.
        let id = downloads
            .start(
                "o/r",
                plan(&files, "slow-Q4_K_M.gguf", false).unwrap(),
                &dir,
            )
            .unwrap();
        std::thread::sleep(Duration::from_millis(400));
        assert!(downloads.cancel(id));
        assert_eq!(wait(&downloads, id).0, "cancelled");
        assert!(!dir.join("slow-Q4_K_M.gguf").exists());
        assert!(
            std::fs::metadata(dir.join(".downloads/slow-Q4_K_M.gguf.part"))
                .unwrap()
                .len()
                > 0
        );
        // Started again, it finishes.
        let id = downloads
            .start(
                "o/r",
                plan(&files, "slow-Q4_K_M.gguf", false).unwrap(),
                &dir,
            )
            .unwrap();
        assert_eq!(wait(&downloads, id).0, "done");
        assert_eq!(
            std::fs::metadata(dir.join("slow-Q4_K_M.gguf"))
                .unwrap()
                .len(),
            3_000_000
        );

        // A repository that is not there says so.
        assert!(list("o/missing").unwrap_err().contains("not found"));
        let _ = stub.kill();
        let _ = stub.wait();
        std::env::remove_var("HELIX_MODEL_HUB");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
