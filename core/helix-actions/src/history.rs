//! The one history of actions: who did what, what it was before, and Undo.
//!
//! Every change through the catalogue lands here, the person's and an agent's alike, so "what changed and who did it" has one answer
//! and Undo works the same for both. Kept as JSON in `$XDG_STATE_HOME/helix/actions/history.json`, newest last, at most [`KEEP`] entries.
//!
//! Two rules make Undo safe to offer everywhere:
//! - Only the newest change of a thing can be undone. Undoing an older one would silently throw away what came after it.
//! - Quick repeats of the same change by the same actor (dragging a slider) are one entry, whose "before" is where it started.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::{Actor, Undo};

/// How many entries are kept.
pub const KEEP: usize = 500;
/// Changes of the same thing by the same actor within this many seconds are one entry.
pub const MERGE_SECONDS: u64 = 4;

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Entry {
    pub id: u64,
    /// Seconds since the Unix epoch.
    pub at: u64,
    /// The action that ran, e.g. `settings.set`.
    pub action: String,
    /// What it acted on, e.g. `theme.mode`.
    pub target: String,
    /// For people: "Appearance: Dark → Light".
    pub sentence: String,
    pub by: Actor,
    pub before: Value,
    pub after: Value,
    pub undo: Undo,
    /// Set when the entry has been undone, with who did it.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub undone_by: Option<Actor>,
    /// For a provider's action: the call that undoes it (`{"action": "...", "params": {...}}`), as the provider said.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub undo_call: Option<Value>,
}

/// What [`History::undo_plan`] says about an entry.
#[derive(Debug, PartialEq)]
pub enum UndoPlan<'a> {
    /// Put `before` back into `target`.
    Restore(&'a Entry),
    /// Why it cannot be undone.
    Refused(String),
}

#[derive(Debug, Default)]
pub struct History {
    path: Option<PathBuf>,
    entries: Vec<Entry>,
    next: u64,
}

#[derive(Serialize, Deserialize)]
struct File {
    version: u32,
    next: u64,
    entries: Vec<Entry>,
}

impl History {
    /// The history at `path`, empty when the file is missing or unreadable (a broken history never stops an action).
    pub fn load(path: &Path) -> Self {
        let file: Option<File> = std::fs::read_to_string(path)
            .ok()
            .and_then(|text| serde_json::from_str(&text).ok());
        let (entries, next) = file.map_or((Vec::new(), 1), |f| (f.entries, f.next.max(1)));
        Self {
            path: Some(path.to_path_buf()),
            entries,
            next,
        }
    }

    /// A history that is never written (tests, and a desktop service without a state folder).
    pub fn in_memory() -> Self {
        Self {
            next: 1,
            ..Self::default()
        }
    }

    /// Where the desktop keeps it.
    pub fn default_path(state_home: &Path) -> PathBuf {
        state_home
            .join("helix")
            .join("actions")
            .join("history.json")
    }

    /// Records a change and returns its entry's id. A quick repeat by the same actor of the newest entry for the same thing extends
    /// that entry instead (its `before` stays where the change started).
    #[allow(clippy::too_many_arguments)]
    pub fn record(
        &mut self,
        now: u64,
        action: &str,
        target: &str,
        by: &Actor,
        before: Value,
        after: Value,
        undo: Undo,
        sentence: impl Fn(&Value, &Value) -> String,
    ) -> u64 {
        if let Some(last) = self.entries.last_mut() {
            if last.action == action
                && last.target == target
                && &last.by == by
                && last.undone_by.is_none()
                && now.saturating_sub(last.at) <= MERGE_SECONDS
            {
                last.at = now;
                last.after = after;
                last.sentence = sentence(&last.before, &last.after);
                let id = last.id;
                self.save();
                return id;
            }
        }
        let id = self.next;
        self.next += 1;
        self.entries.push(Entry {
            id,
            at: now,
            action: action.into(),
            target: target.into(),
            sentence: sentence(&before, &after),
            by: by.clone(),
            before,
            after,
            undo,
            undone_by: None,
            undo_call: None,
        });
        if self.entries.len() > KEEP {
            let extra = self.entries.len() - KEEP;
            self.entries.drain(..extra);
        }
        self.save();
        id
    }

    /// Records a provider's action that says how it is undone (`undo_call`), or that cannot be (`None`). Never merged with another.
    pub fn record_call(
        &mut self,
        now: u64,
        action: &str,
        by: &Actor,
        sentence: &str,
        undo_call: Option<Value>,
    ) -> u64 {
        let id = self.next;
        self.next += 1;
        self.entries.push(Entry {
            id,
            at: now,
            action: action.into(),
            target: action.into(),
            sentence: sentence.into(),
            by: by.clone(),
            before: Value::Null,
            after: Value::Null,
            undo: if undo_call.is_some() {
                Undo::Restore
            } else {
                Undo::None
            },
            undone_by: None,
            undo_call,
        });
        if self.entries.len() > KEEP {
            let extra = self.entries.len() - KEEP;
            self.entries.drain(..extra);
        }
        self.save();
        id
    }

    /// The newest entries first.
    pub fn recent(&self, limit: usize) -> Vec<&Entry> {
        self.entries.iter().rev().take(limit).collect()
    }

    pub fn get(&self, id: u64) -> Option<&Entry> {
        self.entries.iter().find(|e| e.id == id)
    }

