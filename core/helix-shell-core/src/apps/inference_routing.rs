//! Inference fabric routing: task profile → backend (local / LAN / cloud).
//! Edited by Mobius; read by helix-inference (local slots) and Mobius (LAN talk).
//! Path: `~/.local/state/helix/inference-routing.toml` (see `inference-fabric.md`).

use serde::{Deserialize, Serialize};

pub const ROUTING_FILE: &str = "inference-routing.toml";

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum BackendKind {
    Local,
    /// OpenAI-compatible HTTP (Ollama, etc.). Never emits Agent Surface tools.
    Openai,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Backend {
    pub id: String,
    pub kind: BackendKind,
    /// Base URL for `openai` (e.g. `http://host:11434/v1`). Empty for local.
    #[serde(default)]
    pub base_url: String,
    #[serde(default)]
    pub label: String,
    /// Secret-service account id for a Bearer token. Empty = no Authorization header.
    /// Never store the key itself in routing TOML (see inference-fabric.md).
    #[serde(default)]
    pub api_key_ref: String,
}

/// OpenRouter as an OpenAI-compatible cloud preset (talk only; never Act tools).
pub fn openrouter_preset() -> Backend {
    Backend {
        id: "openrouter".into(),
        kind: BackendKind::Openai,
        base_url: "https://openrouter.ai/api/v1".into(),
        label: "OpenRouter".into(),
        api_key_ref: "openrouter".into(),
    }
}

/// Suggested remote model ids when the cloud catalog is unreachable.
pub fn openrouter_suggested_models() -> &'static [&'static str] {
    &[
        "openai/gpt-4o-mini",
        "openai/gpt-4o",
        "anthropic/claude-sonnet-4",
        "google/gemini-2.5-flash",
        "meta-llama/llama-3.3-70b-instruct",
        "deepseek/deepseek-chat",
        "mistralai/mistral-small-3.1-24b-instruct",
    ]
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ProfileRoute {
    /// Backend id from [`Routing::backends`], or `local` / `lan` shorthand.
    pub backend: String,
    /// Local GGUF filename or remote model id. Empty = service default.
    #[serde(default)]
    pub model: String,
}

fn default_temperature() -> f64 {
    0.6
}
fn default_max_tokens() -> u32 {
    1024
}
fn default_remote_timeout_secs() -> u32 {
    120
}

/// Generation defaults applied to Talk / Code / remote turns (Act may ignore).
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct TalkSettings {
    #[serde(default = "default_temperature")]
    pub temperature: f64,
    #[serde(default = "default_max_tokens")]
    pub max_tokens: u32,
    /// HTTP timeout for LAN/cloud completions.
    #[serde(default = "default_remote_timeout_secs")]
    pub remote_timeout_secs: u32,
}

impl Default for TalkSettings {
    fn default() -> Self {
        Self {
            temperature: default_temperature(),
            max_tokens: default_max_tokens(),
            remote_timeout_secs: default_remote_timeout_secs(),
        }
    }
}

impl TalkSettings {
    pub fn clamped(mut self) -> Self {
        if !self.temperature.is_finite() {
            self.temperature = default_temperature();
        }
        self.temperature = self.temperature.clamp(0.0, 2.0);
        self.max_tokens = self.max_tokens.clamp(64, 8192);
        self.remote_timeout_secs = self.remote_timeout_secs.clamp(15, 600);
        self
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Routing {
    #[serde(default)]
    pub backends: Vec<Backend>,
    #[serde(default)]
    pub profiles: std::collections::BTreeMap<String, ProfileRoute>,
    /// When power profile is saver, refuse non-local backends unless true.
    #[serde(default)]
    pub allow_remote_on_battery_saver: bool,
    /// Sampling / remote request defaults for talk profiles.
    #[serde(default)]
    pub talk: TalkSettings,
}

impl Default for Routing {
    fn default() -> Self {
        let mut profiles = std::collections::BTreeMap::new();
        profiles.insert(
            "act".into(),
            ProfileRoute {
                backend: "local".into(),
                model: String::new(),
            },
        );
        profiles.insert(
            "chat".into(),
            ProfileRoute {
                backend: "local".into(),
                model: String::new(),
            },
        );
        profiles.insert(
            "summarize".into(),
            ProfileRoute {
                backend: "local".into(),
                model: String::new(),
            },
        );
        profiles.insert(
            "vision".into(),
            ProfileRoute {
                backend: "local".into(),
                model: String::new(),
            },
        );
        profiles.insert(
            "code".into(),
            ProfileRoute {
                backend: "local".into(),
                model: String::new(),
            },
        );
        Self {
            backends: vec![Backend {
                id: "local".into(),
                kind: BackendKind::Local,
                base_url: String::new(),
                label: "On this computer".into(),
                api_key_ref: String::new(),
            }],
            profiles,
            allow_remote_on_battery_saver: false,
            talk: TalkSettings::default(),
        }
    }
}

impl Routing {
    pub fn parse(text: &str) -> Result<Self, String> {
        toml::from_str(text).map_err(|e| e.to_string())
    }

    pub fn to_toml(&self) -> Result<String, String> {
        toml::to_string_pretty(self).map_err(|e| e.to_string())
    }

    pub fn route(&self, task: &str) -> ProfileRoute {
        self.profiles
            .get(task)
            .cloned()
            .unwrap_or_else(|| ProfileRoute {
                backend: "local".into(),
                model: String::new(),
            })
    }

    pub fn backend(&self, id: &str) -> Option<&Backend> {
        if id == "local" || id == "lan" {
            return self
                .backends
                .iter()
                .find(|b| b.id == id)
                .or_else(|| match id {
                    "local" => self.backends.iter().find(|b| b.kind == BackendKind::Local),
                    // A route that says "lan" must never end up at a cloud service: only an OpenAI-style backend that really is on
                    // this computer or the local network qualifies. Without one the route has no backend and the turn is refused.
                    _ => self.backends.iter().find(|b| b.kind == BackendKind::Openai && is_local_network_url(&b.base_url)),
                });
        }
        self.backends.iter().find(|b| b.id == id)
    }

    pub fn is_local_route(&self, task: &str) -> bool {
        let route = self.route(task);
        if route.backend.is_empty() || route.backend == "local" {
            return true;
        }
        matches!(
            self.backend(&route.backend).map(|b| &b.kind),
            Some(BackendKind::Local)
        )
    }
}

/// Whether `url` points at this computer or the local network: loopback, RFC 1918, link-local, `.local` / `.localhost` names, or an IPv6
/// loopback / unique-local / link-local literal. A name that merely begins like an address (`10.0.0.5.evil.example`) is not one.
pub fn is_local_network_url(url: &str) -> bool {
    let url = url.trim();
    let Some(rest) = url.strip_prefix("http://").or_else(|| url.strip_prefix("https://")) else { return false };
    let authority = rest.split(['/', '?', '#']).next().unwrap_or("");
    if authority.contains('@') {
        return false;
    }
    let host = match authority.strip_prefix('[') {
        Some(v6) => v6.split(']').next().unwrap_or("").to_string(),
        None => authority
            .rsplit_once(':')
            .map_or(authority, |(h, port)| if port.chars().all(|c| c.is_ascii_digit()) { h } else { authority })
            .to_string(),
    };
    let host = host.to_ascii_lowercase();
    if host == "localhost" || host.ends_with(".localhost") || host.ends_with(".local") || host == "::1" {
        return true;
    }
    let octets: Vec<u8> = host.split('.').filter_map(|p| p.parse().ok()).collect();
    if octets.len() == 4 && host.split('.').count() == 4 {
        return matches!(octets[..], [127, ..] | [10, ..] | [192, 168, ..] | [169, 254, ..]) || (octets[0] == 172 && (16..=31).contains(&octets[1]));
    }
    (host.starts_with("fd") && host.contains(':')) || host.starts_with("fe80:")
}

/// Resolve path: `$HELIX_STATE_HOME/helix` or `$XDG_STATE_HOME/helix` or `~/.local/state/helix`.
pub fn state_dir() -> std::path::PathBuf {
    if let Some(base) = std::env::var_os("HELIX_STATE_HOME").filter(|v| !v.is_empty()) {
        return std::path::PathBuf::from(base).join("helix");
    }
    let base = std::env::var_os("XDG_STATE_HOME")
        .filter(|v| !v.is_empty())
        .map(std::path::PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME").map(|h| std::path::PathBuf::from(h).join(".local/state"))
        })
        .unwrap_or_else(|| std::path::PathBuf::from("."));
    base.join("helix")
}

pub fn routing_path() -> std::path::PathBuf {
    state_dir().join(ROUTING_FILE)
}

pub fn load() -> Routing {
    let path = routing_path();
    std::fs::read_to_string(&path)
        .ok()
        .and_then(|text| Routing::parse(&text).ok())
        .unwrap_or_default()
}

pub fn save(routing: &Routing) -> Result<(), String> {
    let dir = state_dir();
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let path = routing_path();
    let temporary = path.with_extension("tmp");
    let mut cleaned = routing.clone();
    cleaned.talk = cleaned.talk.clamped();
    let text = cleaned.to_toml()?;
    std::fs::write(&temporary, text).map_err(|e| e.to_string())?;
    std::fs::rename(&temporary, &path).map_err(|e| e.to_string())
}

/// Values mirrored into Settings search keys (`ai.*`); authoritative store is [`routing_path`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SettingsMirror {
    pub act_model: String,
    pub chat_model: String,
    pub lan_endpoint: String,
    pub allow_remote_on_battery_saver: bool,
}

impl SettingsMirror {
    pub fn from_routing(routing: &Routing) -> Self {
        let lan_endpoint = routing
            .backends
            .iter()
            .find(|b| b.kind == BackendKind::Openai && !b.base_url.is_empty())
            .map(|b| b.base_url.clone())
            .unwrap_or_default();
        Self {
            act_model: routing.route("act").model,
            chat_model: routing.route("chat").model,
            lan_endpoint,
            allow_remote_on_battery_saver: routing.allow_remote_on_battery_saver,
        }
    }
}

pub fn settings_mirror() -> SettingsMirror {
    SettingsMirror::from_routing(&load())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_lan_route_never_resolves_to_a_cloud_backend() {
        let mut r = Routing::default();
        r.backends.push(Backend {
            id: "openrouter".into(),
            kind: BackendKind::Openai,
            base_url: "https://openrouter.ai/api/v1".into(),
            label: "OpenRouter".into(),
            api_key_ref: "k".into(),
        });
        assert!(r.backend("lan").is_none(), "no LAN backend: the route has nothing to go to");
        r.backends.push(Backend {
            id: "studio".into(),
            kind: BackendKind::Openai,
            base_url: "http://studio.local:11434/v1".into(),
            label: "Studio".into(),
            api_key_ref: String::new(),
        });
        assert_eq!(r.backend("lan").map(|b| b.id.as_str()), Some("studio"));
    }

    #[test]
    fn local_network_addresses_are_told_apart() {
        for ok in ["http://localhost:1/v1", "http://127.0.0.1/v1", "http://192.168.0.2:80", "https://10.1.2.3/v1", "http://172.20.0.1", "http://[::1]:9", "http://a.local"] {
            assert!(is_local_network_url(ok), "{ok}");
        }
        for bad in ["https://api.openai.com/v1", "http://8.8.8.8", "http://172.32.0.1", "http://10.0.0.5.evil.example", "http://user@192.168.0.2", "ftp://192.168.0.2", "192.168.0.2", ""] {
            assert!(!is_local_network_url(bad), "{bad}");
        }
    }

    #[test]
    fn default_profiles_are_local() {
        let r = Routing::default();
        assert!(r.is_local_route("act"));
        assert!(r.is_local_route("chat"));
        assert_eq!(r.route("chat").backend, "local");
    }

    #[test]
    fn settings_mirror_reads_profiles_and_lan() {
        let mut r = Routing::default();
        r.profiles.get_mut("act").unwrap().model = "act.gguf".into();
        r.profiles.get_mut("chat").unwrap().model = "chat.gguf".into();
        r.backends.push(Backend {
            id: "lan".into(),
            kind: BackendKind::Openai,
            base_url: "http://127.0.0.1:11434/v1".into(),
            label: "LAN".into(),
            api_key_ref: String::new(),
        });
        r.allow_remote_on_battery_saver = true;
        let mirror = SettingsMirror::from_routing(&r);
        assert_eq!(mirror.act_model, "act.gguf");
        assert_eq!(mirror.chat_model, "chat.gguf");
        assert_eq!(mirror.lan_endpoint, "http://127.0.0.1:11434/v1");
        assert!(mirror.allow_remote_on_battery_saver);
    }

    #[test]
    fn openrouter_preset_is_openai_with_key_ref() {
        let b = openrouter_preset();
        assert_eq!(b.id, "openrouter");
        assert_eq!(b.kind, BackendKind::Openai);
        assert!(b.base_url.contains("openrouter.ai"));
        assert_eq!(b.api_key_ref, "openrouter");
    }

    #[test]
    fn round_trip_toml() {
        let mut r = Routing::default();
        r.backends.push(Backend {
            id: "ollama".into(),
            kind: BackendKind::Openai,
            base_url: "http://127.0.0.1:11434/v1".into(),
            label: "Home Ollama".into(),
            api_key_ref: String::new(),
        });
        r.backends.push(openrouter_preset());
        r.profiles.insert(
            "code".into(),
            ProfileRoute {
                backend: "ollama".into(),
                model: "codellama".into(),
            },
        );
        r.talk.temperature = 0.4;
        r.talk.max_tokens = 2048;
        let text = r.to_toml().unwrap();
        let back = Routing::parse(&text).unwrap();
        assert_eq!(back.route("code").model, "codellama");
        assert!(!back.is_local_route("code"));
        let cloud = back.backend("openrouter").unwrap();
        assert_eq!(cloud.api_key_ref, "openrouter");
        assert!((back.talk.temperature - 0.4).abs() < f64::EPSILON);
        assert_eq!(back.talk.max_tokens, 2048);
    }

    #[test]
    fn legacy_toml_without_talk_uses_defaults() {
        let text = r#"
[[backends]]
id = "local"
kind = "local"

[profiles.chat]
backend = "local"
model = ""
"#;
        let r = Routing::parse(text).unwrap();
        assert_eq!(r.talk.max_tokens, 1024);
        assert!((r.talk.temperature - 0.6).abs() < f64::EPSILON);
    }
}
