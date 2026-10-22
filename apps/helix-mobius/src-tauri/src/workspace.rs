//! A private copy of a project for a task to work in, and what becomes of its changes. The copy is a local `git clone` (its own complete `.git`, so
//! git works inside the sandbox) kept outside Mobius's data folder, which project tools refuse. The person's project is untouched until they apply the
//! task's changes; two tasks on the same project cannot collide.

use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

const MAX_DIFF_BYTES: usize = 1_500_000;

pub fn copies_dir() -> PathBuf {
    crate::data_home().join("helix-task-copies")
}

fn git(dir: &Path, args: &[&str]) -> Result<String, String> {
    let output = Command::new("git")
        .arg("-C")
        .arg(dir)
        .args(args)
        .env("GIT_TERMINAL_PROMPT", "0")
        .env("GIT_CONFIG_NOSYSTEM", "1")
        .stdin(Stdio::null())
        .output()
        .map_err(|_| "git is not installed.".to_string())?;
    if output.status.success() {
        Ok(String::from_utf8_lossy(&output.stdout).to_string())
    } else {
        Err(String::from_utf8_lossy(&output.stderr).trim().to_string())
    }
}

/// Whether `folder` is the top of a git repository that has a commit.
pub fn is_repo(folder: &Path) -> bool {
    git(folder, &["rev-parse", "--show-toplevel"])
        .ok()
        .and_then(|top| std::fs::canonicalize(top.trim()).ok())
        .is_some_and(|top| Some(top) == std::fs::canonicalize(folder).ok())
        && git(folder, &["rev-parse", "--verify", "HEAD"]).is_ok()
}

pub struct Copy {
    pub dir: PathBuf,
    pub base: String,
    pub dirty: bool,
}

/// Copies the project at its last commit.
pub fn make_copy(id: &str, source: &Path) -> Result<Copy, String> {
    if !helix_chat::chat::valid_id(id) {
        return Err("Invalid task.".into());
    }
    let dirty = !git(source, &["status", "--porcelain"])?.trim().is_empty();
    let dir = copies_dir().join(id);
    std::fs::create_dir_all(copies_dir()).map_err(|e| e.to_string())?;
    let source_text = source
        .to_str()
        .ok_or("The folder name is not valid UTF-8.")?;
    let dir_text = dir.to_str().ok_or("Invalid copy path.")?;
    git(
        Path::new("/"),
        &[
            "clone",
            "--quiet",
            "--local",
            "--no-checkout",
            source_text,
            dir_text,
        ],
    )?;
    git(&dir, &["checkout", "--quiet", "HEAD"])?;
    git(&dir, &["config", "user.name", "Helix agent"])?;
    git(&dir, &["config", "user.email", "agent@helix.local"])?;
    let base = git(&dir, &["rev-parse", "HEAD"])?.trim().to_string();
    Ok(Copy { dir, base, dirty })
}

fn inside_copies(dir: &Path) -> Result<PathBuf, String> {
    let dir = std::fs::canonicalize(dir).map_err(|_| "The task's copy is gone.".to_string())?;
    let root =
        std::fs::canonicalize(copies_dir()).map_err(|_| "The task's copy is gone.".to_string())?;
    if dir.starts_with(&root) && dir != root {
        Ok(dir)
    } else {
        Err("That is not a task copy.".into())
    }
}

/// Everything the task changed in its copy, committed or not, new files included, as a unified diff against the commit it started from.
pub fn diff(copy: &Path, base: &str, binary: bool) -> Result<String, String> {
    let copy = inside_copies(copy)?;
    if base.is_empty() || !base.chars().all(|c| c.is_ascii_hexdigit()) {
        return Err("The task has no starting point.".into());
    }
    git(&copy, &["add", "-A"])?;
    let mut args = vec!["diff", "--cached", "--no-color", "--no-ext-diff"];
    if binary {
        args.push("--binary");
    }
    args.push(base);
    let mut text = git(&copy, &args)?;
    if text.len() > MAX_DIFF_BYTES && !binary {
        text.truncate(MAX_DIFF_BYTES);
        text.push_str("\n… (the rest of the diff is not shown)\n");
    }
    Ok(text)
}

/// Applies the task's changes to the project's working tree (not staged, not committed), only if they apply cleanly.
pub fn apply(source: &Path, copy: &Path, base: &str) -> Result<usize, String> {
    let patch = diff(copy, base, true)?;
    if patch.trim().is_empty() {
        return Err("The task changed nothing.".into());
    }
    let run = |check: bool| -> Result<(), String> {
        let mut command = Command::new("git");
        command.arg("-C").arg(source).arg("apply");
        if check {
            command.arg("--check");
        }
        let mut child = command
            .arg("-")
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::piped())
            .spawn()
            .map_err(|_| "git is not installed.".to_string())?;
        child
            .stdin
            .take()
            .ok_or("No input pipe.")?
            .write_all(patch.as_bytes())
            .map_err(|e| e.to_string())?;
        let output = child.wait_with_output().map_err(|e| e.to_string())?;
        if output.status.success() {
            Ok(())
        } else {
            Err(format!(
                "The changes do not fit your project as it is now: {}",
                String::from_utf8_lossy(&output.stderr).trim()
            ))
        }
    };
    run(true)?;
    run(false)?;
    Ok(patch
        .lines()
        .filter(|l| l.starts_with("diff --git"))
        .count())
}

