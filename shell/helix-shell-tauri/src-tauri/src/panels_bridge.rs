//! Client (bar → panels) and server (panels process) for [`helix_shell_core::panels_ipc`].

use std::path::PathBuf;
use std::process::Stdio;
use std::time::Duration;

use helix_shell_core::panels_ipc::{
    decode_reply, decode_request, encode_reply, encode_request, socket_file_name, PanelsReply,
    PanelsRequest, MAX_LINE,
};
use tauri::AppHandle;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::{UnixListener, UnixStream};
use tokio::time::timeout;

use crate::role::ShellRole;

/// Hard cap on one client round-trip so a hung panels process cannot block D-Bus / keybinds.
const CALL_TIMEOUT: Duration = Duration::from_secs(2);
const ENSURE_POLL: Duration = Duration::from_millis(50);
const ENSURE_ATTEMPTS: u32 = 40;

fn runtime_dir() -> Option<PathBuf> {
    std::env::var_os("HELIX_PANELS_IPC_DIR")
        .or_else(|| std::env::var_os("HELIX_AGENT_IPC_DIR"))
        .or_else(|| std::env::var_os("XDG_RUNTIME_DIR"))
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
}

pub fn socket_path() -> Option<PathBuf> {
    let display = std::env::var("WAYLAND_DISPLAY")
        .ok()
        .filter(|d| !d.is_empty() && !d.contains('/'))?;
    let name = socket_file_name(&display)?;
    Some(runtime_dir()?.join(name))
}

async fn write_line(stream: &mut UnixStream, line: &str) -> Result<(), String> {
    let mut buf = line.as_bytes().to_vec();
    buf.push(b'\n');
    stream.write_all(&buf).await.map_err(|e| e.to_string())?;
    stream.flush().await.map_err(|e| e.to_string())
}

async fn read_line(stream: &mut UnixStream) -> Result<String, String> {
    let mut reader = BufReader::new(stream);
    let mut line = String::new();
    let n = reader
        .read_line(&mut line)
        .await
        .map_err(|e| e.to_string())?;
    if n == 0 {
        return Err("panels socket closed".into());
    }
    if line.len() > MAX_LINE {
        return Err("panels reply too long".into());
    }
    Ok(line)
}

async fn call_once(req: PanelsRequest) -> Result<Option<PanelsReply>, String> {
    let Some(path) = socket_path() else {
        return Ok(None);
    };
    if !path.exists() {
        return Ok(None);
    }
    let mut stream = match UnixStream::connect(&path).await {
        Ok(s) => s,
        Err(_) => return Ok(None),
    };
    let line = encode_request(&req).map_err(|e| e.to_string())?;
    write_line(&mut stream, &line).await?;
    let reply_line = read_line(&mut stream).await?;
    let reply = decode_reply(&reply_line).map_err(|e| e.to_string())?;
    Ok(Some(reply))
}

/// One request/reply round-trip with a deadline. `Ok(None)` when the socket is missing or dead.
pub async fn call(req: PanelsRequest) -> Result<Option<PanelsReply>, String> {
    match timeout(CALL_TIMEOUT, call_once(req)).await {
        Ok(result) => result,
        Err(_) => Err("panels ipc timed out".into()),
    }
}

pub async fn ping() -> bool {
    matches!(call(PanelsRequest::Ping).await, Ok(Some(PanelsReply::Pong)))
}

/// Which panel is open on the panels process (`None` if closed or unreachable).
pub async fn open_status() -> Result<Option<String>, String> {
    match call(PanelsRequest::Status).await? {
        Some(PanelsReply::Status { panel }) => Ok(panel),
        Some(PanelsReply::Err { message }) => Err(message),
        Some(_) => Ok(None),
        None => Ok(None),
    }
}

