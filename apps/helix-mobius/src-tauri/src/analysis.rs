//! Static ELF analysis in an isolated REA/Ghidra process. No Agent Surface grants.
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::io::{Read, Write};
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tauri::{AppHandle, Emitter, Manager};
use tokio::io::{AsyncBufReadExt, AsyncReadExt, BufReader};
use tokio::sync::watch;

const MAX_TARGET: u64 = 64 * 1024 * 1024;
const MAX_OUTPUT: u64 = 24 * 1024 * 1024;
const WORKER: &str = include_str!("../analysis-worker.mjs");

#[derive(Clone, Deserialize)]
pub struct Config {
    node: PathBuf,
    packages: PathBuf,
    ghidra: PathBuf,
    java_home: PathBuf,
}

#[derive(Clone, Default, Serialize, Deserialize)]
pub struct Job {
    pub id: String,
    pub target: String,
    pub procedure: String,
    pub sha256: String,
    pub state: String,
    pub stage: String,
    pub started: u64,
    pub error: String,
    pub records: Vec<Value>,
    pub server: Value,
}

#[derive(Default)]
pub struct Hub {
    job: Mutex<Job>,
    cancel: Mutex<Option<watch::Sender<bool>>>,
}

#[derive(Serialize)]
pub struct Readiness {
    pub ready: bool,
    pub detail: String,
}

fn tools_dir() -> PathBuf {
    crate::data_home().join("helix/analysis-tools")
}

fn jobs_dir() -> PathBuf {
    crate::data_home().join("helix/mobius/analysis")
}

fn config() -> Result<Config, String> {
    let bytes = std::fs::read(tools_dir().join("config.json"))
        .map_err(|_| "Analysis tools are not installed. Run python3 tools/scripts/install_analysis.py from the Helix project.".to_string())?;
    let config: Config = serde_json::from_slice(&bytes)
        .map_err(|e| format!("Invalid analysis configuration: {e}"))?;
    for (path, label) in [
        (config.node.clone(), "Node"),
        (
            config
                .packages
                .join("node_modules/rea-agents/scripts/rea.mjs"),
            "REA",
        ),
        (config.ghidra.join("support/analyzeHeadless"), "Ghidra"),
        (config.java_home.join("bin/javac"), "a full JDK"),
        (PathBuf::from("/usr/bin/bwrap"), "Bubblewrap"),
    ] {
        if !path.is_file() {
            return Err(format!("Missing {label}: {}", path.display()));
        }
    }
    let package: Value = serde_json::from_slice(
        &std::fs::read(config.packages.join("node_modules/rea-agents/package.json"))
            .map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())?;
    if package["version"] != "6.4.0" {
        return Err("This integration requires REA 6.4.0. Re-run the analysis installer.".into());
    }
    Ok(config)
}

pub fn readiness() -> Readiness {
    match config() {
        Ok(_) => Readiness {
            ready: true,
            detail: "REA 6.4.0 · Ghidra · local static analysis".into(),
        },
        Err(detail) => Readiness {
            ready: false,
            detail,
        },
    }
}

pub fn view(app: &AppHandle) -> Job {
    app.state::<Arc<Hub>>()
        .job
        .lock()
        .expect("analysis job")
        .clone()
}

