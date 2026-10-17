//! Memory: short facts the person asked Mobius to remember across chats ("I use Fedora with niri", "answer in British English").
//!
//! Only the person adds them (`/remember`, or the Memory sheet), the list is always visible and editable, it lives in one file on this
//! computer, and it is added to the system prompt only when the model runs on this computer, so nothing in it reaches a cloud model.

use serde::{Deserialize, Serialize};

pub const MAX_ITEMS: usize = 40;
pub const MAX_ITEM_CHARS: usize = 300;
/// What the system prompt may carry of it, so a long list cannot crowd out a small model's window.
pub const MAX_PROMPT_CHARS: usize = 1500;

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Item {
    pub id: u64,
    pub text: String,
    pub created: u64,
}

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Memory {
    #[serde(default)]
    pub items: Vec<Item>,
}

impl Memory {
    pub fn load(path: &std::path::Path) -> Memory {
        std::fs::read_to_string(path)
            .ok()
            .and_then(|t| serde_json::from_str(&t).ok())
            .unwrap_or_default()
    }

    pub fn save(&self, path: &std::path::Path) -> Result<(), String> {
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
        }
        std::fs::write(
            path,
            serde_json::to_string_pretty(self).map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())
    }

    /// Adds a fact (whitespace squeezed, hidden control characters refused). An identical fact is not added twice.
    pub fn add(&mut self, text: &str, now: u64) -> Result<(), String> {
        let text = text.split_whitespace().collect::<Vec<_>>().join(" ");
        if text.is_empty() {
            return Err("Write what to remember.".into());
        }
        if text.chars().count() > MAX_ITEM_CHARS {
            return Err(format!("Keep a memory under {MAX_ITEM_CHARS} characters."));
        }
        if text.chars().any(char::is_control) {
            return Err("A memory cannot contain control characters.".into());
        }
        if self
            .items
            .iter()
            .any(|i| i.text.eq_ignore_ascii_case(&text))
        {
            return Ok(());
        }
        if self.items.len() >= MAX_ITEMS {
            return Err(format!(
                "{MAX_ITEMS} memories are already kept. Remove one first."
            ));
        }
        let id = self.items.iter().map(|i| i.id).max().unwrap_or(0) + 1;
        self.items.push(Item {
            id,
            text,
            created: now,
        });
        Ok(())
    }

    pub fn remove(&mut self, id: u64) -> bool {
        let before = self.items.len();
        self.items.retain(|i| i.id != id);
        self.items.len() != before
    }

    /// The paragraph for the system prompt, or `None` with nothing remembered. Newest facts win when the list is longer than the allowance.
    pub fn prompt_text(&self) -> Option<String> {
        if self.items.is_empty() {
            return None;
        }
        let mut lines: Vec<String> = Vec::new();
        let mut used = 0;
        for item in self.items.iter().rev() {
            let line = format!("- {}", item.text);
            if used + line.chars().count() > MAX_PROMPT_CHARS {
                break;
            }
            used += line.chars().count() + 1;
            lines.push(line);
        }
        lines.reverse();
        Some(format!("\n\nThings the person has asked you to remember (use them when relevant; do not mention this list):\n{}", lines.join("\n")))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn adds_squeezes_and_does_not_duplicate() {
        let mut m = Memory::default();
        m.add("  I use   Fedora ", 1).unwrap();
        m.add("i use fedora", 2).unwrap();
        assert_eq!(m.items.len(), 1);
        assert_eq!(m.items[0].text, "I use Fedora");
        assert!(m.add("", 3).is_err());
        assert!(m.add(&"x".repeat(301), 3).is_err());
        assert!(m.add("bad\u{7}", 3).is_err());
    }

    #[test]
    fn removes_by_id_and_caps_the_count() {
        let mut m = Memory::default();
        for i in 0..MAX_ITEMS {
            m.add(&format!("fact {i}"), 1).unwrap();
        }
        assert!(m.add("one more", 1).is_err());
        assert!(m.remove(3));
        assert!(!m.remove(3));
        m.add("one more", 1).unwrap();
        assert_eq!(m.items.last().unwrap().id, MAX_ITEMS as u64 + 1);
    }

    #[test]
    fn the_prompt_keeps_the_newest_within_its_allowance() {
        assert!(Memory::default().prompt_text().is_none());
        let mut m = Memory::default();
        for i in 0..40 {
            m.add(&format!("{i:02} {}", "word ".repeat(12)), 1).unwrap();
        }
        let text = m.prompt_text().unwrap();
        assert!(text.contains("39 word"));
        assert!(!text.contains("- 00 word"));
        assert!(text.chars().count() < MAX_PROMPT_CHARS + 200);
    }

    #[test]
    fn round_trips_through_a_file() {
        let path = std::env::temp_dir().join(format!("memory-{}.json", std::process::id()));
        let mut m = Memory::default();
        m.add("British English", 5).unwrap();
        m.save(&path).unwrap();
        assert_eq!(Memory::load(&path), m);
        let _ = std::fs::remove_file(path);
        assert!(
            Memory::load(std::path::Path::new("/nonexistent/memory.json"))
                .items
                .is_empty()
        );
    }
}
