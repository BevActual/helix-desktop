//! C++ `config_export::serialize` for theme templates (`config.*`).

use std::process::Command;

use helix_shell_core::ConfigStore;
use serde_json::Value;
use toml::Value as Toml;

use crate::assets_path::helix_config_export_bin;

fn toml_to_json(value: &Toml) -> Value {
    match value {
        Toml::Boolean(flag) => Value::Bool(*flag),
        Toml::Integer(number) => Value::from(*number),
        Toml::Float(number) => Value::from(*number),
        Toml::String(text) => Value::String(text.clone()),
        Toml::Array(items) => Value::Array(items.iter().map(toml_to_json).collect()),
        Toml::Table(table) => {
            let mut object = serde_json::Map::new();
            for (key, item) in table {
                object.insert(key.clone(), toml_to_json(item));
            }
            Value::Object(object)
        }
        Toml::Datetime(datetime) => Value::String(datetime.to_string()),
    }
}

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
    Some(toml_to_json(&root))
}

/// `config_export::serialize` when `helix-config-export` is available; else merged TOML JSON.
pub fn template_config_json(store: &ConfigStore) -> Value {
    if let Some(json) = try_cpp_export(store) {
        tracing::debug!("template apply: config.* from helix-config-export");
        return json;
    }
    store.template_config_table()
}
