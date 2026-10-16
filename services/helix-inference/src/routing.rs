//! `inference-routing.toml` (port of C++ `routing.cpp`): which backend and model each task uses. A deliberately small TOML
//! subset (tables and string/bool assignments); a missing or odd file means defaults, never an error.

use std::collections::BTreeMap;
use std::path::PathBuf;

#[derive(Clone, Debug, Default)]
pub struct BackendInfo {
    pub id: String,
    pub kind: String, // local | openai
    pub base_url: String,
    pub label: String,
}

#[derive(Clone, Debug)]
pub struct ProfileRoute {
    pub backend: String,
    pub model: String,
}

impl Default for ProfileRoute {
    fn default() -> Self {
        Self {
            backend: "local".into(),
            model: String::new(),
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct RoutingTable {
    pub backends: Vec<BackendInfo>,
    pub profiles: BTreeMap<String, ProfileRoute>,
    pub allow_remote_on_battery_saver: bool,
}

fn state_dir() -> PathBuf {
    let var = |name: &str| std::env::var(name).ok().filter(|v| !v.is_empty());
    if let Some(helix) = var("HELIX_STATE_HOME") {
        return PathBuf::from(helix).join("helix");
    }
    if let Some(xdg) = var("XDG_STATE_HOME") {
        return PathBuf::from(xdg).join("helix");
    }
    if let Some(home) = var("HOME") {
        return PathBuf::from(home).join(".local/state/helix");
    }
    PathBuf::from("helix")
}

impl RoutingTable {
    fn ensure_defaults(&mut self) {
        if self.backends.is_empty() {
            self.backends.push(BackendInfo {
                id: "local".into(),
                kind: "local".into(),
                base_url: String::new(),
                label: "On this computer".into(),
            });
        }
        for task in ["act", "chat", "summarize", "vision", "code"] {
            self.profiles.entry(task.to_string()).or_default();
        }
    }

    fn upsert_backend(&mut self, backend: BackendInfo) {
        if backend.id.is_empty() {
            return;
        }
        match self.backends.iter_mut().find(|b| b.id == backend.id) {
            Some(existing) => *existing = backend,
            None => self.backends.push(backend),
        }
    }

    pub fn parse(text: &str) -> Self {
        let mut out = RoutingTable::default();
        out.ensure_defaults();
        let mut profile_name = String::new();
        let mut current = BackendInfo::default();
        let mut backend_open = false;

        for raw in text.lines() {
            let line = raw
                .trim_end_matches(['\r', ' ', '\t'])
                .trim_start_matches([' ', '\t']);
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            if line.starts_with("[[") && line.len() >= 4 {
                let Some(end) = line.rfind("]]") else {
                    continue;
                };
                if backend_open {
                    out.upsert_backend(std::mem::take(&mut current));
                }
                backend_open = false;
                profile_name.clear();
                if line.get(2..end) == Some("backends") {
                    current = BackendInfo::default();
                    backend_open = true;
                }
                continue;
            }
            if line.starts_with('[') {
                let Some(end) = line.find(']') else { continue };
                if backend_open {
                    out.upsert_backend(std::mem::take(&mut current));
                }
                backend_open = false;
                profile_name.clear();
                let section = &line[1..end];
                if let Some(name) = section.strip_prefix("profiles.") {
                    profile_name = name.to_string();
                    out.profiles.entry(profile_name.clone()).or_default();
                }
                continue;
            }
            let Some(eq) = line.find('=') else { continue };
            let key = line[..eq].trim_end_matches([' ', '\t']);
            let mut value = line[eq + 1..].trim_start_matches([' ', '\t']).to_string();
            if value.len() >= 2 && value.starts_with('"') && value.ends_with('"') {
                value = value[1..value.len() - 1].to_string();
            }
            if !profile_name.is_empty() {
                let route = out.profiles.entry(profile_name.clone()).or_default();
                match key {
                    "backend" => route.backend = value,
                    "model" => route.model = value,
                    _ => {}
                }
            } else if backend_open {
                match key {
                    "id" => current.id = value,
                    "kind" => current.kind = value,
                    "base_url" => current.base_url = value,
                    "label" => current.label = value,
                    _ => {}
                }
            } else if key == "allow_remote_on_battery_saver" {
                out.allow_remote_on_battery_saver = value == "true";
            }
        }
        if backend_open {
            out.upsert_backend(current);
        }
        out.ensure_defaults();
        out
    }

    /// Loads `<state>/helix/inference-routing.toml`; missing or unreadable means defaults.
    pub fn load() -> Self {
        match std::fs::read_to_string(state_dir().join("inference-routing.toml")) {
            Ok(text) => Self::parse(&text),
            Err(_) => {
                let mut table = Self::default();
                table.ensure_defaults();
                table
            }
        }
    }

    pub fn route(&self, task: &str) -> ProfileRoute {
        self.profiles.get(task).cloned().unwrap_or_default()
    }

    pub fn is_local(&self, task: &str) -> bool {
        let route = self.route(task);
        if route.backend.is_empty() || route.backend == "local" {
            return true;
        }
        self.backends
            .iter()
            .find(|b| b.id == route.backend)
            .is_some_and(|b| b.kind == "local")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_when_empty() {
        let table = RoutingTable::parse("");
        assert!(table.is_local("act"));
        assert_eq!(table.backends.len(), 1);
        assert_eq!(table.route("chat").model, "");
    }

    #[test]
    fn reads_profiles_and_backends() {
        let table = RoutingTable::parse(
            "allow_remote_on_battery_saver = true\n[[backends]]\nid = \"lan\"\nkind = \"openai\"\nbase_url = \"http://x\"\n\
             [profiles.act]\nbackend = \"local\"\nmodel = \"small.gguf\"\n[profiles.chat]\nbackend = \"lan\"\n",
        );
        assert!(table.allow_remote_on_battery_saver);
        assert_eq!(table.route("act").model, "small.gguf");
        assert!(table.is_local("act"));
        assert!(!table.is_local("chat"));
        assert!(table
            .backends
            .iter()
            .any(|b| b.id == "lan" && b.kind == "openai"));
    }
}
