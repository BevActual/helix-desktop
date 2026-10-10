//! Client (bar → panels) and server (panels process) for [`helix_shell_core::panels_ipc`].

use std::path::PathBuf;
use std::time::Duration;

use helix_shell_core::panels_ipc::{
    decode_reply, decode_request, encode_reply, encode_request, socket_file_name, PanelsReply,
    PanelsRequest, MAX_LINE,
};
use tauri::AppHandle;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::{UnixListener, UnixStream};

use crate::role::ShellRole;

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

/// One request/reply round-trip. `Ok(None)` when the socket is missing (panels not running).
pub async fn call(req: PanelsRequest) -> Result<Option<PanelsReply>, String> {
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

pub async fn ping() -> bool {
    matches!(call(PanelsRequest::Ping).await, Ok(Some(PanelsReply::Pong)))
}

/// Best-effort: start the panels unit/binary if the socket is down.
pub fn ensure_panels_started() {
    if socket_path().is_some_and(|p| p.exists()) {
        return;
    }
    // Prefer user unit when installed.
    let _ = std::process::Command::new("systemctl")
        .args(["--user", "start", "helix-shell-panels.service"])
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn();
    // Fallback: same binary with panels role (dev / before units exist).
    if let Ok(exe) = std::env::current_exe() {
        let mut cmd = std::process::Command::new(exe);
        cmd.env("HELIX_SHELL_ROLE", "panels")
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null());
        // Confirmer stays on panels when split.
        if std::env::var_os("HELIX_SHELL_CONFIRMER").is_some() {
            cmd.env("HELIX_SHELL_CONFIRMER", "1");
        }
        let _ = cmd.spawn();
    }
}

pub async fn open_panel(id: &str, mode: Option<&str>) -> Result<bool, String> {
    ensure_panels_started();
    // Brief wait for the socket after spawn.
    for _ in 0..40 {
        if ping().await {
            break;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    let req = PanelsRequest::Open {
        id: id.to_string(),
        mode: mode.map(str::to_string),
    };
    match call(req).await? {
        Some(PanelsReply::Ok) | Some(PanelsReply::Status { .. }) => Ok(true),
        Some(PanelsReply::Err { message }) => Err(message),
        Some(_) => Ok(true),
        None => Ok(false),
    }
}

pub async fn close_panel() -> Result<bool, String> {
    match call(PanelsRequest::Close).await? {
        Some(_) => Ok(true),
        None => Ok(false),
    }
}

pub async fn assistant_submit(request: &str) -> Result<bool, String> {
    ensure_panels_started();
    for _ in 0..40 {
        if ping().await {
            break;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    let req = PanelsRequest::AssistantSubmit {
        request: request.to_string(),
    };
    match call(req).await? {
        Some(PanelsReply::Ok) => Ok(true),
        Some(PanelsReply::Err { message }) => Err(message),
        Some(_) => Ok(true),
        None => Ok(false),
    }
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
