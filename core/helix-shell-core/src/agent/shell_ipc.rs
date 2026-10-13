//! Desktop service → session shell agent socket (same wire format as C++ `IpcClient` / `buildWireCommand`).

use std::collections::BTreeSet;
use std::fs;
use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::time::Duration;

/// Runtime dir for agent sockets (`HELIX_AGENT_IPC_DIR` or `XDG_RUNTIME_DIR`).
pub fn shell_runtime_dir() -> Option<PathBuf> {
    if let Ok(dir) = std::env::var("HELIX_AGENT_IPC_DIR") {
        if !dir.is_empty() {
            return Some(PathBuf::from(dir));
        }
    }
    std::env::var_os("XDG_RUNTIME_DIR").map(PathBuf::from).filter(|p| !p.as_os_str().is_empty())
}

fn clean_token(text: &str) -> bool {
    !text.contains('\x1e') && !text.contains('\0')
}

/// Build one line for the shell command socket (`/\x1e` cwd prefix + tool + arguments).
pub fn build_wire_command(
    tool: &str,
    argv: &[String],
    literal: &[String],
) -> Result<String, String> {
    if tool.is_empty() || tool.contains(' ') || !clean_token(tool) {
        return Err("invalid command name".into());
    }
    let mut line = String::from("/\x1e");
    line.push_str(tool);
    for argument in argv.iter().chain(literal.iter()) {
        if !clean_token(argument) {
            return Err("argument contains a reserved control byte".into());
        }
        line.push(' ');
        line.push_str(argument);
    }
    Ok(line)
}

fn socket_answers(path: &Path) -> bool {
    UnixStream::connect(path).is_ok()
}

/// Newest `helix-wayland*.sock` that accepts a connection, else `helix-<WAYLAND_DISPLAY>.sock`.
pub fn shell_socket_path() -> Option<PathBuf> {
    let runtime = shell_runtime_dir()?;
    if let Ok(display) = std::env::var("WAYLAND_DISPLAY") {
        if !display.is_empty() && !display.contains('/') {
            let named = runtime.join(format!("helix-{display}.sock"));
            if named.exists() {
                return Some(named);
            }
        }
    }
    let mut found: Vec<(std::time::SystemTime, PathBuf)> = Vec::new();
    let entries = fs::read_dir(&runtime).ok()?;
    for entry in entries.flatten() {
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if !name.starts_with("helix-wayland") || !name.ends_with(".sock") {
            continue;
        }
        let modified = entry.metadata().and_then(|m| m.modified()).ok()?;
        found.push((modified, entry.path()));
    }
    found.sort_by(|a, b| b.0.cmp(&a.0));
    for (_, path) in &found {
        if socket_answers(path) {
            return Some(path.clone());
        }
    }
    found.first().map(|(_, path)| path.clone())
}

/// Command names advertised by the live shell (`helix-shell-tools-<display>`); empty if none.
pub fn shell_supported_tools() -> BTreeSet<String> {
    let mut names = BTreeSet::new();
    let Some(runtime) = shell_runtime_dir() else {
        return names;
    };
    let Ok(entries) = fs::read_dir(runtime) else {
        return names;
    };
    let mut newest: Option<(std::time::SystemTime, PathBuf)> = None;
    for entry in entries.flatten() {
        let file_name = entry.file_name();
        let file_name = file_name.to_string_lossy();
        if !file_name.starts_with("helix-shell-tools-") {
            continue;
        }
        let Some(modified) = entry.metadata().and_then(|m| m.modified()).ok() else {
            continue;
        };
        let path = entry.path();
        let Ok(text) = fs::read_to_string(&path) else {
            continue;
        };
        let Some(pid_line) = text.lines().next() else {
            continue;
        };
        if pid_line.is_empty() || !pid_line.bytes().all(|b| b.is_ascii_digit()) {
            continue;
        }
        if !Path::new("/proc").join(pid_line).exists() {
            continue;
        }
        if newest.as_ref().is_none_or(|(time, _)| modified > *time) {
            newest = Some((modified, path));
        }
    }
    let Some((_, path)) = newest else {
        return names;
    };
    let Ok(text) = fs::read_to_string(&path) else {
        return names;
    };
    for line in text.lines().skip(1) {
        if !line.is_empty() && line.len() <= 80 {
            names.insert(line.to_string());
        }
    }
    names
}

/// Send one command line to the shell socket; returns the response body (trimmed).
pub fn exchange(line: &str, socket: Option<&Path>) -> Result<String, String> {
    let path = socket
        .map(PathBuf::from)
        .or_else(shell_socket_path)
        .ok_or_else(|| "helix is not running".to_string())?;
    let mut stream = UnixStream::connect(&path).map_err(|_| "helix is not running".to_string())?;
    stream
        .set_read_timeout(Some(Duration::from_secs(2)))
        .ok();
    stream
        .set_write_timeout(Some(Duration::from_secs(2)))
        .ok();
    stream
        .write_all(line.as_bytes())
        .map_err(|error| error.to_string())?;
    stream.shutdown(std::net::Shutdown::Write).ok();
    let mut response = String::new();
    stream
        .read_to_string(&mut response)
        .map_err(|error| error.to_string())?;
    while response.ends_with('\n') || response.ends_with('\r') {
        response.pop();
    }
    Ok(response)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wire_command_matches_service_format() {
        assert_eq!(
            build_wire_command("volume-up", &[], &[]).unwrap(),
            "/\x1evolume-up"
        );
        assert_eq!(
            build_wire_command("clipboard-copy", &[], &["hello world".into()]).unwrap(),
            "/\x1eclipboard-copy hello world"
        );
        assert!(build_wire_command("two words", &[], &[]).is_err());
    }
}