pub fn load(app: &AppHandle, id: &str) -> Result<Job, String> {
    if !helix_chat::chat::valid_id(id) {
        return Err("Invalid analysis job.".into());
    }
    let hub = app.state::<Arc<Hub>>();
    let cancellation = hub.cancel.lock().expect("analysis cancel");
    if cancellation.is_some() {
        let current = view(app);
        return if current.id == id {
            Ok(current)
        } else {
            Err("Finish or cancel the current analysis first.".into())
        };
    }
    let file = std::fs::File::open(jobs_dir().join(id).join("evidence.json"))
        .map_err(|_| "Saved analysis evidence could not be opened.")?;
    let mut bytes = Vec::new();
    file.take(MAX_OUTPUT + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() as u64 > MAX_OUTPUT {
        return Err("Saved evidence exceeds its size limit.".into());
    }
    let job: Job =
        serde_json::from_slice(&bytes).map_err(|_| "Saved analysis evidence is invalid.")?;
    if job.id != id {
        return Err("Saved evidence does not match this job.".into());
    }
    *hub.job.lock().expect("analysis job") = job.clone();
    Ok(job)
}

fn publish(app: &AppHandle, change: impl FnOnce(&mut Job)) {
    let hub = app.state::<Arc<Hub>>();
    let snapshot = {
        let mut job = hub.job.lock().expect("analysis job");
        change(&mut job);
        job.clone()
    };
    let _ = app.emit("analysis-view", &snapshot);
}

pub fn cancel(app: &AppHandle) {
    let hub = app.state::<Arc<Hub>>();
    let cancellation = hub.cancel.lock().expect("analysis cancel");
    if let Some(sender) = cancellation.as_ref() {
        let _ = sender.send(true);
        publish(app, |job| {
            job.state = "cancelling".into();
            job.stage = "Stopping analysis".into();
        });
    }
}

fn private_dir(path: &Path) -> Result<(), String> {
    std::fs::DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(path)
        .map_err(|e| e.to_string())
}

fn private_write(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(path)
        .map_err(|e| e.to_string())?;
    file.write_all(bytes).map_err(|e| e.to_string())
}

/// Open a single native picker, independent from the text attachment picker.
pub fn pick() -> Result<Option<String>, String> {
    for (command, args) in [
        (
            "zenity",
            vec![
                "--file-selection",
                "--title=Analyze a Linux binary",
                "--file-filter=All files | *",
            ],
        ),
        (
            "kdialog",
            vec![
                "--getopenfilename",
                ".",
                "*",
                "--title",
                "Analyze a Linux binary",
            ],
        ),
    ] {
        if let Ok(output) = std::process::Command::new(command).args(args).output() {
            if output.status.code() == Some(1) {
                return Ok(None);
            }
            if output.status.success() {
                let path = String::from_utf8(output.stdout)
                    .map_err(|_| "The selected path is not UTF-8.")?;
                let path = path.trim_end_matches(['\r', '\n']);
                return Ok((!path.is_empty()).then(|| path.to_string()));
            }
        }
    }
    Err("No file picker is available. Install zenity or kdialog, or enter a file path.".into())
}

fn prepare(dir: &Path, path: &str, procedure: &str) -> Result<String, String> {
    let path = Path::new(path);
    if !path.is_absolute() {
        return Err("Choose an absolute file path.".into());
    }
    if !std::fs::metadata(path)
        .map_err(|e| e.to_string())?
        .is_file()
    {
        return Err("Select a regular Linux ELF file.".into());
    }
    let source = std::fs::OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NONBLOCK)
        .open(path)
        .map_err(|e| format!("Could not open the selected binary: {e}"))?;
    let metadata = source.metadata().map_err(|e| e.to_string())?;
    if !metadata.is_file() || metadata.len() > MAX_TARGET {
        return Err("Select a regular Linux ELF file up to 64 MiB.".into());
    }
    let mut bytes = Vec::new();
    source
        .take(MAX_TARGET + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    validate_elf(&bytes)?;
    private_dir(&dir.join("input"))?;
    private_dir(&dir.join("work"))?;
    private_write(&dir.join("input/target"), &bytes)?;
    private_write(&dir.join("worker.mjs"), WORKER.as_bytes())?;
    private_write(
        &dir.join("selection.json"),
        &serde_json::to_vec(&json!({"path": "/input/target", "procedure": procedure}))
            .map_err(|e| e.to_string())?,
    )?;
    Ok(format!("{:x}", Sha256::digest(&bytes)))
}

fn validate_elf(bytes: &[u8]) -> Result<(), String> {
    if bytes.len() as u64 > MAX_TARGET {
        return Err("The binary exceeds 64 MiB.".into());
    }
    if bytes.len() < 52
        || &bytes[..4] != b"\x7fELF"
        || !matches!(bytes[4], 1 | 2)
        || !matches!(bytes[5], 1 | 2)
        || bytes[6] != 1
    {
        return Err("This first version analyzes Linux ELF binaries. The selected file is not an ELF binary.".into());
    }
    if bytes[4] == 2 && bytes.len() < 64 {
        return Err("The ELF header is incomplete.".into());
    }
    Ok(())
}

fn sandbox(config: &Config, dir: &Path) -> tokio::process::Command {
    let mut command = tokio::process::Command::new("/usr/bin/bwrap");
    command.args([
        "--unshare-all",
        "--die-with-parent",
        "--new-session",
        "--clearenv",
    ]);
    for path in ["/usr", "/bin", "/lib", "/lib64"] {
        if Path::new(path).exists() {
            command.args(["--ro-bind", path, path]);
        }
    }
    // Libraries and Java need these host files, not the rest of /etc or the home directory.
    for path in ["/etc/ld.so.cache", "/etc/localtime"] {
        if Path::new(path).exists() {
            command.args(["--ro-bind", path, path]);
        }
    }
    for (source, destination) in [
        (config.node.clone(), "/runtime/node"),
        (config.packages.clone(), "/tools"),
        (config.ghidra.clone(), "/ghidra"),
        (config.java_home.clone(), "/jdk"),
        (dir.join("input"), "/input"),
        (dir.join("worker.mjs"), "/worker.mjs"),
        (dir.join("selection.json"), "/selection.json"),
    ] {
        command.arg("--ro-bind").arg(source).arg(destination);
    }
    command.arg("--bind").arg(dir.join("work")).arg("/job");
    command.args([
        "--proc",
        "/proc",
        "--dev",
        "/dev",
        "--tmpfs",
        "/tmp",
        "--dir",
        "/home/analysis",
        "--setenv",
        "HOME",
        "/home/analysis",
        "--setenv",
        "XDG_CACHE_HOME",
        "/job/cache",
        "--setenv",
        "PATH",
        "/jdk/bin:/usr/bin:/bin",
        "--setenv",
        "JAVA_HOME",
        "/jdk",
        "--setenv",
        "GHIDRA_INSTALL_DIR",
        "/ghidra",
        "--setenv",
        "TMPDIR",
        "/tmp",
        "--setenv",
        "_JAVA_OPTIONS",
        "-Xmx1536m -XX:ActiveProcessorCount=2",
        "--chdir",
        "/job",
        "/runtime/node",
        "--max-old-space-size=768",
        "/worker.mjs",
        "/selection.json",
    ]);
    command
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true);
    command
}

