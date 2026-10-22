//! The account and session the last login used, so the next login screen offers them first. It is the greeter user's own small file
//! (`~/.local/state/helix-greeter/last.json`, or `HELIX_GREETER_REMEMBER`); nothing in it is a secret. Reading is lenient and anything it
//! names is only used when the greeter itself lists it.

use serde::{Deserialize, Serialize};
use std::path::PathBuf;

#[derive(Debug, Default, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Last {
    #[serde(default)]
    pub user: String,
    #[serde(default)]
    pub session: String,
}

fn path() -> Option<PathBuf> {
    if let Some(explicit) = std::env::var_os("HELIX_GREETER_REMEMBER") {
        return Some(PathBuf::from(explicit));
    }
    let base = std::env::var_os("XDG_STATE_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/state")))?;
    Some(base.join("helix-greeter/last.json"))
}

pub fn parse(text: &str) -> Last {
    if text.len() > 4096 {
        return Last::default();
    }
    serde_json::from_str(text).unwrap_or_default()
}

pub fn load() -> Last {
    path()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .map(|text| parse(&text))
        .unwrap_or_default()
}

/// Writes what the login used. A failure is ignored: not remembering must never stop a login.
pub fn save(last: &Last) {
    let Some(path) = path() else { return };
    if let Some(dir) = path.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let temporary = path.with_extension("json.tmp");
    if let Ok(text) = serde_json::to_string(last) {
        if std::fs::write(&temporary, text).is_ok() {
            let _ = std::fs::rename(&temporary, &path);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn what_was_saved_comes_back_and_rubbish_is_nothing() {
        let last = Last {
            user: "ada".into(),
            session: "helix".into(),
        };
        assert_eq!(parse(&serde_json::to_string(&last).unwrap()), last);
        assert_eq!(parse("nonsense"), Last::default());
        assert_eq!(
            parse(r#"{"user":"ada"}"#),
            Last {
                user: "ada".into(),
                session: String::new()
            }
        );
        assert_eq!(parse(&"x".repeat(5000)), Last::default());
    }
}
