//! Finds the installed applications (desktop entries) and starts one. Parsing and ranking live in helix_shell_core::launcher.
//! The page never sees or sends a command: it names an application by id and the argument list comes from the entry found here.

use std::collections::BTreeSet;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use helix_shell_core::launcher::{parse_desktop_entry, App};

const MAX_ENTRIES: usize = 5000;
const MAX_FILE_BYTES: u64 = 64 * 1024;
const MAX_DEPTH: usize = 4;

fn data_dirs() -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    let home_data = std::env::var_os("XDG_DATA_HOME")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| Path::new(&home).join(".local/share")));
    dirs.extend(home_data);
    let system = std::env::var("XDG_DATA_DIRS")
        .ok()
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| "/usr/local/share:/usr/share".to_string());
    dirs.extend(
        system
            .split(':')
            .filter(|d| !d.is_empty())
            .map(PathBuf::from),
    );
    dirs
}

fn desktops() -> Vec<String> {
    std::env::var("XDG_CURRENT_DESKTOP")
        .unwrap_or_default()
        .split(':')
        .filter(|name| !name.is_empty())
        .map(str::to_string)
        .collect()
}

fn walk(root: &Path, dir: &Path, depth: usize, out: &mut Vec<(String, PathBuf)>) {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let Ok(kind) = entry.file_type() else {
            continue;
        };
        if kind.is_dir() && depth < MAX_DEPTH {
            walk(root, &path, depth + 1, out);
        } else if (kind.is_file() || kind.is_symlink())
            && path.extension().is_some_and(|ext| ext == "desktop")
        {
            if let Ok(relative) = path.strip_prefix(root) {
                // The desktop file id: the path below applications/ with directory separators turned into dashes.
                out.push((relative.to_string_lossy().replace('/', "-"), path));
            }
        }
    }
}

/// Every application this session should list, alphabetically. Earlier data directories win over later ones for the same id,
/// so a user's own entry overrides the system one. Terminal applications are left out: the launcher has no terminal to put them in.
pub fn scan() -> Vec<App> {
    let desktops = desktops();
    let mut seen = BTreeSet::new();
    let mut apps = Vec::new();
    for dir in data_dirs() {
        let root = dir.join("applications");
        let mut files = Vec::new();
        walk(&root, &root, 0, &mut files);
        files.sort();
        for (id, path) in files {
            if apps.len() >= MAX_ENTRIES || !seen.insert(id.clone()) {
                continue;
            }
            let Ok(meta) = std::fs::metadata(&path) else {
                continue;
            };
            if meta.len() > MAX_FILE_BYTES {
                continue;
            }
            let Ok(text) = std::fs::read_to_string(&path) else {
                continue;
            };
            if let Some(app) = parse_desktop_entry(&id, &text) {
                if app.shown_in(&desktops) && !app.terminal {
                    apps.push(app);
                }
            }
        }
    }
    apps.sort_by_cached_key(|app| app.name.to_lowercase());
    apps
}

/// Starts the application in its own process group, with no terminal and no inherited stdio, and reaps it when it exits.
/// Settings the shell's own processes are started with. A program the shell launches is not part of the shell, and must not take its role
/// from the process that started it (a second shell started by hand from a launched terminal would otherwise come up as "panels").
const SHELL_ONLY_VARS: &[&str] = &["HELIX_SHELL_ROLE", "HELIX_SHELL_CONFIRMER", "HELIX_SURFACE_CONTROL", "HELIX_SPIKE_AUTO_PANEL"];

pub fn scrub_shell_env(command: &mut Command) -> &mut Command {
    for name in SHELL_ONLY_VARS {
        command.env_remove(name);
    }
    command
}

pub fn launch(app: &App) -> Result<(), String> {
    let (program, args) = app.argv.split_first().ok_or("the entry has no command")?;
    let mut command = Command::new(program);
    scrub_shell_env(&mut command);
    command
        .args(args)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .process_group(0);
    if let Some(home) = std::env::var_os("HOME") {
        command.current_dir(home);
    }
    let mut child = command.spawn().map_err(|error| error.to_string())?;
    std::thread::spawn(move || {
        let _ = child.wait();
    });
    Ok(())
}

const MAX_ICON_BYTES: u64 = 96 * 1024;
const ICON_SIZES: &[&str] = &[
    "48x48", "64x64", "32x32", "128x128", "256x256", "scalable", "24x24",
];

