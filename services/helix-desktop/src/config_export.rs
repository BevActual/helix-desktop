//! Theme `config.*` table for template apply (see `helix-shell-core::desktop::config_export`).

use helix_shell_core::desktop::config_export::template_config_for_apply;
use helix_shell_core::ConfigStore;
use serde_json::Value;

use crate::assets_path::helix_config_export_bin;

/// Effective config JSON for `ApplyInputs::config_table`.
pub fn template_config_json(store: &ConfigStore) -> Value {
    let bin = helix_config_export_bin();
    let json = template_config_for_apply(store, bin.as_deref());
    if bin.is_some() && json.get("bar").is_some() {
        tracing::debug!("template apply: config.* from helix-config-export");
    }
    json
}
