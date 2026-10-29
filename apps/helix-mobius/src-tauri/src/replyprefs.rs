//! How replies are written, apart from the routing file: how much a reply says, and whether reasoning is shown.
//! A style chosen for one chat replaces the detail line. The page draws reasoning; this only remembers that choice.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ReplyPrefs {
    pub detail: String,
    pub thinking: String,
}

impl Default for ReplyPrefs {
    fn default() -> Self {
        Self {
            detail: "balanced".into(),
            thinking: "folded".into(),
        }
    }
}

fn file() -> PathBuf {
    crate::data_home().join("helix/mobius/reply-prefs.json")
}

pub fn load() -> ReplyPrefs {
    read(&file())
}

pub fn save(detail: &str, thinking: &str) -> Result<ReplyPrefs, String> {
    write(&file(), detail, thinking)
}

/// The line added when this chat has no style of its own. Balanced adds nothing.
pub fn detail_instruction(detail: &str) -> &'static str {
    match detail {
        "concise" => "Answer briefly. Lead with the result, and skip preamble and recap.",
        "detailed" => "Answer thoroughly. Include what a reader needs to follow the result, and name a trade-off when more than one choice is reasonable.",
        _ => "",
    }
}

fn known<'a>(value: &'a str, allowed: &[&str], fallback: &'a str) -> &'a str {
    if allowed.contains(&value) {
        value
    } else {
        fallback
    }
}

fn normalize(detail: &str, thinking: &str) -> Result<ReplyPrefs, String> {
    if !matches!(detail, "concise" | "balanced" | "detailed") {
        return Err("Choose concise, balanced or detailed.".into());
    }
    if !matches!(thinking, "open" | "folded" | "hidden") {
        return Err("Choose open, folded or hidden.".into());
    }
    Ok(ReplyPrefs {
        detail: detail.to_string(),
        thinking: thinking.to_string(),
    })
}

fn read(path: &Path) -> ReplyPrefs {
    let Ok(text) = std::fs::read_to_string(path) else {
        return ReplyPrefs::default();
    };
    let Ok(raw) = serde_json::from_str::<ReplyPrefs>(&text) else {
        return ReplyPrefs::default();
    };
    ReplyPrefs {
        detail: known(
            &raw.detail,
            &["concise", "balanced", "detailed"],
            "balanced",
        )
        .to_string(),
        thinking: known(&raw.thinking, &["open", "folded", "hidden"], "folded").to_string(),
    }
}

fn write(path: &Path, detail: &str, thinking: &str) -> Result<ReplyPrefs, String> {
    let prefs = normalize(detail, thinking)?;
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let text = serde_json::to_string(&prefs).map_err(|e| e.to_string())?;
    std::fs::write(path, text).map_err(|e| e.to_string())?;
    Ok(prefs)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detail_is_a_line_only_at_the_ends() {
        assert!(detail_instruction("balanced").is_empty());
        assert!(detail_instruction("concise").contains("briefly"));
        assert!(detail_instruction("detailed").contains("thoroughly"));
        assert!(detail_instruction("nope").is_empty());
    }

    #[test]
    fn saves_only_known_choices_and_keeps_the_last_good_file() {
        let dir = std::env::temp_dir().join(format!("helix-reply-prefs-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("reply-prefs.json");
        assert_eq!(read(&path), ReplyPrefs::default());
        let saved = write(&path, "concise", "hidden").unwrap();
        assert_eq!(saved.detail, "concise");
        assert_eq!(read(&path).thinking, "hidden");
        assert!(write(&path, "loud", "folded").is_err());
        assert_eq!(read(&path).detail, "concise");
        std::fs::write(&path, "{\"detail\":\"sideways\",\"thinking\":\"open\"}").unwrap();
        let cleaned = read(&path);
        assert_eq!(cleaned.detail, "balanced");
        assert_eq!(cleaned.thinking, "open");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
