//! The model library: what is installed, what can be downloaded, and the downloads themselves.
//!
//! A download goes to `<name>.part` in the models folder, is checked against the checksum written in the catalog, and only then takes its
//! real name; anything that does not match is deleted. The service lists models by their names in that folder, so a half-written file
//! is never offered. Only files from the catalog are downloaded, and only from its fixed host.

use std::collections::HashMap;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use helix_chat::model_catalog::{self, CatalogModel};
use serde::Serialize;
use sha2::{Digest, Sha256};

pub fn models_dir() -> PathBuf {
    if let Some(dir) = std::env::var_os("HELIX_MODEL_DIR").filter(|v| !v.is_empty()) {
        return PathBuf::from(dir);
    }
    let base = std::env::var_os("XDG_DATA_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share")))
        .unwrap_or_else(|| PathBuf::from("."));
    base.join("helix/models")
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum Phase {
    Downloading,
    Verifying,
    Failed,
}

#[derive(Clone, Debug, Serialize)]
pub struct Progress {
    pub phase: Phase,
    pub done: u64,
    pub total: u64,
    pub error: String,
}

struct Job {
    progress: Progress,
    cancel: Arc<AtomicBool>,
}

#[derive(Default)]
pub struct Downloads {
    jobs: Mutex<HashMap<String, Job>>,
}

#[derive(Clone, Debug, Serialize)]
pub struct Card {
    /// The catalog id, or the file name for a model that was put there by hand.
    pub id: String,
    /// The name the inference service lists it under: what a route stores.
    pub file: String,
    pub title: String,
    pub maker: String,
    pub blurb: String,
    pub tasks: Vec<String>,
    pub vision: bool,
    pub license: String,
    pub bytes: u64,
    pub memory_gb: f32,
    pub installed: bool,
    pub loaded: bool,
    pub in_catalog: bool,
    pub progress: Option<Progress>,
}

fn installed_size(dir: &Path, file: &str) -> Option<u64> {
    std::fs::metadata(dir.join(file))
        .ok()
        .filter(|m| m.is_file())
        .map(|m| m.len())
}

fn complete(dir: &Path, model: &CatalogModel) -> bool {
    model
        .files
        .iter()
        .all(|f| installed_size(dir, f.name) == Some(f.bytes))
}

/// Models in the folder that the catalog does not know: listed by file (a sharded model once, by its first shard).
fn unknown_models(dir: &Path) -> Vec<(String, u64)> {
    let mut out = Vec::new();
    let Ok(entries) = std::fs::read_dir(dir) else {
        return out;
    };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().to_string();
        if !model_catalog::safe_file_name(&name) || name.starts_with("mmproj-") {
            continue;
        }
        if model_catalog::CATALOG
            .iter()
            .any(|m| m.files.iter().any(|f| f.name == name))
        {
            continue;
        }
        if name.contains("-of-") && !name.contains("-00001-of-") {
            continue;
        }
        let bytes = entry.metadata().map(|m| m.len()).unwrap_or(0);
        out.push((name, bytes));
    }
    out.sort();
    out
}

pub fn list(downloads: &Downloads, loaded: &[String]) -> Vec<Card> {
    let dir = models_dir();
    let jobs = downloads.jobs.lock().expect("downloads");
    let mut cards: Vec<Card> = model_catalog::CATALOG
        .iter()
        .map(|m| Card {
            id: m.id.to_string(),
            file: m.model_file().to_string(),
            title: m.title.to_string(),
            maker: m.maker.to_string(),
            blurb: m.blurb.to_string(),
            tasks: m.tasks.iter().map(|t| t.to_string()).collect(),
            vision: m.vision,
            license: m.license.to_string(),
            bytes: m.bytes(),
            memory_gb: m.memory_gb,
            installed: complete(&dir, m),
            loaded: loaded.iter().any(|l| l == m.model_file()),
            in_catalog: true,
            progress: jobs.get(m.id).map(|j| j.progress.clone()),
        })
        .collect();
    for (name, bytes) in unknown_models(&dir) {
        let stem = name.trim_end_matches(".gguf").to_string();
        cards.push(Card {
            id: name.clone(),
            file: name.clone(),
            title: stem,
            maker: String::new(),
            blurb: "Added by hand to the models folder.".into(),
            tasks: vec!["chat".into()],
            vision: false,
            license: String::new(),
            bytes,
            memory_gb: 0.0,
            installed: true,
            loaded: loaded.contains(&name),
            in_catalog: false,
            progress: None,
        });
    }
    cards
}

