//! Reviewed file edits for Tools mode: one exact replacement in one file of the chat's project folder, shown as a diff before it is applied,
//! with a restore point kept so it can be undone. The model names a path inside the folder, the exact text to replace (empty to create a new
//! file) and the new text; nothing else is writable through it.

use std::path::{Component, Path, PathBuf};

use serde::{Deserialize, Serialize};

pub const MAX_FILE_BYTES: u64 = 512 * 1024;
pub const MAX_TEXT_CHARS: usize = 20_000;

/// What an applied edit needs to be undone: the file, what it held before (`None` = it did not exist) and what the edit left in it.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Checkpoint {
    pub path: PathBuf,
    pub before: Option<String>,
    pub after: String,
    /// False when the change left no file (it was deleted): undo then expects the file to still be absent.
    #[serde(default = "yes")]
    pub after_present: bool,
}

fn yes() -> bool {
    true
}

/// A path inside `root` that the edit tool may touch: relative, no `..`, no hidden file or folder (so no `.git`, `.ssh`), not a file that
/// looks like it holds credentials, and not reaching outside the folder through a link.
pub fn resolve(root: &Path, rel: &str) -> Result<PathBuf, String> {
    let rel_path = Path::new(rel.trim());
    if rel.trim().is_empty() || rel_path.is_absolute() {
        return Err("Give the file's path relative to the project folder.".into());
    }
    for part in rel_path.components() {
        match part {
            Component::Normal(name) => {
                if name.to_string_lossy().starts_with('.') {
                    return Err("Hidden files and folders are not edited.".into());
                }
            }
            Component::CurDir => {}
            _ => return Err("The path must stay inside the project folder.".into()),
        }
    }
    let root = root
        .canonicalize()
        .map_err(|e| format!("The project folder is not available: {e}"))?;
    let target = root.join(rel_path);
    // The folder the file lives in must exist and, once links are followed, still be inside the project folder.
    let parent = target.parent().ok_or("Give a file path.")?;
    let parent = parent
        .canonicalize()
        .map_err(|_| "The file's folder does not exist.".to_string())?;
    if !parent.starts_with(&root) {
        return Err("The path must stay inside the project folder.".into());
    }
    let target = parent.join(target.file_name().ok_or("Give a file path.")?);
    if let Ok(meta) = std::fs::symlink_metadata(&target) {
        if meta.file_type().is_symlink() {
            return Err("That path is a link, so it is not edited.".into());
        }
    }
    if crate::attach::is_secret_file(&target, "") {
        return Err("That file looks like it holds credentials, so it is not edited.".into());
    }
    Ok(target)
}

/// Works out the edit without touching anything: the checkpoint it would leave.
pub fn plan(root: &Path, rel: &str, old: &str, new: &str) -> Result<Checkpoint, String> {
    let target = resolve(root, rel)?;
    if old.chars().count() > MAX_TEXT_CHARS || new.chars().count() > MAX_TEXT_CHARS {
        return Err("An edit is limited to 20,000 characters of text on each side.".into());
    }
    if old.is_empty() {
        if target.exists() {
            return Err("That file already exists. Give the exact text to replace.".into());
        }
        return Ok(Checkpoint {
            path: target,
            before: None,
            after: new.to_string(),
            after_present: true,
        });
    }
    let meta = std::fs::metadata(&target).map_err(|_| "That file does not exist.".to_string())?;
    if !meta.is_file() || meta.len() > MAX_FILE_BYTES {
        return Err("That is not a text file small enough to edit.".into());
    }
    let before = std::fs::read_to_string(&target)
        .map_err(|_| "That file is not readable text.".to_string())?;
    match before.matches(old).count() {
        0 => Err("The text to replace was not found in the file. Copy it exactly.".into()),
        1 => {
            let after = before.replacen(old, new, 1);
            Ok(Checkpoint {
                path: target,
                before: Some(before),
                after,
                after_present: true,
            })
        }
        n => Err(format!(
            "The text to replace appears {n} times. Include more of the lines around it."
        )),
    }
}

/// Writes the planned edit after saving its checkpoint to `store` (a file the undo reads).
pub fn apply(checkpoint: &Checkpoint, store: &Path) -> Result<(), String> {
    if let Some(dir) = store.parent() {
        std::fs::create_dir_all(dir).map_err(|e| format!("Could not save a restore point: {e}"))?;
    }
    let body = serde_json::to_string(checkpoint).map_err(|e| e.to_string())?;
    std::fs::write(store, body).map_err(|e| format!("Could not save a restore point: {e}"))?;
    std::fs::write(&checkpoint.path, &checkpoint.after)
        .map_err(|e| format!("Could not write the file: {e}"))
}

