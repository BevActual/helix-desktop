//! Documents on the stage: what a chat has made and been edited into, kept as versions. One file per chat; the model's blocks and the person's
//! edits both land here, so nothing either writes is lost.

use std::path::PathBuf;

use serde::{Deserialize, Serialize};

pub const MAX_VERSIONS: usize = 20;
pub const MAX_BODY_CHARS: usize = 60_000;
pub const MAX_DOCUMENTS: usize = 30;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Version {
    pub body: String,
    /// `model` or `person`.
    pub by: String,
    pub at: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Document {
    pub title: String,
    pub versions: Vec<Version>,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct Store {
    #[serde(default)]
    pub documents: Vec<Document>,
}

fn file(chat_id: &str) -> PathBuf {
    let safe: String = chat_id
        .chars()
        .filter(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_')
        .collect();
    crate::data_home()
        .join("helix/mobius/stage")
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
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
        }
        std::fs::write(
            path,
            serde_json::to_string(self).map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())
    }

    /// Records a body as the newest version of a document (made if new). The same text as the newest version is not a new version.
    pub fn record(&mut self, title: &str, body: &str, by: &str, at: u64) -> Result<(), String> {
        let title = title.split_whitespace().collect::<Vec<_>>().join(" ");
        let title: String = title.chars().take(80).collect();
        if title.is_empty() {
            return Err("A document needs a title.".into());
        }
        if body.chars().count() > MAX_BODY_CHARS {
            return Err("That document is too long for the stage.".into());
        }
        let index = match self
            .documents
            .iter()
            .position(|d| d.title.eq_ignore_ascii_case(&title))
        {
            Some(i) => i,
            None => {
                if self.documents.len() >= MAX_DOCUMENTS {
                    return Err("This chat already has 30 documents.".into());
                }
                self.documents.push(Document {
                    title,
                    versions: Vec::new(),
                });
                self.documents.len() - 1
            }
        };
        let doc = &mut self.documents[index];
        if doc.versions.last().is_some_and(|v| v.body == body) {
            return Ok(());
        }
        doc.versions.push(Version {
            body: body.to_string(),
            by: by.to_string(),
            at,
        });
        if doc.versions.len() > MAX_VERSIONS {
            let extra = doc.versions.len() - MAX_VERSIONS;
            doc.versions.drain(..extra);
        }
        Ok(())
    }
}

#[tauri::command]
pub fn stage_get(chat_id: String) -> Store {
    Store::load(&chat_id)
}

#[tauri::command]
pub fn stage_record(
    chat_id: String,
    title: String,
    body: String,
    by: String,
) -> Result<Store, String> {
    if chat_id.is_empty() {
        return Err("Send a message first so this chat has somewhere to keep documents.".into());
    }
    let by = if by == "person" { "person" } else { "model" };
    let mut store = Store::load(&chat_id);
    store.record(&title, &body, by, crate::now())?;
    store.save(&chat_id)?;
    Ok(store)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keeps_versions_and_ignores_a_repeat() {
        let mut s = Store::default();
        s.record("Plan", "one", "model", 1).unwrap();
        s.record("plan", "one", "person", 2).unwrap();
        s.record("Plan", "two", "person", 3).unwrap();
        assert_eq!(s.documents.len(), 1);
        assert_eq!(s.documents[0].versions.len(), 2);
        assert_eq!(s.documents[0].versions[1].by, "person");
    }

    #[test]
    fn caps_versions_documents_and_size() {
        let mut s = Store::default();
        for i in 0..25 {
            s.record("Doc", &format!("v{i}"), "model", i).unwrap();
        }
        assert_eq!(s.documents[0].versions.len(), MAX_VERSIONS);
        assert_eq!(s.documents[0].versions[0].body, "v5");
        assert!(s.record("", "x", "model", 1).is_err());
        assert!(s
            .record("Big", &"x".repeat(MAX_BODY_CHARS + 1), "model", 1)
            .is_err());
        for i in 0..MAX_DOCUMENTS {
            let _ = s.record(&format!("d{i}"), "x", "model", 1);
        }
        assert!(s.record("one more", "x", "model", 1).is_err());
    }
}
