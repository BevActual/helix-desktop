//! Desktop runtime state (`Configuration1.GetState` / `SetState` / `ClearState`).
//! Persists under `$XDG_STATE_HOME/helix/state.toml`, matching C++ `ConfigService` state store.

use std::collections::HashMap;
use std::fs;
use std::path::PathBuf;

use serde_json::{json, Value};
use toml::Value as Toml;

#[derive(Clone, Debug)]
enum Entry {
    Bool(bool),
    String(String),
}

#[derive(Clone, Debug)]
pub struct RuntimeState {
    path: PathBuf,
    /// owner → key → value
    data: HashMap<String, HashMap<String, Entry>>,
}

impl RuntimeState {
    pub fn load(path: PathBuf) -> Self {
        let data = read_file(&path).unwrap_or_default();
        Self { path, data }
    }

    pub fn get(&self, owner: &str, key: &str, kind: &str) -> Result<Value, String> {
        let entry = self
            .data
            .get(owner)
            .and_then(|keys| keys.get(key));
        match kind {
            "boolean" => {
                let present = matches!(entry, Some(Entry::Bool(_)));
                let value = match entry {
                    Some(Entry::Bool(flag)) => *flag,
                    _ => false,
                };
                Ok(json!({"present": present, "value": value}))
            }
            "string" => {
                let present = matches!(entry, Some(Entry::String(_)));
                let value = match entry {
                    Some(Entry::String(text)) => text.clone(),
                    _ => String::new(),
                };
                Ok(json!({"present": present, "value": value}))
            }
            other => Err(format!("Unknown runtime state type {other}")),
        }
    }

    pub fn set_bool(&mut self, owner: &str, key: &str, value: bool) -> Result<(), String> {
        self.data
            .entry(owner.to_string())
            .or_default()
            .insert(key.to_string(), Entry::Bool(value));
        self.save()
    }

    pub fn set_string(&mut self, owner: &str, key: &str, value: &str) -> Result<(), String> {
        self.data
            .entry(owner.to_string())
            .or_default()
            .insert(key.to_string(), Entry::String(value.to_string()));
        self.save()
    }

    pub fn clear_owner(&mut self, owner: &str) -> Result<(), String> {
        self.data.remove(owner);
        self.save()
    }

    fn save(&self) -> Result<(), String> {
        if let Some(parent) = self.path.parent() {
            fs::create_dir_all(parent).map_err(|error| error.to_string())?;
        }
        let mut root = toml::map::Map::new();
        for (owner, keys) in &self.data {
            let mut table = toml::map::Map::new();
            for (key, entry) in keys {
                let value = match entry {
                    Entry::Bool(flag) => Toml::Boolean(*flag),
                    Entry::String(text) => Toml::String(text.clone()),
                };
                table.insert(key.clone(), value);
            }
            root.insert(owner.clone(), Toml::Table(table));
        }
        let text = toml::to_string_pretty(&Toml::Table(root)).map_err(|error| error.to_string())?;
        fs::write(&self.path, text).map_err(|error| error.to_string())
    }
}

fn read_file(path: &PathBuf) -> Option<HashMap<String, HashMap<String, Entry>>> {
    let text = fs::read_to_string(path).ok()?;
    let root: Toml = text.parse().ok()?;
    let table = root.as_table()?;
    let mut data = HashMap::new();
    for (owner, node) in table {
        let Some(keys) = node.as_table() else {
            continue;
        };
        let mut map = HashMap::new();
        for (key, value) in keys {
            let entry = match value {
                Toml::Boolean(flag) => Entry::Bool(*flag),
                Toml::String(text) => Entry::String(text.clone()),
                _ => continue,
            };
            map.insert(key.clone(), entry);
        }
        data.insert(owner.clone(), map);
    }
    Some(data)
}
