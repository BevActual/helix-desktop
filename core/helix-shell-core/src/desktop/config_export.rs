//! Effective config for theme templates (`config.*`). Rust export is default ([`rust_export_toml`],
//! `helix-rust-config-export` CLI). Milestone C (`parseConfigTable` → serialize) is tracked in
//! `docs/design/config-export-rust.md`.

use std::path::PathBuf;
use std::process::Command;

use serde_json::Value;
use toml::Value as Toml;

use super::config_export_rust;
use super::config_export_serialize::serialize_default_config;
use super::settings::{toml_to_json_value, ConfigPaths, ConfigStore};

pub fn configure_export_env(command: &mut Command, paths: &ConfigPaths) {
    if let Some(parent) = paths.config_dir.parent() {
        command.env("XDG_CONFIG_HOME", parent);
        command.env("HELIX_CONFIG_HOME", parent);
    }
    if let Some(state_helix) = paths.settings_toml.parent() {
        if let Some(parent) = state_helix.parent() {
            command.env("XDG_STATE_HOME", parent);
            command.env("HELIX_STATE_HOME", parent);
        }
    }
}

/// Locate `helix-rust-config-export` (Cargo `target/*` or sibling of the current test binary).
pub fn discover_rust_config_export_bin() -> Option<PathBuf> {
    if let Ok(path) = std::env::var("HELIX_RUST_CONFIG_EXPORT_BIN") {
        if !path.is_empty() {
            let candidate = PathBuf::from(path);
            if candidate.is_file() {
                return Some(candidate);
            }
        }
    }
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    if let Some(root) = manifest.parent().and_then(|p| p.parent()) {
        for subdir in ["target/debug", "target/fast", "build-debug"] {
            let candidate = root.join(subdir).join("helix-rust-config-export");
            if candidate.is_file() {
                return Some(candidate);
            }
        }
    }
    if let Ok(exe) = std::env::current_exe() {
        for name in ["helix-rust-config-export", "helix-rust-config-export.exe"] {
            if let Some(parent) = exe.parent() {
                let sibling = parent.join(name);
                if sibling.is_file() {
                    return Some(sibling);
                }
            }
        }
    }
    None
}

/// `config_export::serialize(makeDefaultConfig())` (milestone B).
pub fn default_export_toml() -> Toml {
    serialize_default_config()
}

/// Effective export for merged user config (milestone C: patch overlay today; parse → writeTable later).
pub fn serialize_effective_export(store: &ConfigStore) -> Option<Toml> {
    rust_export_from_store(store)
}

/// In-process Rust exporter (parity target: `tests/golden/config_export_minimal.toml`).
pub fn rust_export_toml(paths: &ConfigPaths) -> Option<Toml> {
    config_export_rust::rust_export_toml(paths)
}

/// Like [`rust_export_toml`] but uses an already-loaded [`ConfigStore`].
pub fn rust_export_from_store(store: &ConfigStore) -> Option<Toml> {
    config_export_rust::rust_export_from_store(store)
}

/// Effective export TOML from the in-process Rust exporter.
pub fn export_effective_toml(store: &ConfigStore) -> Option<Toml> {
    rust_export_from_store(store)
}

/// `config_export::serialize` JSON when export succeeds; else merged TOML JSON.
pub fn template_config_for_apply(store: &ConfigStore) -> Value {
    if let Some(root) = export_effective_toml(store) {
        return toml_to_json_value(&root);
    }
    store.template_config_table()
}
