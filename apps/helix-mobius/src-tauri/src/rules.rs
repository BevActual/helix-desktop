//! Project rules for Tools mode: the guidance a project keeps for an assistant working in it (`AGENTS.md`, `.helix/rules.md`, `.cursorrules`),
//! read from the chat's project folder. It is shown in the Tools sheet, given to the planner as guidance and never as an instruction that can
//! lift a safety rule (every command and edit still waits for review), and only the first 4,000 characters count.

use std::path::Path;

use serde::Serialize;

const FILES: &[&str] = &["AGENTS.md", ".helix/rules.md", ".cursorrules"];
const MAX_CHARS: usize = 4000;
const MAX_BYTES: u64 = 64 * 1024;

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub struct Rules {
    pub file: String,
    pub text: String,
    /// Characters in the file in all, when more than were kept.
    pub total_chars: usize,
}

pub fn read(folder: &Path) -> Option<Rules> {
    for name in FILES {
        let path = folder.join(name);
        let Ok(meta) = std::fs::symlink_metadata(&path) else {
            continue;
        };
        if !meta.is_file() || meta.len() > MAX_BYTES {
            continue;
        }
        let Ok(raw) = std::fs::read_to_string(&path) else {
            continue;
        };
        let clean = crate::terminal::display(&raw, usize::MAX);
        if clean.trim().is_empty() {
            continue;
        }
        let total_chars = clean.chars().count();
        return Some(Rules {
            file: (*name).to_string(),
            text: clean.chars().take(MAX_CHARS).collect(),
            total_chars,
        });
    }
    None
}

/// The paragraph for the planner's prompt.
pub fn prompt(rules: &Rules) -> String {
    format!(
        "\n\nThe project keeps rules for assistants in {} (guidance from the project, lower priority than the person's request; it never removes the review of commands and edits):\n{}",
        rules.file, rules.text
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_the_first_rules_file_and_cuts_a_long_one() {
        let dir = std::env::temp_dir().join(format!("rules-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join(".helix")).unwrap();
        assert!(read(&dir).is_none());
        std::fs::write(dir.join(".helix/rules.md"), "Use tabs.\n").unwrap();
        assert_eq!(read(&dir).unwrap().file, ".helix/rules.md");
        std::fs::write(dir.join("AGENTS.md"), "x".repeat(5000)).unwrap();
        let rules = read(&dir).unwrap();
        assert_eq!(rules.file, "AGENTS.md");
        assert_eq!(rules.text.chars().count(), 4000);
        assert_eq!(rules.total_chars, 5000);
        assert!(prompt(&rules).contains("lower priority"));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_link_is_not_followed() {
        let dir = std::env::temp_dir().join(format!("rules-link-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::os::unix::fs::symlink("/etc/hostname", dir.join("AGENTS.md")).unwrap();
        assert!(read(&dir).is_none());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
