//! Agent Surface desktop capture via niri screenshots.

use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::sync::mpsc;
use std::time::Duration;

use helix_compositor::capture::{pick_agent_desk_window, resolve_scope, CaptureScope};
use serde_json::{json, Value};
use tauri::AppHandle;

use crate::dock::niri_action;
use crate::shell_state;

fn socket_path() -> Option<PathBuf> {
    std::env::var_os("NIRI_SOCKET")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
}

fn runtime_dir() -> Option<PathBuf> {
    std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
}

fn capture_file() -> Result<PathBuf, String> {
    let dir = runtime_dir().ok_or("no runtime directory")?;
    let name = format!(
        "helix-capture-{}.png",
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0)
    );
    Ok(dir.join(name))
}

struct ScreenshotWaiter {
    rx: mpsc::Receiver<Option<String>>,
}

fn spawn_screenshot_waiter() -> ScreenshotWaiter {
    let (tx, rx) = mpsc::channel();
    std::thread::spawn(move || {
        let path = match socket_path() {
            Some(p) => p,
            None => {
                let _ = tx.send(None);
                return;
            }
        };
        let Ok(mut stream) = UnixStream::connect(path) else {
            let _ = tx.send(None);
            return;
        };
        let _ = stream.set_read_timeout(Some(Duration::from_secs(20)));
        if stream.write_all(b"\"EventStream\"\n").is_err() {
            let _ = tx.send(None);
            return;
        }
        for line in BufReader::new(stream).lines().map_while(Result::ok) {
            let Ok(event) = serde_json::from_str::<Value>(&line) else {
                continue;
            };
            if let Some(body) = event.get("ScreenshotCaptured") {
                let saved = body.get("path").and_then(Value::as_str).map(String::from);
                let _ = tx.send(saved);
                return;
            }
        }
        let _ = tx.send(None);
    });
    ScreenshotWaiter { rx }
}

impl ScreenshotWaiter {
    fn wait(self, timeout: Duration) -> Result<Option<String>, String> {
        self.rx
            .recv_timeout(timeout)
            .map_err(|_| "timed out waiting for a screenshot".to_string())
    }
}

fn shoot(action: Value, path: &Path) -> Result<(), String> {
    let waiter = spawn_screenshot_waiter();
    std::thread::sleep(Duration::from_millis(120));
    niri_action(&action)?;
    let saved = waiter.wait(Duration::from_secs(20))?;
    if saved.as_deref().is_some_and(|p| Path::new(p).is_file()) {
        return Ok(());
    }
    if path.is_file() {
        return Ok(());
    }
    Err("the compositor did not write a screenshot".into())
}

fn shoot_window(id: u64, path: &Path) -> Result<(), String> {
    let path_str = path.to_string_lossy().into_owned();
    shoot(
        json!({
            "ScreenshotWindow": {
                "id": id,
                "write_to_disk": true,
                "show_pointer": false,
                "path": path_str,
            }
        }),
        path,
    )
}

fn shoot_output(name: &str, path: &Path) -> Result<(), String> {
    let path_str = path.to_string_lossy().into_owned();
    shoot(
        json!({
            "HelixScreenshotOutput": {
                "output": name,
                "write_to_disk": true,
                "show_pointer": false,
                "path": path_str,
            }
        }),
        path,
    )
}

fn shoot_all_outputs(path: &Path) -> Result<(), String> {
    let path_str = path.to_string_lossy().into_owned();
    shoot(
        json!({
            "HelixScreenshotAllOutputs": {
                "write_to_disk": true,
                "show_pointer": false,
                "path": path_str,
            }
        }),
        path,
    )
}

fn shoot_screen(path: &Path) -> Result<(), String> {
    let path_str = path.to_string_lossy().into_owned();
    shoot(
        json!({
            "ScreenshotScreen": {
                "write_to_disk": true,
                "show_pointer": false,
                "path": path_str,
            }
        }),
        path,
    )
}

/// `desktop-capture` for Agent Surface. Returns `ok <absolute-path>\n`.
pub fn desktop_capture(app: &AppHandle, target: Option<String>) -> Result<String, String> {
    let caps = helix_compositor::compositor_ipc::helix_capabilities();
    let placement = helix_compositor::compositor_config::agent_desk_placement_enabled();
    let scope = resolve_scope(target.as_deref(), caps.agent_desk, placement);
    let path = capture_file()?;
    match scope {
        CaptureScope::AgentDesk => {
            let desk = helix_compositor::compositor_ipc::agent_desk_status();
            if !desk.available {
                return Err("agent desk is not available on this compositor".into());
            }
            let workspace = desk
                .workspace_id
                .ok_or("the agent desk workspace does not exist yet")?;
            let niri = shell_state(app).lock().expect("shell state").niri.clone();
            let window = pick_agent_desk_window(&niri, workspace)
                .ok_or("there are no windows on the agent desk to capture")?;
            shoot_window(window, &path)?;
        }
        CaptureScope::AllOutputs => {
            if !helix_compositor::compositor_ipc::is_helix_compositor() {
                return Err(
                    "capturing every output requires helix-niri; use focused or agent-desk on stock niri"
                        .into(),
                );
            }
            shoot_all_outputs(&path)?;
        }
        CaptureScope::Output(name) => {
            if !helix_compositor::compositor_ipc::is_helix_compositor() {
                return Err(format!(
                    "capturing output {name} requires helix-niri; use focused or agent-desk on stock niri"
                ));
            }
            shoot_output(&name, &path)?;
        }
        CaptureScope::FocusedOutput => {
            shoot_screen(&path)?;
        }
    }
    if !path.is_file() {
        return Err("capture file was not created".into());
    }
    Ok(format!("ok {}\n", path.display()))
}
