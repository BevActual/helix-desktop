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
pub fn launch(app: &App) -> Result<(), String> {
    let (program, args) = app.argv.split_first().ok_or("the entry has no command")?;
    let mut command = Command::new(program);
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