/// Return only after the namespace launcher is reaped; killing its init also kills owned descendants.
async fn run(
    config: &Config,
    dir: &Path,
    mut cancelled: watch::Receiver<bool>,
    mut event: impl FnMut(Value),
) -> Result<(), String> {
    let mut child = sandbox(config, dir)
        .spawn()
        .map_err(|e| format!("Could not start the analysis sandbox: {e}"))?;
    let stdout = child.stdout.take().ok_or("Missing analysis output")?;
    let mut stderr = child.stderr.take().ok_or("Missing analysis diagnostics")?;
    let diagnostics = tokio::spawn(async move {
        let mut retained = Vec::new();
        let mut buffer = [0u8; 4096];
        while let Ok(count) = stderr.read(&mut buffer).await {
            if count == 0 {
                break;
            }
            let keep = count.min((64 * 1024usize).saturating_sub(retained.len()));
            retained.extend_from_slice(&buffer[..keep]);
        }
        String::from_utf8_lossy(&retained).to_string()
    });
    let outcome = async {
        let mut lines = BufReader::new(stdout.take(MAX_OUTPUT + 1)).lines();
        let mut total = 0u64;
        let mut complete = false;
        while let Some(line) = lines.next_line().await.map_err(|e| e.to_string())? {
            total += line.len() as u64 + 1;
            if total > MAX_OUTPUT || line.len() > 5 * 1024 * 1024 {
                return Err("Analysis output exceeded its size limit.".into());
            }
            let value: Value =
                serde_json::from_str(&line).map_err(|_| "Analysis returned invalid output.")?;
            match value["kind"].as_str() {
                Some("complete") => complete = true,
                Some("failed") => {
                    return Err(value["error"]
                        .as_str()
                        .unwrap_or("Analysis failed.")
                        .to_string())
                }
                Some("stage" | "evidence" | "engine") => event(value),
                _ => return Err("Analysis returned an unknown event.".into()),
            }
        }
        if !complete {
            return Err("The analysis worker exited before completing.".into());
        }
        Ok(())
    };
    let outcome = tokio::select! {
        result = outcome => result,
        _ = cancelled.changed() => Err("cancelled".into()),
        _ = tokio::time::sleep(Duration::from_secs(600)) => Err("Analysis exceeded its ten-minute deadline.".into()),
    };
    let _ = child.start_kill();
    child
        .wait()
        .await
        .map_err(|e| format!("Could not reap analysis sandbox: {e}"))?;
    let diagnostic = diagnostics.await.unwrap_or_default();
    outcome.map_err(|reason| {
        if reason == "cancelled" || diagnostic.is_empty() {
            reason
        } else {
            format!(
                "{reason}\n{}",
                diagnostic.chars().take(6000).collect::<String>()
            )
        }
    })
}