/// Puts the file back as it was, only if it still holds what the edit left (a later change by the person is not overwritten).
pub fn undo(store: &Path) -> Result<PathBuf, String> {
    let body = std::fs::read_to_string(store)
        .map_err(|_| "There is no restore point for that edit.".to_string())?;
    let checkpoint: Checkpoint =
        serde_json::from_str(&body).map_err(|_| "The restore point is damaged.".to_string())?;
    let current = std::fs::read_to_string(&checkpoint.path).ok();
    let expected = checkpoint
        .after_present
        .then_some(checkpoint.after.as_str());
    if current.as_deref() != expected {
        return Err("The file has changed since this edit, so it was left alone.".into());
    }
    match &checkpoint.before {
        Some(before) => std::fs::write(&checkpoint.path, before).map_err(|e| e.to_string())?,
        None => std::fs::remove_file(&checkpoint.path).map_err(|e| e.to_string())?,
    }
    let _ = std::fs::remove_file(store);
    Ok(checkpoint.path)
}

/// The diff the person reviews: the path, then every line of the old text with `- ` and of the new text with `+ `.
pub fn diff_text(rel: &str, old: &str, new: &str) -> String {
    let mut out = format!(
        "{}{}\n",
        if old.is_empty() { "new file " } else { "" },
        rel.trim()
    );
    for line in old.lines() {
        out.push_str(&format!("- {line}\n"));
    }
    for line in new.lines() {
        out.push_str(&format!("+ {line}\n"));
    }
    out
}

/// One file a turn changed, as the Changes card shows it.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct FileChange {
    pub path: String,
    /// `added`, `modified` or `deleted`.
    pub kind: String,
    pub diff: String,
    pub added: usize,
    pub removed: usize,
    #[serde(default)]
    pub reverted: bool,
}

/// The text files of a project folder (relative path to content), bounded: 2,000 files, 512 KiB each, 30 MB in all, six levels deep, no
/// hidden or build folders. What a turn is compared against to say what it changed.
pub fn snapshot(root: &Path) -> std::collections::BTreeMap<String, String> {
    const SKIP: &[&str] = &[
        "node_modules",
        "target",
        "dist",
        "build",
        "__pycache__",
        "vendor",
    ];
    let mut out = std::collections::BTreeMap::new();
    let mut total = 0u64;
    let mut stack = vec![(root.to_path_buf(), 0usize)];
    while let Some((dir, depth)) = stack.pop() {
        for entry in std::fs::read_dir(&dir).into_iter().flatten().flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            if name.starts_with('.') {
                continue;
            }
            let Ok(kind) = entry.file_type() else {
                continue;
            };
            let path = entry.path();
            if kind.is_dir() {
                if depth < 6 && !SKIP.contains(&name.as_str()) {
                    stack.push((path, depth + 1));
                }
            } else if kind.is_file() && out.len() < 2000 {
                let Ok(meta) = entry.metadata() else { continue };
                if meta.len() > MAX_FILE_BYTES || total + meta.len() > 30 * 1024 * 1024 {
                    continue;
                }
                if let Ok(text) = std::fs::read_to_string(&path) {
                    total += meta.len();
                    out.insert(
                        path.strip_prefix(root)
                            .unwrap_or(&path)
                            .to_string_lossy()
                            .into_owned(),
                        text,
                    );
                }
            }
        }
    }
    out
}