fn panels_unit_installed() -> bool {
    if helix_shell_core::nested_test_session() {
        return false;
    }
    std::process::Command::new("systemctl")
        .args(["--user", "cat", "helix-shell-panels.service"])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

fn unlink_stale_socket() {
    let Some(path) = socket_path() else {
        return;
    };
    if path.exists() {
        let _ = std::fs::remove_file(&path);
        eprintln!("PANELS ipc: removed stale socket {}", path.display());
    }
}

fn start_panels_via_systemd() -> bool {
    if helix_shell_core::nested_test_session() {
        return false;
    }
    std::process::Command::new("systemctl")
        .args(["--user", "start", "helix-shell-panels.service"])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

fn start_panels_via_exe() {
    let Ok(exe) = std::env::current_exe() else {
        return;
    };
    let mut cmd = std::process::Command::new(exe);
    cmd.env("HELIX_SHELL_ROLE", "panels")
        .env("HELIX_SHELL_CONFIRMER", "1")
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    match cmd.spawn() {
        Ok(_) => eprintln!("PANELS ipc: spawned panels role (no systemd unit)"),
        Err(error) => eprintln!("PANELS ipc: spawn failed: {error}"),
    }
}

/// Best-effort: revive panels if Ping fails. Prefer systemd; never double-spawn when the unit exists.
pub async fn ensure_panels_started() {
    if ping().await {
        return;
    }
    unlink_stale_socket();
    if panels_unit_installed() {
        if !start_panels_via_systemd() {
            eprintln!("PANELS ipc: systemctl start helix-shell-panels failed");
        }
    } else {
        start_panels_via_exe();
    }
    for _ in 0..ENSURE_ATTEMPTS {
        if ping().await {
            return;
        }
        tokio::time::sleep(ENSURE_POLL).await;
    }
    eprintln!("PANELS ipc: panels did not answer Ping after ensure");
}

async fn wait_ready_then(req: PanelsRequest) -> Result<bool, String> {
    ensure_panels_started().await;
    match call(req).await? {
        Some(PanelsReply::Ok) | Some(PanelsReply::Status { .. }) => Ok(true),
        Some(PanelsReply::Err { message }) => Err(message),
        Some(_) => Ok(true),
        None => Ok(false),
    }
}

pub async fn open_panel(id: &str, mode: Option<&str>) -> Result<bool, String> {
    wait_ready_then(PanelsRequest::Open {
        id: id.to_string(),
        mode: mode.map(str::to_string),
    })
    .await
}

pub async fn close_panel() -> Result<bool, String> {
    match call(PanelsRequest::Close).await? {
        Some(_) => Ok(true),
        None => Ok(false),
    }
}

pub async fn escape() -> Result<bool, String> {
    match call(PanelsRequest::Escape).await? {
        Some(_) => Ok(true),
        None => Ok(false),
    }
}

pub async fn outside() -> Result<bool, String> {
    match call(PanelsRequest::Outside).await? {
        Some(_) => Ok(true),
        None => Ok(false),
    }
}

pub async fn assistant_submit(request: &str) -> Result<bool, String> {
    wait_ready_then(PanelsRequest::AssistantSubmit {
        request: request.to_string(),
    })
    .await
}

/// Panels process: accept connections and dispatch via `on_request`.
pub fn serve<F>(app: AppHandle, on_request: F)
where
    F: FnMut(&AppHandle, PanelsRequest) -> PanelsReply + Send + 'static,
{
    if ShellRole::from_env() != ShellRole::Panels {
        return;
    }
    let Some(path) = socket_path() else {
        eprintln!("PANELS ipc: no runtime dir / WAYLAND_DISPLAY");
        return;
    };
    tauri::async_runtime::spawn(async move {
        if let Some(parent) = path.parent() {
            let _ = tokio::fs::create_dir_all(parent).await;
        }
        let _ = tokio::fs::remove_file(&path).await;
        let listener = match UnixListener::bind(&path) {
            Ok(l) => l,
            Err(error) => {
                eprintln!("PANELS ipc bind {}: {error}", path.display());
                return;
            }
        };
        // 0600
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600));
        }
        eprintln!("PANELS ipc listening on {}", path.display());
        let handler = std::sync::Arc::new(std::sync::Mutex::new(on_request));
        loop {
            let Ok((mut stream, _)) = listener.accept().await else {
                continue;
            };
            let app = app.clone();
            let handler = handler.clone();
            tauri::async_runtime::spawn(async move {
                let reply = match read_line(&mut stream).await {
                    Ok(line) => match decode_request(&line) {
                        // Caller (`main`) hops to the GTK main thread; do not nest another hop here.
                        Ok(req) => handler.lock().expect("panels handler")(&app, req),
                        Err(error) => PanelsReply::Err {
                            message: error.to_string(),
                        },
                    },
                    Err(error) => PanelsReply::Err { message: error },
                };
                if let Ok(line) = encode_reply(&reply) {
                    let _ = write_line(&mut stream, &line).await;
                }
            });
        }
    });
}
