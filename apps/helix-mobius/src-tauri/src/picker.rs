//! Which models the chat's model menu leaves out. The person turns models off in Settings (a list with a switch each, like the model lists of
//! other editors); the menu then shows the rest. Keys are `local:<file>` and `chatgpt:<id>`. OpenRouter models are not here: pinning one is
//! what puts it in the menu (`cloud.rs`).

use std::path::PathBuf;

const MAX_KEYS: usize = 400;

fn path() -> PathBuf {
    crate::data_home().join("helix/mobius/picker-hidden.json")
}

pub fn hidden() -> Vec<String> {
    std::fs::read_to_string(path())
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default()
}

fn valid(key: &str) -> bool {
    let Some((source, id)) = key.split_once(':') else {
        return false;
    };
    matches!(source, "local" | "chatgpt")
        && !id.is_empty()
        && id.chars().count() <= 200
        && !id.chars().any(char::is_control)
}

/// Shows or hides one model in the menu. Returns the hidden keys now.
pub fn set_shown(key: &str, shown: bool) -> Result<Vec<String>, String> {
    if !valid(key) {
        return Err("That is not a model of this computer or the ChatGPT plan.".into());
    }
    let mut list = hidden();
    list.retain(|k| k != key);
    if !shown {
        if list.len() >= MAX_KEYS {
            return Err("That is more hidden models than Mobius keeps track of.".into());
        }
        list.push(key.to_string());
    }
    let file = path();
    if let Some(dir) = file.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    std::fs::write(
        &file,
        serde_json::to_string(&list).map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())?;
    Ok(list)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keys_are_checked() {
        assert!(valid("local:Qwen3-4B.gguf"));
        assert!(valid("chatgpt:gpt-5.5"));
        assert!(!valid("openrouter:x/y"));
        assert!(!valid("local:"));
        assert!(!valid("nocolon"));
        assert!(!valid("local:a\nb"));
    }
}
