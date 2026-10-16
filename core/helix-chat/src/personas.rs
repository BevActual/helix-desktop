//! Mobius agent personas: hub-side minds (prompt, memory scope, default route).
//! They never hold Agent Surface grants — see `inference-fabric.md`.

use serde::{Deserialize, Serialize};

use crate::display::sanitize_for_display;

pub const PERSONAS_FILE: &str = "personas.toml";
pub const MAX_PERSONAS: usize = 32;
pub const MAX_NAME_CHARS: usize = 40;
pub const MAX_PROMPT_CHARS: usize = 4_000;

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Persona {
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub system_prompt: String,
    /// Inference task profile: `chat`, `code`, `vision`, `summarize`, …
    #[serde(default = "default_task")]
    pub task: String,
    /// Optional override of routing backend id for this persona.
    #[serde(default)]
    pub backend: String,
    /// Optional model id override.
    #[serde(default)]
    pub model: String,
    /// A symbol from [`ICONS`] shown on the persona's tile; empty falls back to its initials.
    #[serde(default)]
    pub icon: String,
    /// One line saying what this persona is for.
    #[serde(default)]
    pub description: String,
    /// Example prompts shown on an empty chat (at most [`MAX_STARTERS`]).
    #[serde(default)]
    pub starters: Vec<String>,
}

pub const MAX_STARTERS: usize = 4;
pub const MAX_STARTER_CHARS: usize = 80;
pub const MAX_DESCRIPTION_CHARS: usize = 120;
/// The symbols a persona may pick from (all exist in the shared icon set).
pub const ICONS: [&str; 14] = [
    "sparkle",
    "music",
    "palette",
    "fork",
    "clipboard",
    "eye",
    "shield",
    "sun",
    "moon",
    "bolt",
    "pin",
    "image",
    "folder",
    "lock",
];

fn default_task() -> String {
    "chat".into()
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct PersonaStore {
    #[serde(default)]
    pub active: String,
    #[serde(default)]
    pub personas: Vec<Persona>,
}

impl Default for PersonaStore {
    fn default() -> Self {
        let id = "helix".to_string();
        Self {
            active: id.clone(),
            personas: vec![Persona {
                id,
                name: "Helix".into(),
                system_prompt: String::new(), // app fills default SYSTEM_PROMPT when empty
                task: "chat".into(),
                backend: String::new(),
                model: String::new(),
                icon: "sparkle".into(),
                description: "General help, on this computer.".into(),
                starters: Vec::new(),
            }],
        }
    }
}

impl PersonaStore {
    pub fn parse(text: &str) -> Result<Self, String> {
        toml::from_str(text).map_err(|e| e.to_string())
    }

    pub fn to_toml(&self) -> Result<String, String> {
        toml::to_string_pretty(self).map_err(|e| e.to_string())
    }

    pub fn active_persona(&self) -> &Persona {
        self.personas
            .iter()
            .find(|p| p.id == self.active)
            .or_else(|| self.personas.first())
            .expect("PersonaStore always has at least one persona")
    }

    pub fn set_active(&mut self, id: &str) -> bool {
        if self.personas.iter().any(|p| p.id == id) {
            self.active = id.to_string();
            true
        } else {
            false
        }
    }

    pub fn upsert(&mut self, mut persona: Persona) -> Result<(), String> {
        persona.name = sanitize_for_display(&persona.name, MAX_NAME_CHARS);
        if persona.name.is_empty() {
            return Err("name is required".into());
        }
        if persona.id.is_empty()
            || persona.id.len() > 32
            || !persona
                .id
                .bytes()
                .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
        {
            return Err("id must be lowercase letters, digits or hyphens".into());
        }
        // Identity text goes first in every prompt: invisible and direction-changing characters are dropped, and the length is bounded.
        persona.system_prompt = super::mobius_lab::strip_invisible(&persona.system_prompt);
        if persona.system_prompt.chars().count() > MAX_PROMPT_CHARS {
            return Err("system prompt is too long".into());
        }
        persona.description = sanitize_for_display(&persona.description, MAX_DESCRIPTION_CHARS);
        if !persona.icon.is_empty() && !ICONS.contains(&persona.icon.as_str()) {
            persona.icon.clear();
        }
        persona.starters = persona
            .starters
            .iter()
            .map(|line| sanitize_for_display(line, MAX_STARTER_CHARS))
            .filter(|line| !line.is_empty())
            .take(MAX_STARTERS)
            .collect();
        if let Some(existing) = self.personas.iter_mut().find(|p| p.id == persona.id) {
            *existing = persona;
        } else {
            if self.personas.len() >= MAX_PERSONAS {
                return Err("too many personas".into());
            }
            self.personas.push(persona);
        }
        Ok(())
    }
}

pub fn mobius_state_dir() -> std::path::PathBuf {
    super::inference_routing::state_dir().join("mobius")
}

pub fn personas_path() -> std::path::PathBuf {
    mobius_state_dir().join(PERSONAS_FILE)
}

pub fn load() -> PersonaStore {
    std::fs::read_to_string(personas_path())
        .ok()
        .and_then(|text| PersonaStore::parse(&text).ok())
        .unwrap_or_default()
}

pub fn save(store: &PersonaStore) -> Result<(), String> {
    let dir = mobius_state_dir();
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let path = personas_path();
    let temporary = path.with_extension("tmp");
    std::fs::write(&temporary, store.to_toml()?).map_err(|e| e.to_string())?;
    std::fs::rename(&temporary, &path).map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_has_helix() {
        let s = PersonaStore::default();
        assert_eq!(s.active_persona().name, "Helix");
    }

    #[test]
    fn upsert_rejects_bad_id() {
        let mut s = PersonaStore::default();
        assert!(s
            .upsert(Persona {
                id: "Bad_ID".into(),
                name: "X".into(),
                system_prompt: String::new(),
                task: "chat".into(),
                backend: String::new(),
                model: String::new(),
                icon: String::new(),
                description: String::new(),
                starters: Vec::new(),
            })
            .is_err());
    }

    #[test]
    fn upsert_cleans_the_new_fields() {
        let mut s = PersonaStore::default();
        s.upsert(Persona {
            id: "coder".into(),
            name: "Coder".into(),
            system_prompt: "You are\u{200b} a coder.".into(),
            task: "code".into(),
            backend: String::new(),
            model: String::new(),
            icon: "not-an-icon".into(),
            description: "Writes code".into(),
            starters: vec![
                "a".into(),
                "".into(),
                "b".into(),
                "c".into(),
                "d".into(),
                "e".into(),
            ],
        })
        .unwrap();
        let coder = s.personas.iter().find(|p| p.id == "coder").unwrap();
        assert_eq!(coder.system_prompt, "You are a coder.");
        assert_eq!(coder.icon, "");
        assert_eq!(coder.starters, vec!["a", "b", "c", "d"]);
    }
}
