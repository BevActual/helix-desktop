//! Session `helix-$WAYLAND_DISPLAY.sock` msg IPC (subset of C++ `IpcService`).

use std::io::{Read, Write};
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use tokio::sync::Mutex;

use crate::state::State;
use crate::template_apply;

const MAX_COMMAND_BYTES: usize = 64 * 1024;
const CALLER_CWD_SEP: char = '\x1e';

pub fn resolve_socket_path() -> PathBuf {
    let runtime = std::env::var_os("XDG_RUNTIME_DIR")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp"));
    let display = std::env::var("WAYLAND_DISPLAY").unwrap_or_else(|_| "wayland-0".into());
    if display.contains('/') {
        runtime.join("helix-wayland.sock")
    } else {
        runtime.join(format!("helix-{display}.sock"))
    }
}

fn strip_caller_cwd_prefix(line: &mut String) {
    if let Some(idx) = line.find(CALLER_CWD_SEP) {
        let cwd = line[..idx].to_string();
        if cwd.starts_with('/') && std::path::Path::new(&cwd).is_dir() {
            *line = line[idx + 1..].to_string();
        }
    }
}

fn parse_command(line: &str) -> (&str, &str) {
    let line = line.trim();
    if let Some(space) = line.find(' ') {
        (line[..space].trim(), line[space + 1..].trim())
    } else {
        (line, "")
    }
}

fn handle_line(state: &State, line: &str) -> String {
    let mut command = line.to_string();
    strip_caller_cwd_prefix(&mut command);
    let (name, args) = parse_command(&command);
    match name {
        "templates-apply" => {
            if !args.is_empty() {
                return "error: usage: templates-apply\n".to_string();
            }
            if state.desktop.is_none() {
                return "error: theme palette has not been resolved yet\n".to_string();
            }
            match template_apply::reapply_templates(&state.config) {
                Ok(()) => "ok\n".to_string(),
                Err(message) => format!("error: {message}\n"),
            }
        }
        "--help" | "-h" => {
            "Usage: helix-panel <command> [args]\n\nCommands:\n  templates-apply  Apply configured theme templates for the current palette\n".to_string()
        }
        "" => String::new(),
        _ => "error: unknown command (try: helix-panel --help)\n".to_string(),
    }
}

fn serve_connection(mut stream: UnixStream, state: &State) {
    stream
        .set_read_timeout(Some(Duration::from_millis(100)))
        .ok();
    let mut command = String::new();
    let mut buf = [0u8; 4096];
    while command.len() < MAX_COMMAND_BYTES {
        match stream.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => command.push_str(&String::from_utf8_lossy(&buf[..n])),
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => break,
            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(_) => return,
        }
    }
    if command.len() >= MAX_COMMAND_BYTES {
        let _ = stream.write_all(b"error: IPC command too large\n");
        return;
    }
    let response = handle_line(state, &command);
    if response.is_empty() {
        return;
    }
    let _ = stream.write_all(response.as_bytes());
}

fn ipc_thread(state: Arc<Mutex<State>>) {
    let path = resolve_socket_path();
    let _ = std::fs::remove_file(&path);
    let listener = match UnixListener::bind(&path) {
        Ok(listener) => listener,
        Err(error) => {
            tracing::warn!(%error, path = %path.display(), "msg IPC bind failed");
            return;
        }
    };
    if let Ok(meta) = std::fs::metadata(&path) {
        let mut perms = meta.permissions();
        perms.set_mode(0o600);
        let _ = std::fs::set_permissions(&path, perms);
    }
    tracing::info!(path = %path.display(), "helix-desktop msg IPC listening");
    for connection in listener.incoming() {
        match connection {
            Ok(stream) => {
                let locked = state.blocking_lock();
                serve_connection(stream, &locked);
            }
            Err(error) => {
                tracing::warn!(%error, "msg IPC accept failed");
            }
        }
    }
}

pub fn start(state: Arc<Mutex<State>>) {
    std::thread::spawn(move || ipc_thread(state));
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn templates_apply_requires_desktop() {
        let state = State::new(crate::agents::new_hub());
        let out = handle_line(&state, "templates-apply");
        assert!(out.contains("not been resolved"));
    }

    #[test]
    fn rejects_extra_args() {
        let state = State::new(crate::agents::new_hub());
        let out = handle_line(&state, "templates-apply extra");
        assert!(out.contains("usage"));
    }

    #[test]
    fn templates_apply_ok_when_desktop_registered() {
        let mut state = State::new(crate::agents::new_hub());
        state.desktop = Some(":1.42".into());
        let out = handle_line(&state, "templates-apply");
        assert_eq!(out, "ok\n");
    }
}
