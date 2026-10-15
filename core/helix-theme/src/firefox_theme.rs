//! Pywalfox-compatible Firefox theme push after a template writes `colors.json`
//! (`src/theme/firefox_theme/firefox_theme.cpp` `applyFirefoxTheme`).

use std::fs;
use std::io::Write;
use std::os::unix::net::UnixDatagram;
use std::path::{Path, PathBuf};

use serde_json::Value;

const EXTENSION_ID: &str = "pywalfox@frewacom.org";
const MANIFEST_NAME: &str = "pywalfox";
const CMD_UPDATE: &str = "action:update";
const CMD_DARK: &str = "theme:mode:dark";
const CMD_LIGHT: &str = "theme:mode:light";

#[derive(Debug, PartialEq, Eq)]
pub struct ApplyResult {
    pub warning: Option<String>,
}

/// After `colors.json` exists: ensure the native-messaging manifest when safe, set theme mode, notify hosts.
pub fn apply(colors_json: &Path, mode: &str) -> Result<ApplyResult, String> {
    if colors_json.as_os_str().is_empty() {
        return Err("colors.json path is empty".into());
    }
    if !colors_json.is_file() {
        return Err(format!("colors.json not found: {}", colors_json.display()));
    }
    validate_colors_json(colors_json)?;

    let mut warning = None;
    ensure_manifest_owned_by_helix(&mut warning)?;

    if mode == "dark" || mode == "light" {
        set_theme_mode(mode)?;
        let cmd = if mode == "dark" { CMD_DARK } else { CMD_LIGHT };
        notify_running_hosts(cmd, &mut warning);
    }
    notify_running_hosts(CMD_UPDATE, &mut warning);
    Ok(ApplyResult { warning })
}

fn validate_colors_json(path: &Path) -> Result<(), String> {
    let text = fs::read_to_string(path).map_err(|_| format!("Could not read colors from: {}", path.display()))?;
    let root: Value = serde_json::from_str(&text)
        .map_err(|error| format!("Failed to read colors: {error}"))?;
    let colors = root
        .get("colors")
        .and_then(Value::as_object)
        .ok_or_else(|| format!("{} does not contain any color values", path.display()))?;
    if !root.get("wallpaper").and_then(Value::as_str).is_some() {
        return Err(format!("{} does not contain a wallpaper path", path.display()));
    }
    let mut entries: Vec<String> = Vec::new();
    for i in 0..16 {
        let key = format!("color{i}");
        if let Some(c) = colors.get(&key).and_then(Value::as_str) {
            entries.push(c.to_string());
        }
    }
    if entries.len() < 16 {
        entries.clear();
        for (_, v) in colors {
            if let Some(c) = v.as_str() {
                entries.push(c.to_string());
            }
        }
    }
    if entries.len() < 16 {
        return Err(format!(
            "{} containing the generated Pywal colors is invalid",
            path.display()
        ));
    }
    Ok(())
}

fn home_dir() -> Option<PathBuf> {
    std::env::var_os("HOME").map(PathBuf::from).filter(|p| !p.as_os_str().is_empty())
}

fn xdg_config_home() -> Option<PathBuf> {
    if let Ok(v) = std::env::var("XDG_CONFIG_HOME") {
        if !v.is_empty() {
            return Some(PathBuf::from(v));
        }
    }
    home_dir().map(|h| h.join(".config"))
}

fn command_notify_dir() -> PathBuf {
    if let Ok(runtime) = std::env::var("XDG_RUNTIME_DIR") {
        if !runtime.is_empty() {
            return PathBuf::from(runtime).join("helix").join("firefox-theme");
        }
    }
    let uid = unsafe { libc::getuid() };
    std::env::temp_dir().join(format!("helix-firefox-theme-{uid}"))
}

fn command_notify_path() -> PathBuf {
    command_notify_dir().join("command")
}

fn unix_socket_path() -> PathBuf {
    let uid = unsafe { libc::getuid() };
    std::env::temp_dir().join(format!("pywalfox_socket_{uid}"))
}

fn publish_command(command: &str) -> bool {
    let dir = command_notify_dir();
    if fs::create_dir_all(&dir).is_err() {
        return false;
    }
    let path = command_notify_path();
    let tmp = dir.join(format!("command.tmp.{}", std::process::id()));
    if fs::File::create(&tmp)
        .and_then(|mut f| f.write_all(command.as_bytes()).and_then(|_| f.flush()))
        .is_err()
    {
        return false;
    }
    fs::rename(&tmp, &path).is_ok()
}

