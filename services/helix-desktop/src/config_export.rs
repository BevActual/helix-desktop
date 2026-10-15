//! Theme `config.*` table for template apply (see `helix-shell-core::desktop::config_export`).

use helix_shell_core::desktop::config_export::template_config_for_apply;
use helix_shell_core::ConfigStore;
use serde_json::Value;

/// Effective config JSON for `ApplyInputs::config_table`.
pub fn template_config_json(store: &ConfigStore) -> Value {
    let json = template_config_for_apply(store, None);
    if json.get("bar").is_some() {
        tracing::debug!("template apply: config.* from schema export");
    }
    json
}