pub fn start(app: AppHandle, path: String, procedure: String) -> Result<Job, String> {
    let config = config()?;
    let procedure = procedure.trim().to_string();
    if procedure.chars().count() > 160 || procedure.chars().any(char::is_control) {
        return Err("Use a function name or address up to 160 characters.".into());
    }
    let hub = app.state::<Arc<Hub>>();
    let (sender, receiver) = watch::channel(false);
    let mut cancellation = hub.cancel.lock().expect("analysis cancel");
    if cancellation.is_some() {
        return Err("An analysis is already running. Cancel it before starting another.".into());
    }
    let job = Job {
        id: crate::new_id(),
        target: path.clone(),
        procedure: procedure.clone(),
        state: "preparing".into(),
        stage: "Preparing a private copy".into(),
        started: crate::now(),
        ..Job::default()
    };
    *hub.job.lock().expect("analysis job") = job.clone();
    *cancellation = Some(sender);
    drop(cancellation);
    let dir = jobs_dir().join(&job.id);
    tauri::async_runtime::spawn(async move {
        let preparation_dir = dir.clone();
        let digest =
            tokio::task::spawn_blocking(move || prepare(&preparation_dir, &path, &procedure))
                .await
                .map_err(|e| e.to_string())
                .and_then(|v| v);
        let outcome = match digest {
            Ok(digest) if !*receiver.borrow() => {
                publish(&app, |job| {
                    job.sha256 = digest;
                    job.state = "analyzing".into();
                });
                run(&config, &dir, receiver, |value| {
                    publish(&app, |job| match value["kind"].as_str() {
                        Some("stage") => {
                            job.stage = value["stage"].as_str().unwrap_or_default().to_string()
                        }
                        Some("evidence") => job.records.push(value["record"].clone()),
                        Some("engine") => job.server = value["server"].clone(),
                        _ => {}
                    })
                })
                .await
            }
            Ok(_) => Err("cancelled".into()),
            Err(error) => Err(error),
        };
        let mut snapshot = view(&app);
        match outcome {
            Ok(()) => {
                snapshot.state = "ready".into();
                snapshot.stage = "Analysis complete".into();
            }
            Err(error) if error == "cancelled" => {
                snapshot.state = "cancelled".into();
                snapshot.stage = "Analysis cancelled".into();
            }
            Err(error) => {
                snapshot.state = "failed".into();
                snapshot.stage = "Analysis failed".into();
                snapshot.error = error;
            }
        }
        let result = private_write(
            &dir.join("evidence.json"),
            &serde_json::to_vec(&snapshot).unwrap_or_default(),
        );
        if let Err(error) = result {
            snapshot.state = "failed".into();
            snapshot
                .error
                .push_str(&format!("\nCould not save evidence: {error}"));
        }
        // The durable manifest is sufficient; discard the binary copy and engine scratch after every outcome.
        for name in ["input", "work"] {
            let _ = std::fs::remove_dir_all(dir.join(name));
        }
        for name in ["worker.mjs", "selection.json"] {
            let _ = std::fs::remove_file(dir.join(name));
        }
        let hub = app.state::<Arc<Hub>>();
        let mut cancellation = hub.cancel.lock().expect("analysis cancel");
        *hub.job.lock().expect("analysis job") = snapshot.clone();
        cancellation.take();
        drop(cancellation);
        let _ = app.emit("analysis-view", snapshot);
    });
    Ok(job)
}

