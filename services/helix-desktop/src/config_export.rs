//! C++ `config_export::serialize` for theme templates (`config.*`).

use std::process::Command;

use helix_shell_core::desktop::settings::toml_to_json_value;
use helix_shell_core::ConfigStore;
use serde_json::Value;
use toml::Value as Toml;

use crate::assets_path::helix_config_export_bin;

fn configure_export_env(command: &mut Command, store: &ConfigStore) {
    let paths = store.paths();
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

fn try_cpp_export(store: &ConfigStore) -> Option<Value> {
    let bin = helix_config_export_bin()?;
    let mut command = Command::new(&bin);
    configure_export_env(&mut command, store);
    let output = command.output().ok()?;
    if !output.status.success() {
        tracing::debug!(
            stderr = %String::from_utf8_lossy(&output.stderr).trim(),
            "helix-config-export failed; using merged TOML for config.*"
        );
        return None;
    }
    let text = String::from_utf8(output.stdout).ok()?;
    let root: Toml = text.parse().ok()?;
    Some(toml_to_json_value(&root))
}

/// `config_export::serialize` when `helix-config-export` is available; else merged TOML JSON.
pub fn template_config_json(store: &ConfigStore) -> Value {
    if let Some(json) = try_cpp_export(store) {
        tracing::debug!("template apply: config.* from helix-config-export");
        return json;
    }
    store.template_config_table()
}

#[cfg(test)]
mod tests {
    use super::*;
    use helix_shell_core::desktop::settings::ConfigPaths;
    use std::fs;
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_root() -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        std::env::temp_dir().join(format!("helix-config-export-{nanos}"))
    }

    fn export_bin() -> Option<PathBuf> {
        if let Ok(path) = std::env::var("HELIX_CONFIG_EXPORT_BIN") {
            let candidate = PathBuf::from(path);
            if candidate.is_file() {
                return Some(candidate);
            }
        }
        let candidate = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../build-debug/helix-config-export");
        candidate.is_file().then_some(candidate)
    }

    #[test]
    fn schema_export_adds_bar_root_when_helper_present() {
        let Some(bin) = export_bin() else {
            eprintln!("skip schema_export_adds_bar_root: helix-config-export not built");
            return;
        };
        let root = temp_root();
        let config_dir = root.join("cfg").join("helix");
        let state_dir = root.join("state").join("helix");
        fs::create_dir_all(&config_dir).unwrap();
        fs::create_dir_all(&state_dir).unwrap();
        fs::write(
            config_dir.join("config.toml"),
            "[theme]\nmode = \"dark\"\n",
        )
        .unwrap();
        fs::write(state_dir.join("settings.toml"), "").unwrap();

        std::env::set_var("HELIX_CONFIG_EXPORT_BIN", bin);
        std::env::set_var("XDG_CONFIG_HOME", root.join("cfg"));
        std::env::set_var("XDG_STATE_HOME", root.join("state"));

        let store = ConfigStore::load(ConfigPaths {
            config_dir,
            settings_toml: state_dir.join("settings.toml"),
        });
        let exported = template_config_json(&store);
        assert!(exported.get("theme").is_some());
        assert!(
            exported.get("bar").is_some(),
            "config_export::serialize always emits [bar]"
        );
        let merged = store.template_config_table();
        assert!(
            merged.get("bar").is_none(),
            "merged TOML should not invent [bar] without config"
        );
        let _ = fs::remove_dir_all(root);
    }
}