fn try_send_socket_command(command: &str) -> bool {
    let path = unix_socket_path();
    let sock = match UnixDatagram::unbound() {
        Ok(sock) => sock,
        Err(_) => return false,
    };
    sock.connect(path).is_ok() && sock.send(command.as_bytes()).is_ok()
}

fn notify_running_hosts(command: &str, warning: &mut Option<String>) {
    if publish_command(command) || try_send_socket_command(command) {
        return;
    }
    if warning.is_none() {
        *warning = Some(
            "Firefox theme host is not running; colors will apply when the extension connects".into(),
        );
    }
}

fn user_manifest_path() -> Option<PathBuf> {
    home_dir().map(|h| h.join(".mozilla").join("native-messaging-hosts").join("pywalfox.json"))
}

fn resolve_host_executable() -> Option<PathBuf> {
    if let Ok(path) = std::env::var("HELIX_TEMPLATE_APPLY_BIN") {
        let p = PathBuf::from(path);
        if p.is_file() {
            return Some(p);
        }
    }
    if let Ok(bindir) = std::env::var("HELIX_BINDIR") {
        for name in ["helix-desktop", "helix-template-apply"] {
            let p = PathBuf::from(&bindir).join(name);
            if p.is_file() {
                return Some(p);
            }
        }
    }
    if let Ok(path) = std::env::current_exe() {
        if path.is_file() {
            return Some(path);
        }
    }
    None
}

fn path_looks_like_helix_host(path: &Path) -> bool {
    match path.file_name().and_then(|n| n.to_str()) {
        Some("helix-shell")
        | Some("helix-pywalfox")
        | Some("helix-desktop")
        | Some("helix-template-apply") => true,
        _ => false,
    }
}

fn read_existing_manifest_host() -> Option<PathBuf> {
    let manifest = user_manifest_path()?;
    let text = fs::read_to_string(manifest).ok()?;
    let root: Value = serde_json::from_str(&text).ok()?;
    root.get("path")
        .and_then(Value::as_str)
        .map(PathBuf::from)
}

fn install_manifest(host: &Path) -> Result<(), String> {
    let manifest = user_manifest_path().ok_or_else(|| "HOME is not set".to_string())?;
    fs::create_dir_all(manifest.parent().unwrap()).map_err(|error| {
        format!("failed to create native-messaging-hosts directory: {error}")
    })?;
    let canonical = host.canonicalize().unwrap_or_else(|_| host.to_path_buf());
    let body = serde_json::json!({
        "name": MANIFEST_NAME,
        "description": "Helix Firefox theme native messaging host",
        "path": canonical.to_string_lossy(),
        "type": "stdio",
        "allowed_extensions": [EXTENSION_ID],
    });
    fs::write(&manifest, format!("{}\n", serde_json::to_string_pretty(&body).unwrap()))
        .map_err(|error| format!("failed to write {}: {error}", manifest.display()))?;
    Ok(())
}

fn ensure_manifest_owned_by_helix(warning: &mut Option<String>) -> Result<(), String> {
    let host = resolve_host_executable().ok_or_else(|| "helix executable not found".to_string())?;
    if let Some(existing) = read_existing_manifest_host() {
        if !path_looks_like_helix_host(&existing) {
            *warning = Some(format!("leaving existing native messaging host at {}", existing.display()));
            return Ok(());
        }
    }
    install_manifest(&host)
}

fn pywalfox_config_path() -> Option<PathBuf> {
    xdg_config_home().map(|c| c.join("pywalfox").join("config.json"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    #[test]
    fn validate_requires_sixteen_colors() {
        let dir = std::env::temp_dir().join(format!("helix-ff-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("colors.json");
        let mut file = fs::File::create(&path).unwrap();
        writeln!(
            file,
            "{{\"colors\":{{\"color0\":\"#000\"}}, \"wallpaper\": \"/tmp/w.png\"}}"
        )
        .unwrap();
        assert!(validate_colors_json(&path).is_err());
        let _ = fs::remove_dir_all(&dir);
    }
}

fn set_theme_mode(mode: &str) -> Result<(), String> {
    let path = pywalfox_config_path().ok_or_else(|| "HOME is not set".to_string())?;
    let mut root: Value = fs::read_to_string(&path)
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .filter(Value::is_object)
        .unwrap_or_else(|| Value::Object(serde_json::Map::new()));
    if let Some(table) = root.as_object_mut() {
        table.insert("theme_mode".into(), Value::String(mode.to_string()));
    }
    fs::create_dir_all(path.parent().unwrap()).map_err(|e| e.to_string())?;
    fs::write(&path, format!("{}\n", serde_json::to_string_pretty(&root).unwrap()))
        .map_err(|e| e.to_string())?;
    Ok(())
}