fn set_progress(downloads: &Downloads, id: &str, progress: Progress) {
    if let Some(job) = downloads.jobs.lock().expect("downloads").get_mut(id) {
        job.progress = progress;
    }
}

fn sha256_of(path: &Path, cancel: &AtomicBool) -> Result<String, String> {
    let mut file = std::fs::File::open(path).map_err(|e| e.to_string())?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0u8; 1 << 20];
    loop {
        if cancel.load(Ordering::Relaxed) {
            return Err("Cancelled.".into());
        }
        let n = file.read(&mut buffer).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        hasher.update(&buffer[..n]);
    }
    Ok(hasher
        .finalize()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect())
}

/// Starts the download of a catalog model. Returns an error if it is unknown, already installed, or already running.
pub fn start(
    downloads: &Arc<Downloads>,
    id: &str,
    notify: impl Fn() + Send + Sync + 'static,
) -> Result<(), String> {
    let model = model_catalog::find(id).ok_or("That model is not in the library.")?;
    let dir = models_dir();
    if complete(&dir, model) {
        return Err("Already installed.".into());
    }
    let cancel = Arc::new(AtomicBool::new(false));
    {
        let mut jobs = downloads.jobs.lock().expect("downloads");
        if jobs
            .get(id)
            .is_some_and(|j| j.progress.phase != Phase::Failed)
        {
            return Err("Already downloading.".into());
        }
        jobs.insert(
            id.to_string(),
            Job {
                progress: Progress {
                    phase: Phase::Downloading,
                    done: 0,
                    total: model.bytes(),
                    error: String::new(),
                },
                cancel: cancel.clone(),
            },
        );
    }
    let downloads = downloads.clone();
    let id = id.to_string();
    tauri::async_runtime::spawn(async move {
        let result = run(&downloads, model, &dir, &cancel, &notify).await;
        match result {
            Ok(()) => {
                downloads.jobs.lock().expect("downloads").remove(&id);
            }
            Err(error) if cancel.load(Ordering::Relaxed) => {
                let _ = error;
                downloads.jobs.lock().expect("downloads").remove(&id);
            }
            Err(error) => set_progress(
                &downloads,
                &id,
                Progress {
                    phase: Phase::Failed,
                    done: 0,
                    total: model.bytes(),
                    error,
                },
            ),
        }
        notify();
    });
    Ok(())
}

