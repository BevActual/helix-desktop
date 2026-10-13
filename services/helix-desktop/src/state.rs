use std::collections::HashMap;
use std::path::PathBuf;

use helix_shell_core::{ConfigStore, GrantStore, RiskPolicy, TaintTracker};
use serde_json::Value;

use crate::audio_backend::AudioBackend;
use crate::bluetooth_backend::BluetoothBackend;
use crate::network_backend::NetworkBackend;
use crate::palette_catalog::load_palette_previews;
use crate::runtime_state::RuntimeState;

#[derive(Clone, Debug)]
pub struct ClientSession {
    pub label: String,
    #[allow(dead_code)]
    pub version: String,
}

#[derive(Clone, Debug)]
pub struct PendingConfirm {
    pub client: String,
    pub kind: ConfirmKind,
    pub tool: String,
    pub args: Vec<String>,
    pub reason: String,
    pub tiers: Vec<String>,
    pub tainted: bool,
    pub label: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ConfirmKind {
    Action,
    Grant,
}

pub struct State {
    pub risk: RiskPolicy,
    pub grants: GrantStore,
    pub taint: TaintTracker,
    pub config: ConfigStore,
    pub audio: AudioBackend,
    pub network: NetworkBackend,
    pub bluetooth: BluetoothBackend,
    /// Last wallpaper path from Appearance1.SetWallpaper (scaffold; not applied to compositor).
    pub wallpaper: String,
    /// Cached Appearance1 `palette-previews` (builtins + community catalog + custom files).
    pub palette_previews: Vec<Value>,
    pub runtime: RuntimeState,
    /// Desktop-reported capabilities (`gamma-control`, …).
    pub desktop_capabilities: HashMap<String, bool>,
    pub clients: HashMap<String, ClientSession>,
    pub next_operation: u64,
    pub pending: HashMap<u64, PendingConfirm>,
    /// Confirmer bus unique name (must stay on one connection for signals).
    pub confirmer: Option<String>,
    /// Manager grant key (`exe:…`), not unique name — Settings may reconnect.
    pub manager: Option<String>,
    /// Desktop shell unique name.
    pub desktop: Option<String>,
    /// Agent Surface kill switch (`AgentSurfaceActivity1.SetEnabled`).
    pub agent_enabled: bool,
    pub audit: Vec<Value>,
}

impl State {
    pub fn new() -> Self {
        let config = ConfigStore::load(helix_shell_core::ConfigPaths::from_env());
        let initial_volume = config
            .get("audio.sound_volume")
            .as_f64()
            .unwrap_or(0.5);
        let state_dir = config
            .paths()
            .settings_toml
            .parent()
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("helix"));
        let grants_path = state_dir.join("agent/grants.json");
        let mut grants = GrantStore::new(grants_path);
        if let Err(error) = grants.load() {
            tracing::warn!(%error, "grants load failed; starting empty");
        }
        let runtime = RuntimeState::load(state_dir.join("state.toml"));
        let palette_previews =
            load_palette_previews(&config.paths().config_dir, &config.paths().settings_toml);
        Self {
            risk: RiskPolicy::embedded(),
            grants,
            taint: TaintTracker::default(),
            audio: AudioBackend::new(initial_volume),
            network: NetworkBackend::new(),
            bluetooth: BluetoothBackend::new(),
            wallpaper: String::new(),
            palette_previews,
            runtime,
            desktop_capabilities: HashMap::new(),
            config,
            clients: HashMap::new(),
            next_operation: 1,
            pending: HashMap::new(),
            confirmer: None,
            manager: None,
            desktop: None,
            agent_enabled: true,
            audit: Vec::new(),
        }
    }

    pub fn refresh_palette_previews(&mut self) {
        self.palette_previews = load_palette_previews(
            &self.config.paths().config_dir,
            &self.config.paths().settings_toml,
        );
    }

    pub fn alloc_operation(&mut self) -> u64 {
        let id = self.next_operation;
        self.next_operation = self.next_operation.saturating_add(1);
        id
    }
}