/// What changed in `root` since `before` was taken: one entry per file, with a unified diff (cut at 8,000 characters) and the checkpoint that
/// reverts it.
pub fn changes_since(
    root: &Path,
    before: &std::collections::BTreeMap<String, String>,
) -> Vec<(FileChange, Checkpoint)> {
    let after = snapshot(root);
    let mut out = Vec::new();
    let paths: std::collections::BTreeSet<&String> = before.keys().chain(after.keys()).collect();
    for path in paths {
        let (old, new) = (before.get(path), after.get(path));
        if old == new {
            continue;
        }
        let (kind, old_text, new_text) = match (old, new) {
            (None, Some(n)) => ("added", "", n.as_str()),
            (Some(o), None) => ("deleted", o.as_str(), ""),
            (Some(o), Some(n)) => ("modified", o.as_str(), n.as_str()),
            (None, None) => continue,
        };
        let diff = similar::TextDiff::from_lines(old_text, new_text);
        let (mut added, mut removed) = (0, 0);
        for change in diff.iter_all_changes() {
            match change.tag() {
                similar::ChangeTag::Insert => added += 1,
                similar::ChangeTag::Delete => removed += 1,
                similar::ChangeTag::Equal => {}
            }
        }
        let text: String = diff
            .unified_diff()
            .context_radius(2)
            .to_string()
            .chars()
            .take(8000)
            .collect();
        let abs = root.join(path);
        out.push((
            FileChange {
                path: path.clone(),
                kind: kind.into(),
                diff: text,
                added,
                removed,
                reverted: false,
            },
            Checkpoint {
                path: abs,
                before: old.cloned(),
                after: new.cloned().unwrap_or_default(),
                after_present: new.is_some(),
            },
        ));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn folder() -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "edits-{}-{:?}",
            std::process::id(),
            std::thread::current().id()
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join("src")).unwrap();
        dir
    }

    #[test]
    fn replaces_one_exact_match_and_undoes_it() {
        let root = folder();
        std::fs::write(root.join("src/a.rs"), "fn a() {}\nfn b() {}\n").unwrap();
        let cp = plan(&root, "src/a.rs", "fn b() {}", "fn b() { 1 }").unwrap();
        assert_eq!(cp.after, "fn a() {}\nfn b() { 1 }\n");
        let store = root.join("cp.json");
        apply(&cp, &store).unwrap();
        assert_eq!(
            std::fs::read_to_string(root.join("src/a.rs")).unwrap(),
            cp.after
        );
        undo(&store).unwrap();
        assert_eq!(
            std::fs::read_to_string(root.join("src/a.rs")).unwrap(),
            "fn a() {}\nfn b() {}\n"
        );
    }

    #[test]
    fn refuses_what_is_not_exact_or_not_inside() {
        let root = folder();
        std::fs::write(root.join("a.txt"), "x x").unwrap();
        assert!(plan(&root, "a.txt", "x", "y")
            .unwrap_err()
            .contains("2 times"));
        assert!(plan(&root, "a.txt", "zzz", "y")
            .unwrap_err()
            .contains("not found"));
        assert!(plan(&root, "../outside.txt", "", "y").is_err());
        assert!(plan(&root, "/etc/passwd", "", "y").is_err());
        assert!(plan(&root, ".git/config", "", "y").is_err());
        assert!(plan(&root, "nodir/x.txt", "", "y").is_err());
        assert!(plan(&root, "a.txt", "", "y")
            .unwrap_err()
            .contains("already exists"));
        std::os::unix::fs::symlink("/etc/hostname", root.join("link.txt")).unwrap();
        assert!(plan(&root, "link.txt", "a", "b").is_err());
    }

    #[test]
    fn creates_a_file_and_undo_removes_it_but_not_a_changed_one() {
        let root = folder();
        let cp = plan(&root, "src/new.txt", "", "hello\n").unwrap();
        let store = root.join("cp.json");
        apply(&cp, &store).unwrap();
        assert!(root.join("src/new.txt").exists());
        std::fs::write(root.join("src/new.txt"), "edited by hand\n").unwrap();
        assert!(undo(&store).unwrap_err().contains("changed"));
        assert!(root.join("src/new.txt").exists());
        std::fs::write(root.join("src/new.txt"), "hello\n").unwrap();
        undo(&store).unwrap();
        assert!(!root.join("src/new.txt").exists());
    }

    #[test]
    fn the_diff_marks_each_line() {
        assert_eq!(diff_text("a.rs", "x\ny", "z"), "a.rs\n- x\n- y\n+ z\n");
        assert!(diff_text("n.rs", "", "z").starts_with("new file n.rs"));
    }

    #[test]
    fn says_what_changed_and_reverts_each_file() {
        let root = folder();
        std::fs::write(root.join("keep.txt"), "same\n").unwrap();
        std::fs::write(root.join("edit.txt"), "one\ntwo\n").unwrap();
        std::fs::write(root.join("gone.txt"), "bye\n").unwrap();
        let before = snapshot(&root);
        std::fs::write(root.join("edit.txt"), "one\nTWO\nthree\n").unwrap();
        std::fs::remove_file(root.join("gone.txt")).unwrap();
        std::fs::write(root.join("new.txt"), "hi\n").unwrap();
        std::fs::create_dir_all(root.join(".git")).unwrap();
        std::fs::write(root.join(".git/x"), "ignored").unwrap();
        let changes = changes_since(&root, &before);
        let kinds: Vec<_> = changes
            .iter()
            .map(|(c, _)| (c.path.as_str(), c.kind.as_str(), c.added, c.removed))
            .collect();
        assert_eq!(
            kinds,
            [
                ("edit.txt", "modified", 2, 1),
                ("gone.txt", "deleted", 0, 1),
                ("new.txt", "added", 1, 0)
            ]
        );
        assert!(changes[0].0.diff.contains("-two") && changes[0].0.diff.contains("+TWO"));
        for (i, (_, cp)) in changes.iter().enumerate() {
            let store = root.join(format!("cp{i}.json"));
            std::fs::write(&store, serde_json::to_string(cp).unwrap()).unwrap();
            undo(&store).unwrap();
        }
        assert_eq!(
            std::fs::read_to_string(root.join("edit.txt")).unwrap(),
            "one\ntwo\n"
        );
        assert_eq!(
            std::fs::read_to_string(root.join("gone.txt")).unwrap(),
            "bye\n"
        );
        assert!(!root.join("new.txt").exists());
    }
}
