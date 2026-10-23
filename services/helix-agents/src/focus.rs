//! The folder an agent should start in: where the focused window's shell is. The compositor says which window has the keyboard (its process id);
//! a terminal's shell is a child of that process, and the program running in the shell a child of that, so the deepest descendant's working
//! directory is the folder the person is looking at.

use std::path::{Path, PathBuf};

/// `(pid, parent)` of every process under `proc_root`.
fn processes(proc_root: &Path) -> Vec<(u32, u32)> {
    let mut out = Vec::new();
    for entry in std::fs::read_dir(proc_root).into_iter().flatten().flatten() {
        let Ok(pid) = entry.file_name().to_string_lossy().parse::<u32>() else {
            continue;
        };
        let Ok(stat) = std::fs::read_to_string(entry.path().join("stat")) else {
            continue;
        };
        // `pid (name) state ppid ...`, where the name may contain spaces and parentheses: read after the last ')'.
        let Some(after) = stat.rsplit_once(')').map(|(_, rest)| rest) else {
            continue;
        };
        if let Some(parent) = after
            .split_whitespace()
            .nth(1)
            .and_then(|p| p.parse::<u32>().ok())
        {
            out.push((pid, parent));
        }
    }
    out
}

/// The working directory of the deepest (newest at each level) descendant of `pid`, or of `pid` itself.
pub fn working_directory(proc_root: &Path, pid: u32) -> Option<PathBuf> {
    let all = processes(proc_root);
    let mut current = pid;
    for _ in 0..16 {
        // Children, newest (highest pid) first: the shell the person just opened or the program they just ran.
        let next = all
            .iter()
            .filter(|(_, parent)| *parent == current)
            .map(|(p, _)| *p)
            .max();
        match next {
            Some(child) => current = child,
            None => break,
        }
    }
    let cwd = std::fs::read_link(proc_root.join(current.to_string()).join("cwd")).ok()?;
    cwd.is_dir().then_some(cwd)
}

/// The pid of the focused window, from `niri msg --json focused-window`.
pub fn focused_pid() -> Option<u32> {
    let out = std::process::Command::new("niri")
        .args(["msg", "--json", "focused-window"])
        .output()
        .ok()?;
    let value: serde_json::Value = serde_json::from_slice(&out.stdout).ok()?;
    value
        .get("pid")
        .and_then(serde_json::Value::as_u64)
        .map(|p| p as u32)
}

/// A folder that is worth starting an agent in: not the root, not a system folder. (The home folder itself is allowed: an agent started there is the
/// person's choice, and the box shows it as the project.)
pub fn usable(dir: &Path, home: &Path) -> bool {
    dir.is_absolute()
        && dir != Path::new("/")
        && (dir.starts_with(home)
            || dir.starts_with("/tmp")
            || dir.starts_with("/srv")
            || dir.starts_with("/mnt")
            || dir.starts_with("/media")
            || dir.starts_with("/opt"))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fake(root: &Path, pid: u32, parent: u32, name: &str, cwd: &Path) {
        let dir = root.join(pid.to_string());
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(
            dir.join("stat"),
            format!("{pid} ({name}) S {parent} 1 1 0\n"),
        )
        .unwrap();
        let _ = std::os::unix::fs::symlink(cwd, dir.join("cwd"));
    }

    #[test]
    fn the_deepest_newest_descendant_decides() {
        let base = std::env::temp_dir().join(format!("helix-focus-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&base);
        let (proc_root, project, other) =
            (base.join("proc"), base.join("project"), base.join("other"));
        for d in [&proc_root, &project, &other] {
            std::fs::create_dir_all(d).unwrap();
        }
        // terminal 100 → shell 101 (in `other`) and a newer shell 105 (in `project`) → editor 106 (in `project`) with a name that has parentheses.
        fake(&proc_root, 100, 1, "terminal", &base);
        fake(&proc_root, 101, 100, "bash", &other);
        fake(&proc_root, 105, 100, "bash", &project);
        fake(&proc_root, 106, 105, "vim (odd name)", &project);
        assert_eq!(working_directory(&proc_root, 100), Some(project.clone()));
        // A window with no children is its own folder; a missing one is nothing.
        assert_eq!(working_directory(&proc_root, 101), Some(other));
        assert_eq!(working_directory(&proc_root, 999), None);
        let _ = std::fs::remove_dir_all(&base);
    }

    #[test]
    fn only_sensible_folders_are_offered() {
        let home = Path::new("/home/a");
        assert!(usable(Path::new("/home/a/work"), home));
        assert!(usable(Path::new("/tmp/x"), home));
        assert!(!usable(Path::new("/"), home));
        assert!(!usable(Path::new("/usr/lib"), home));
        assert!(!usable(Path::new("relative"), home));
    }
}