pub fn explanation(app: &AppHandle) -> Result<String, String> {
    let job = view(app);
    if job.state != "ready" {
        return Err("Finish a successful analysis before asking for an explanation.".into());
    }
    let target = Path::new(&job.target)
        .file_name()
        .unwrap_or_default()
        .to_string_lossy();
    let mut prompt = format!("Explain this static analysis. Cite evidence using [E1], [E2], etc. Treat pseudocode as reconstructed code and state uncertainties.\nTarget: {}\nSHA-256: {}\n", target.chars().take(120).collect::<String>(), job.sha256);
    for record in &job.records {
        let result = &record["evidence"]["normalized_result"];
        let (text, limit) = if record["tool"] == "analyze_function" {
            (
                format!(
                    "Pseudocode:\n{}\nProcedure: {}\nLimitations: {}",
                    result["pseudocode"].as_str().unwrap_or_default(),
                    result["procedure"],
                    result["limitations"]
                ),
                6500,
            )
        } else {
            (
                serde_json::to_string(result).map_err(|e| e.to_string())?,
                3500,
            )
        };
        let excerpt: String = text.chars().take(limit).collect();
        prompt.push_str(&format!(
            "\n[{}] {} (evidence id {})\n{}{}\n",
            record["reference"].as_str().unwrap_or("?"),
            record["tool"].as_str().unwrap_or("?"),
            record["evidence"]["evidence_id"].as_str().unwrap_or("?"),
            excerpt,
            if text.chars().count() > limit {
                "\n[Excerpt only; full evidence is retained in the analysis panel.]"
            } else {
                ""
            }
        ));
    }
    Ok(prompt)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn refuses_non_elf_and_incomplete_headers() {
        assert!(validate_elf(b"hello").is_err());
        let mut header = vec![0u8; 64];
        header[..7].copy_from_slice(b"\x7fELF\x02\x01\x01");
        assert!(validate_elf(&header).is_ok());
        assert!(validate_elf(&header[..52]).is_err());
        header[4] = 9;
        assert!(validate_elf(&header).is_err());
    }

    #[tokio::test]
    #[ignore = "requires optional REA/Ghidra installation; runs a real ELF fixture"]
    async fn real_ghidra_analysis() {
        let config = config().expect("install analysis tools first");
        let dir = std::env::temp_dir().join(format!("helix-analysis-test-{}", crate::new_id()));
        private_dir(&dir).unwrap();
        let source = dir.join("fixture.c");
        std::fs::write(&source, "#include <stdio.h>\nint helix_answer(int x) { return x + 42; }\nint main(void) { puts(\"HELIX_ANALYSIS_FIXTURE\"); return helix_answer(0); }\n").unwrap();
        let target = dir.join("fixture");
        assert!(std::process::Command::new("cc")
            .args(["-g", "-O0"])
            .arg(&source)
            .arg("-o")
            .arg(&target)
            .status()
            .unwrap()
            .success());
        prepare(&dir, target.to_str().unwrap(), "helix_answer").unwrap();
        let (_cancel, receiver) = watch::channel(false);
        let mut records = Vec::new();
        run(&config, &dir, receiver, |event| {
            if event["kind"] == "evidence" {
                records.push(event["record"].clone());
            }
        })
        .await
        .unwrap();
        assert!(records
            .iter()
            .any(|record| record["tool"] == "analyze_function"
                && record["evidence"]["normalized_result"]["pseudocode"]
                    .as_str()
                    .is_some_and(|code| code.contains("x + 42") || code.contains("x + 0x2a"))));
        assert!(records.iter().any(|record| record["tool"] == "list_strings"
            && record.to_string().contains("HELIX_ANALYSIS_FIXTURE")));
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[tokio::test]
    #[ignore = "requires optional analysis tools"]
    async fn cancellation_reaps_the_sandbox() {
        let config = config().unwrap();
        let dir = std::env::temp_dir().join(format!("helix-analysis-cancel-{}", crate::new_id()));
        prepare(&dir, "/usr/bin/true", "").unwrap();
        let (sender, receiver) = watch::channel(false);
        tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(500)).await;
            sender.send(true).unwrap();
        });
        assert_eq!(
            run(&config, &dir, receiver, |_| {}).await.unwrap_err(),
            "cancelled"
        );
        let needle = dir.to_string_lossy();
        for process in std::fs::read_dir("/proc").unwrap().flatten() {
            if let Ok(command) = std::fs::read(process.path().join("cmdline")) {
                assert!(
                    !String::from_utf8_lossy(&command).contains(needle.as_ref()),
                    "analysis process was left running"
                );
            }
        }
        std::fs::remove_dir_all(dir).unwrap();
    }
}
