//! Other answers to the same question: when a reply is redone, the one it replaces is kept here (up to three), so two models, or two tries,
//! can be read side by side and the better one kept. One small file per chat, beside the transcript; never sent to a model.

use std::collections::BTreeMap;
use std::path::PathBuf;

use serde::{Deserialize, Serialize};

pub const MAX_PER_REPLY: usize = 3;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Alt {
    pub text: String,
    pub model: String,
    pub tokens: u32,
    pub ms: u64,
    pub at: u64,
    /// The question it answers (the message before it), so an answer is never shown against a different question.
    #[serde(default)]
    pub ask: String,
}

/// Message index (as text) to its other answers, newest first.
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct Store {
    #[serde(default)]
    pub replies: BTreeMap<String, Vec<Alt>>,
}

fn file(chat_id: &str) -> PathBuf {
    let safe: String = chat_id
        .chars()
        .filter(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_')
        .collect();
    crate::data_home()
        .join("helix/mobius/alternates")
        .join(format!("{safe}.json"))
}

impl Store {
    pub fn load(chat_id: &str) -> Store {
        std::fs::read_to_string(file(chat_id))
            .ok()
            .and_then(|t| serde_json::from_str(&t).ok())
            .unwrap_or_default()
    }

    pub fn save(&self, chat_id: &str) -> Result<(), String> {
        let path = file(chat_id);
        if self.replies.values().all(Vec::is_empty) {
            let _ = std::fs::remove_file(path);
            return Ok(());
        }
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
        }
        std::fs::write(
            path,
            serde_json::to_string(self).map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())
    }

    /// Keeps `alt` as another answer to message `index` (the newest first; an identical text is not kept twice; at most three).
    pub fn push(&mut self, index: usize, alt: Alt) {
        let list = self.replies.entry(index.to_string()).or_default();
        list.retain(|a| a.text != alt.text);
        list.insert(0, alt);
        list.truncate(MAX_PER_REPLY);
    }

    pub fn list(&self, index: usize) -> Vec<Alt> {
        self.replies
            .get(&index.to_string())
            .cloned()
            .unwrap_or_default()
    }

    /// Takes answer `which` out of the list (the caller puts the current answer in its place).
    pub fn take(&mut self, index: usize, which: usize) -> Option<Alt> {
        let list = self.replies.get_mut(&index.to_string())?;
        (which < list.len()).then(|| list.remove(which))
    }
}

/// The question a reply answers: the message before it.
fn ask_of(messages: &[helix_chat::chat::Message], index: usize) -> String {
    index
        .checked_sub(1)
        .and_then(|i| messages.get(i))
        .map(|m| m.text.chars().take(300).collect())
        .unwrap_or_default()
}

#[tauri::command]
pub fn alt_list(app: tauri::AppHandle, index: usize) -> Vec<Alt> {
    let shared = crate::shared(&app);
    let state = shared.lock().expect("state");
    let ask = ask_of(&state.chat.conversation.messages, index);
    let chat_id = state.chat.conversation.id.clone();
    drop(state);
    Store::load(&chat_id)
        .list(index)
        .into_iter()
        .filter(|a| a.ask == ask)
        .collect()
}

/// Makes another answer the reply, and keeps the one it replaces as an alternative, so a choice can be changed back.
#[tauri::command]
pub fn alt_keep(app: tauri::AppHandle, index: usize, which: usize) -> Result<(), String> {
    {
        let shared = crate::shared(&app);
        let mut state = shared.lock().expect("state");
        if state.chat.is_streaming() {
            return Err("Wait for the reply to finish, or stop it.".into());
        }
        let chat_id = state.chat.conversation.id.clone();
        let last = state.chat.conversation.messages.len().checked_sub(1);
        if last != Some(index) {
            return Err("Only the latest reply can be swapped.".into());
        }
        let ask = ask_of(&state.chat.conversation.messages, index);
        let mut store = Store::load(&chat_id);
        let chosen = store
            .take(index, which)
            .ok_or("That answer is no longer kept.")?;
        let message = &mut state.chat.conversation.messages[index];
        let before = Alt {
            text: std::mem::replace(&mut message.text, chosen.text),
            model: message
                .stats
                .as_ref()
                .map(|s| s.model.clone())
                .unwrap_or_default(),
            tokens: message.stats.as_ref().map_or(0, |s| s.tokens),
            ms: message.stats.as_ref().map_or(0, |s| s.ms),
            at: crate::now(),
            ask: ask.clone(),
        };
        message.stats = Some(helix_chat::chat::ReplyStats {
            model: chosen.model,
            tokens: chosen.tokens,
            ms: chosen.ms,
            files: Vec::new(),
            cost_micro: None,
        });
        store.push(index, before);
        store.save(&chat_id)?;
        crate::save(&state.chat.conversation);
    }
    crate::emit_view(&app);
    crate::emit_list(&app);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn alt(text: &str) -> Alt {
        Alt {
            text: text.into(),
            model: "m".into(),
            tokens: 1,
            ms: 1,
            at: 1,
            ask: "q".into(),
        }
    }

    #[test]
    fn keeps_the_newest_three_without_repeats() {
        let mut s = Store::default();
        for t in ["a", "b", "c", "d", "c"] {
            s.push(0, alt(t));
        }
        let texts: Vec<_> = s.list(0).into_iter().map(|a| a.text).collect();
        assert_eq!(texts, ["c", "d", "b"]);
        assert!(s.list(1).is_empty());
    }

    #[test]
    fn taking_one_removes_it_and_refuses_what_is_not_there() {
        let mut s = Store::default();
        s.push(2, alt("x"));
        s.push(2, alt("y"));
        assert_eq!(s.take(2, 0).unwrap().text, "y");
        assert_eq!(s.list(2).len(), 1);
        assert!(s.take(2, 5).is_none());
        assert!(s.take(9, 0).is_none());
    }
}