    /// Whether entry `id` may be undone, and what to put back.
    pub fn undo_plan(&self, id: u64) -> UndoPlan<'_> {
        let Some(entry) = self.get(id) else {
            return UndoPlan::Refused("That change is no longer in the history.".into());
        };
        if entry.undo != Undo::Restore {
            return UndoPlan::Refused("That cannot be undone.".into());
        }
        if entry.undone_by.is_some() {
            return UndoPlan::Refused("That change was already undone.".into());
        }
        let newer = self
            .entries
            .iter()
            .any(|e| e.id > entry.id && e.target == entry.target && e.undone_by.is_none());
        if newer {
            return UndoPlan::Refused(
                "It has changed again since; undo the newer change first.".into(),
            );
        }
        UndoPlan::Restore(entry)
    }

    /// Marks entry `id` undone by `by`.
    pub fn mark_undone(&mut self, id: u64, by: &Actor) {
        if let Some(entry) = self.entries.iter_mut().find(|e| e.id == id) {
            entry.undone_by = Some(by.clone());
            self.save();
        }
    }

    fn save(&self) {
        let Some(path) = &self.path else { return };
        let file = File {
            version: 1,
            next: self.next,
            entries: self.entries.clone(),
        };
        let Ok(text) = serde_json::to_string(&file) else {
            return;
        };
        if let Some(dir) = path.parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        let partial = path.with_extension("json.part");
        if std::fs::write(&partial, text).is_ok() {
            let _ = std::fs::rename(&partial, path);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn words(before: &Value, after: &Value) -> String {
        format!("{before} → {after}")
    }

    #[test]
    fn a_change_is_recorded_with_who_did_it_and_can_be_undone() {
        let mut history = History::in_memory();
        let id = history.record(
            100,
            "settings.set",
            "theme.mode",
            &Actor::agent("Mobius"),
            json!("dark"),
            json!("light"),
            Undo::Restore,
            words,
        );
        let entry = history.get(id).unwrap();
        assert_eq!(entry.by.label, "Mobius");
        assert_eq!(entry.sentence, "\"dark\" → \"light\"");
        assert_eq!(
            history.undo_plan(id),
            UndoPlan::Restore(history.get(id).unwrap())
        );
        history.mark_undone(id, &Actor::person());
        assert!(matches!(history.undo_plan(id), UndoPlan::Refused(why) if why.contains("already")));
    }

    #[test]
    fn dragging_a_slider_is_one_change_from_where_it_started() {
        let mut history = History::in_memory();
        let you = Actor::person();
        let a = history.record(
            100,
            "settings.set",
            "dock.icon_size",
            &you,
            json!(52),
            json!(56),
            Undo::Restore,
            words,
        );
        let b = history.record(
            102,
            "settings.set",
            "dock.icon_size",
            &you,
            json!(56),
            json!(64),
            Undo::Restore,
            words,
        );
        assert_eq!(a, b);
        let entry = history.get(a).unwrap();
        assert_eq!(
            (entry.before.clone(), entry.after.clone()),
            (json!(52), json!(64))
        );
        // Later, or by someone else, it is a new entry.
        let c = history.record(
            110,
            "settings.set",
            "dock.icon_size",
            &you,
            json!(64),
            json!(60),
            Undo::Restore,
            words,
        );
        let d = history.record(
            111,
            "settings.set",
            "dock.icon_size",
            &Actor::agent("Mobius"),
            json!(60),
            json!(70),
            Undo::Restore,
            words,
        );
        assert!(c != a && d != c);
        assert_eq!(history.recent(10).len(), 3);
    }

    #[test]
    fn only_the_newest_change_of_a_thing_can_be_undone() {
        let mut history = History::in_memory();
        let first = history.record(
            100,
            "settings.set",
            "theme.mode",
            &Actor::agent("Mobius"),
            json!("dark"),
            json!("light"),
            Undo::Restore,
            words,
        );
        let second = history.record(
            200,
            "settings.set",
            "theme.mode",
            &Actor::person(),
            json!("light"),
            json!("auto"),
            Undo::Restore,
            words,
        );
        assert!(
            matches!(history.undo_plan(first), UndoPlan::Refused(why) if why.contains("changed again"))
        );
        history.mark_undone(second, &Actor::person());
        assert!(matches!(history.undo_plan(first), UndoPlan::Restore(_)));
    }

    #[test]
    fn it_survives_a_restart_and_keeps_counting() {
        let dir = std::env::temp_dir().join(format!("helix-actions-{}", std::process::id()));
        let path = History::default_path(&dir);
        let _ = std::fs::remove_file(&path);
        let mut history = History::load(&path);
        let id = history.record(
            100,
            "settings.set",
            "theme.mode",
            &Actor::person(),
            json!("dark"),
            json!("light"),
            Undo::Restore,
            words,
        );
        let mut again = History::load(&path);
        assert_eq!(again.get(id).unwrap().target, "theme.mode");
        let next = again.record(
            200,
            "settings.set",
            "dock.icon_size",
            &Actor::person(),
            json!(52),
            json!(64),
            Undo::Restore,
            words,
        );
        assert!(next > id);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_providers_action_keeps_how_to_undo_it() {
        let mut history = History::in_memory();
        let call = json!({"action": "notes.delete", "params": {"id": 3}});
        let id = history.record_call(
            100,
            "notes.add",
            &Actor::agent("Mobius"),
            "Added a note",
            Some(call.clone()),
        );
        let entry = history.get(id).unwrap();
        assert_eq!(
            (entry.undo, entry.undo_call.clone()),
            (Undo::Restore, Some(call))
        );
        assert!(matches!(history.undo_plan(id), UndoPlan::Restore(_)));
        let gone = history.record_call(101, "notes.send", &Actor::person(), "Sent a note", None);
        assert!(matches!(history.undo_plan(gone), UndoPlan::Refused(_)));
    }
}