pub fn discard(copy: &Path) -> Result<(), String> {
    match inside_copies(copy) {
        Ok(dir) => std::fs::remove_dir_all(dir).map_err(|e| e.to_string()),
        Err(_) => Ok(()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The tests change the process's data folder, so they take turns.
    static TURN: std::sync::Mutex<()> = std::sync::Mutex::new(());

    fn sh(dir: &Path, args: &[&str]) {
        let out = Command::new("git")
            .arg("-C")
            .arg(dir)
            .args(args)
            .env("GIT_CONFIG_NOSYSTEM", "1")
            .output()
            .unwrap();
        assert!(
            out.status.success(),
            "git {args:?}: {}",
            String::from_utf8_lossy(&out.stderr)
        );
    }
    fn repo(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("mobius-ws-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        sh(&dir, &["init", "-q"]);
        sh(&dir, &["config", "user.email", "t@t"]);
        sh(&dir, &["config", "user.name", "t"]);
        std::fs::write(dir.join("a.txt"), "one\ntwo\n").unwrap();
        sh(&dir, &["add", "."]);
        sh(&dir, &["commit", "-qm", "first"]);
        std::fs::canonicalize(dir).unwrap()
    }

    #[test]
    fn a_copy_is_reviewed_applied_and_discarded_without_touching_the_project_first() {
        let _turn = TURN.lock().unwrap_or_else(|e| e.into_inner());
        let data = std::env::temp_dir().join(format!("mobius-ws-data-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&data);
        std::env::set_var("XDG_DATA_HOME", &data);
        let source = repo("flow");
        assert!(is_repo(&source));
        assert!(!is_repo(&source.join("nowhere")));
        let id = "0000000000000a01";
        let copy = make_copy(id, &source).unwrap();
        assert!(!copy.dirty);
        // The task edits, adds and commits in its copy.
        std::fs::write(copy.dir.join("a.txt"), "one\ntwo\nthree\n").unwrap();
        std::fs::write(copy.dir.join("new.txt"), "hello\n").unwrap();
        sh(&copy.dir, &["add", "new.txt"]);
        sh(&copy.dir, &["commit", "-qm", "agent commit"]);
        assert_eq!(
            std::fs::read_to_string(source.join("a.txt")).unwrap(),
            "one\ntwo\n",
            "the project is untouched"
        );
        let text = diff(&copy.dir, &copy.base, false).unwrap();
        assert!(
            text.contains("+three") && text.contains("+hello") && text.contains("new.txt"),
            "{text}"
        );
        assert_eq!(apply(&source, &copy.dir, &copy.base).unwrap(), 2);
        assert_eq!(
            std::fs::read_to_string(source.join("a.txt")).unwrap(),
            "one\ntwo\nthree\n"
        );
        assert_eq!(
            std::fs::read_to_string(source.join("new.txt")).unwrap(),
            "hello\n"
        );
        discard(&copy.dir).unwrap();
        assert!(!copy.dir.exists());
        discard(&copy.dir).unwrap();
    }

    #[test]
    fn changes_that_no_longer_fit_are_refused_and_nothing_is_half_applied() {
        let _turn = TURN.lock().unwrap_or_else(|e| e.into_inner());
        let data = std::env::temp_dir().join(format!("mobius-ws-data2-{}", std::process::id()));
        std::env::set_var("XDG_DATA_HOME", &data);
        let source = repo("conflict");
        let copy = make_copy("0000000000000a02", &source).unwrap();
        std::fs::write(copy.dir.join("a.txt"), "ONE\ntwo\n").unwrap();
        std::fs::write(copy.dir.join("b.txt"), "b\n").unwrap();
        // Meanwhile the person changed the same line.
        std::fs::write(source.join("a.txt"), "uno\ntwo\n").unwrap();
        let error = apply(&source, &copy.dir, &copy.base).unwrap_err();
        assert!(error.contains("do not fit"), "{error}");
        assert!(!source.join("b.txt").exists(), "all or nothing");
        assert_eq!(
            std::fs::read_to_string(source.join("a.txt")).unwrap(),
            "uno\ntwo\n"
        );
    }

    #[test]
    fn uncommitted_changes_are_noted_and_paths_outside_the_copies_are_refused() {
        let _turn = TURN.lock().unwrap_or_else(|e| e.into_inner());
        let data = std::env::temp_dir().join(format!("mobius-ws-data3-{}", std::process::id()));
        std::env::set_var("XDG_DATA_HOME", &data);
        let source = repo("dirty");
        std::fs::write(source.join("a.txt"), "changed\n").unwrap();
        assert!(make_copy("0000000000000a03", &source).unwrap().dirty);
        assert!(diff(&source, "abc", false).is_err(), "not a task copy");
        assert!(
            discard(&source).is_ok() && source.exists(),
            "a project is never removed as a copy"
        );
        assert!(make_copy("../x", &source).is_err());
    }
}