async fn run(
    downloads: &Downloads,
    model: &'static CatalogModel,
    dir: &Path,
    cancel: &AtomicBool,
    notify: &(impl Fn() + Send + Sync),
) -> Result<(), String> {
    std::fs::create_dir_all(dir).map_err(|_| "Could not create the models folder.".to_string())?;
    let total = model.bytes();
    let mut finished: u64 = 0;
    let client = reqwest::Client::builder()
        .redirect(reqwest::redirect::Policy::limited(5))
        .connect_timeout(std::time::Duration::from_secs(20))
        .build()
        .map_err(|e| e.to_string())?;
    for file in model.files {
        let target = dir.join(file.name);
        if installed_size(dir, file.name) == Some(file.bytes) {
            finished += file.bytes;
            continue;
        }
        let part = dir.join(format!("{}.part", file.name));
        if let Some(parent) = part.parent() {
            std::fs::create_dir_all(parent)
                .map_err(|_| "Could not create the models folder.".to_string())?;
        }
        let mut have = std::fs::metadata(&part).map(|m| m.len()).unwrap_or(0);
        if have > file.bytes {
            let _ = std::fs::remove_file(&part);
            have = 0;
        }
        if have < file.bytes {
            let mut request = client.get(file.url);
            if have > 0 {
                request = request.header("Range", format!("bytes={have}-"));
            }
            let mut response = request
                .send()
                .await
                .map_err(|_| "Could not reach the download server.".to_string())?;
            if have > 0 && response.status().as_u16() != 206 {
                // The server did not honour the range: start again rather than append the wrong bytes.
                let _ = std::fs::remove_file(&part);
                have = 0;
                response = client
                    .get(file.url)
                    .send()
                    .await
                    .map_err(|_| "Could not reach the download server.".to_string())?;
            }
            if !response.status().is_success() {
                return Err(format!(
                    "The download server refused the request ({}).",
                    response.status().as_u16()
                ));
            }
            let mut out = std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(&part)
                .map_err(|_| "Could not write to the models folder.".to_string())?;
            let mut done = have;
            let mut last_note = std::time::Instant::now();
            while let Some(chunk) = response
                .chunk()
                .await
                .map_err(|_| "The download was interrupted. Try again to resume.".to_string())?
            {
                if cancel.load(Ordering::Relaxed) {
                    return Err("Cancelled.".into());
                }
                if done + chunk.len() as u64 > file.bytes {
                    let _ = std::fs::remove_file(&part);
                    return Err("The file is larger than expected; it was discarded.".into());
                }
                out.write_all(&chunk).map_err(|e| {
                    if e.raw_os_error() == Some(28) {
                        "There is not enough disk space.".to_string()
                    } else {
                        "Could not write to the models folder.".to_string()
                    }
                })?;
                done += chunk.len() as u64;
                if last_note.elapsed() >= std::time::Duration::from_millis(250) {
                    last_note = std::time::Instant::now();
                    set_progress(
                        downloads,
                        model.id,
                        Progress {
                            phase: Phase::Downloading,
                            done: finished + done,
                            total,
                            error: String::new(),
                        },
                    );
                    notify();
                }
            }
            out.flush()
                .map_err(|_| "Could not write to the models folder.".to_string())?;
            if done != file.bytes {
                return Err("The download ended early. Try again to resume.".into());
            }
        }
        set_progress(
            downloads,
            model.id,
            Progress {
                phase: Phase::Verifying,
                done: finished + file.bytes,
                total,
                error: String::new(),
            },
        );
        notify();
        let part_for_hash = part.clone();
        let flag = downloads
            .jobs
            .lock()
            .expect("downloads")
            .get(model.id)
            .map(|j| j.cancel.clone())
            .unwrap_or_default();
        let sum = tauri::async_runtime::spawn_blocking(move || sha256_of(&part_for_hash, &flag))
            .await
            .map_err(|_| "Checking the file failed.".to_string())??;
        if sum != file.sha256 {
            let _ = std::fs::remove_file(&part);
            return Err("The file did not match its checksum and was discarded.".into());
        }
        std::fs::rename(&part, &target)
            .map_err(|_| "Could not finish saving the model.".to_string())?;
        finished += file.bytes;
    }
    Ok(())
}

pub fn cancel(downloads: &Downloads, id: &str) {
    if let Some(job) = downloads.jobs.lock().expect("downloads").get(id) {
        job.cancel.store(true, Ordering::Relaxed);
    }
}

/// Removes a catalog model's files (and any half-finished download of it).
pub fn remove(id: &str) -> Result<(), String> {
    let model =
        model_catalog::find(id).ok_or("Only models from the library can be removed here.")?;
    let dir = models_dir();
    for file in model.files {
        let _ = std::fs::remove_file(dir.join(file.name));
        let _ = std::fs::remove_file(dir.join(format!("{}.part", file.name)));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lists_the_catalog_and_what_was_added_by_hand() {
        let dir = std::env::temp_dir().join(format!("helix-library-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("my-own-model-q4.gguf"), b"12345").unwrap();
        std::fs::write(dir.join("my-own-model-q4.gguf.part"), b"1").unwrap();
        std::fs::write(dir.join("mmproj-my-own-model.gguf"), b"1").unwrap();
        std::env::set_var("HELIX_MODEL_DIR", &dir);
        let cards = list(&Downloads::default(), &["my-own-model-q4.gguf".to_string()]);
        let own = cards
            .iter()
            .find(|c| !c.in_catalog)
            .expect("the hand-added model is listed");
        assert_eq!(own.file, "my-own-model-q4.gguf");
        assert!(own.installed && own.loaded);
        assert_eq!(
            cards.iter().filter(|c| !c.in_catalog).count(),
            1,
            "partials and projectors are not models"
        );
        assert!(cards.iter().filter(|c| c.in_catalog).all(|c| !c.installed));
        std::env::remove_var("HELIX_MODEL_DIR");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