fn icon_candidates(icon: &str) -> Vec<PathBuf> {
    if icon.starts_with('/') {
        return vec![PathBuf::from(icon)];
    }
    // A theme icon name is a single path component: nothing that could climb out of the icon directories.
    if icon.is_empty() || icon.contains('/') || icon.contains("..") || icon.contains('\0') {
        return Vec::new();
    }
    let mut paths = Vec::new();
    for dir in data_dirs() {
        for size in ICON_SIZES {
            for ext in ["png", "svg"] {
                paths.push(dir.join(format!("icons/hicolor/{size}/apps/{icon}.{ext}")));
            }
        }
        for ext in ["png", "svg"] {
            paths.push(dir.join(format!("pixmaps/{icon}.{ext}")));
        }
    }
    paths
}

/// The icon as a `data:` URI the page can show, or `None`. Only PNG and SVG files up to 96 KB are read. An SVG shown through an
/// image element cannot run script, so it is as safe as a PNG.
pub fn icon_data_uri(icon: &str) -> Option<String> {
    use base64::Engine;
    for path in icon_candidates(icon) {
        let mime = match path.extension().and_then(|ext| ext.to_str()) {
            Some("png") => "image/png",
            Some("svg") => "image/svg+xml",
            _ => continue,
        };
        let Ok(meta) = std::fs::metadata(&path) else {
            continue;
        };
        if !meta.is_file() || meta.len() == 0 || meta.len() > MAX_ICON_BYTES {
            continue;
        }
        let Ok(bytes) = std::fs::read(&path) else {
            continue;
        };
        return Some(format!(
            "data:{mime};base64,{}",
            base64::engine::general_purpose::STANDARD.encode(bytes)
        ));
    }
    None
}

fn history_path() -> Option<PathBuf> {
    let state = std::env::var_os("XDG_STATE_HOME")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| Path::new(&home).join(".local/state")))?;
    Some(state.join("helix/launcher-history.json"))
}

/// The launch history saved by an earlier session. A missing or damaged file is an empty history, never an error.
pub fn load_history() -> helix_shell_core::launcher::History {
    let entries = history_path()
        .and_then(|path| std::fs::read_to_string(path).ok())
        .and_then(|text| serde_json::from_str::<serde_json::Value>(&text).ok())
        .and_then(|value| value.get("entries").and_then(|e| e.as_array()).cloned())
        .unwrap_or_default();
    helix_shell_core::launcher::History::from_entries(entries.iter().filter_map(|entry| {
        Some((
            entry.get("id")?.as_str()?.to_string(),
            u32::try_from(entry.get("count")?.as_u64()?).ok()?,
            entry.get("last")?.as_u64()?,
        ))
    }))
}

/// Saves the history privately (directory 0700, file 0600), by writing a new file and renaming it so a crash cannot leave half a file.
pub fn save_history(history: &helix_shell_core::launcher::History) {
    use std::io::Write;
    use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
    let Some(path) = history_path() else { return };
    let Some(dir) = path.parent() else { return };
    if std::fs::DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(dir)
        .is_err()
    {
        return;
    }
    // The folder may have been made by another process with looser permissions; what is kept here is the person's alone.
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = std::fs::set_permissions(dir, std::fs::Permissions::from_mode(0o700));
    }
    let entries: Vec<serde_json::Value> = history
        .entries()
        .map(|(id, count, last)| serde_json::json!({"id": id, "count": count, "last": last}))
        .collect();
    let text = serde_json::json!({"version": 1, "entries": entries}).to_string();
    let temporary = path.with_extension("json.tmp");
    let written = std::fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(0o600)
        .open(&temporary)
        .and_then(|mut file| file.write_all(text.as_bytes()))
        .and_then(|()| std::fs::rename(&temporary, &path));
    if written.is_err() {
        let _ = std::fs::remove_file(&temporary);
    }
}

#[cfg(test)]
mod env_tests {
    use super::*;

    #[test]
    fn a_launched_program_does_not_inherit_the_shells_role() {
        let mut command = Command::new("true");
        command.env("HELIX_SHELL_ROLE", "panels").env("HOME", "/home/x");
        scrub_shell_env(&mut command);
        let removed: Vec<_> = command.get_envs().filter(|(_, v)| v.is_none()).map(|(k, _)| k.to_string_lossy().into_owned()).collect();
        assert!(removed.contains(&"HELIX_SHELL_ROLE".to_string()), "{removed:?}");
        assert!(removed.contains(&"HELIX_SHELL_CONFIRMER".to_string()));
        assert!(!removed.contains(&"HOME".to_string()), "ordinary variables are untouched");
    }
}
