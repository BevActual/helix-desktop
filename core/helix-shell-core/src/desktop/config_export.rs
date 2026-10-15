//! Effective config for theme templates (`config.*`). Uses `helix-config-export` when available.
//! Pure-Rust `config_export::serialize` is tracked in `docs/design/config-export-rust.md`.

use std::path::{Path, PathBuf};
use std::process::Command;

use serde_json::Value;
use toml::Value as Toml;

use super::config_export_rust;
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

/// Locate `helix-config-export` (`HELIX_CONFIG_EXPORT_BIN`, sibling of current exe, or Meson `build-*` dirs).
pub fn discover_config_export_bin() -> Option<PathBuf> {
    if let Ok(path) = std::env::var("HELIX_CONFIG_EXPORT_BIN") {
        if !path.is_empty() {
            let candidate = PathBuf::from(path);
            if candidate.is_file() {
                return Some(candidate);
            }
        }
    }
    if let Ok(exe) = std::env::current_exe() {
        for name in ["helix-config-export", "helix-config-export.exe"] {
            if let Some(parent) = exe.parent() {
                let sibling = parent.join(name);
                if sibling.is_file() {
                    return Some(sibling);
                }
            }
        }
        for ancestor in exe.ancestors() {
            for subdir in ["", "build-debug", "build-fast", "_build"] {
                let candidate = if subdir.is_empty() {
                    ancestor.join("helix-config-export")
                } else {
                    ancestor.join(subdir).join("helix-config-export")
                };
                if candidate.is_file() {
                    return Some(candidate);
                }
            }
        }
    }
    None
}

/// In-process Rust exporter (parity target: `tests/golden/config_export_minimal.toml`).
pub fn rust_export_toml(paths: &ConfigPaths) -> Option<Toml> {
    config_export_rust::rust_export_toml(paths)
}

/// Like [`rust_export_toml`] but uses an already-loaded [`ConfigStore`].
pub fn rust_export_from_store(store: &ConfigStore) -> Option<Toml> {
    config_export_rust::rust_export_from_store(store)
}

/// Run `helix-config-export` and parse stdout as TOML.
pub fn cpp_export_toml(paths: &ConfigPaths, bin: &Path) -> Option<Toml> {
    let mut command = Command::new(bin);
    configure_export_env(&mut command, paths);
    let output = command.output().ok()?;
    if !output.status.success() {
        return None;
    }
    let text = String::from_utf8(output.stdout).ok()?;
    text.parse().ok()
}

/// Effective export TOML: Rust (when implemented), then C++ helper, else `None`.
pub fn export_effective_toml(store: &ConfigStore, export_bin: Option<&Path>) -> Option<Toml> {
    if let Some(root) = rust_export_from_store(store) {
        return Some(root);
    }
    if let Some(bin) = export_bin {
        return cpp_export_toml(store.paths(), bin);
    }
    let discovered = discover_config_export_bin()?;
    cpp_export_toml(store.paths(), &discovered)
}

/// `config_export::serialize` JSON when export succeeds; else merged TOML JSON.
pub fn template_config_for_apply(store: &ConfigStore, export_bin: Option<&Path>) -> Value {
    if let Some(root) = export_effective_toml(store, export_bin) {
        return toml_to_json_value(&root);
    }
    store.template_config_table()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_root() -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        std::env::temp_dir().join(format!("helix-core-export-{nanos}"))
    }

    #[test]
    fn cpp_export_matches_schema_bar_root() {
        let Some(bin) = discover_config_export_bin() else {
            eprintln!("skip cpp_export_matches_schema_bar_root: helix-config-export not built");
            return;
        };
        let root = temp_root();
        let config_dir = root.join("cfg").join("helix");
        let state_dir = root.join("state").join("helix");
        fs::create_dir_all(&config_dir).unwrap();
        fs::create_dir_all(&state_dir).unwrap();
        fs::write(config_dir.join("config.toml"), "[theme]\nmode = \"dark\"\n").unwrap();
        fs::write(state_dir.join("settings.toml"), "").unwrap();

        let paths = ConfigPaths {
            config_dir,
            settings_toml: state_dir.join("settings.toml"),
        };
        let exported = cpp_export_toml(&paths, &bin).expect("export");
        assert!(exported.get("bar").is_some());
        let store = ConfigStore::load(paths);
        assert!(store.template_config_table().get("bar").is_none());
        let _ = fs::remove_dir_all(root);
    }
}
