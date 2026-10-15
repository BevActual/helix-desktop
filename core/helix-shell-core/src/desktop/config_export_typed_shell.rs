//! `shell` section: composed typed subtrees + `merge_export_subtree` for large tables.

use toml::Value as Toml;

use super::config_export_defaults::write_shell_defaults;
use super::config_export_rust::{coerce_patch_value, merge_export_subtree, merged_overlay_for_parsed_section};
use super::config_export_serialize::f32_bits;
use super::config_export_typed_scalar::overlay_known_leaves;

const MERGE_SUBTREE_KEYS: &[&str] = &[
    "keyboard_layout",
    "launcher",
    "screen_record",
    "screenshot",
    "session",
];

const OVERLAY_SUBTREE_KEYS: &[&str] = &[
    "animation",
    "greeter_sync",
    "mpris",
    "privacy",
    "screen_corners",
    "shadow",
    "window_switcher",
];

fn shell_panel_export(merged_shell: Option<&Toml>) -> Toml {
    let defaults = write_shell_defaults();
    let panel_defaults = defaults.get("panel").expect("shell.panel defaults");
    let merged_panel = merged_shell.and_then(|shell| shell.get("panel"));
    let mut out = match merged_panel {
        Some(panel) => overlay_known_leaves(panel_defaults, panel),
        None => panel_defaults.clone(),
    };
    if let Some(panel) = merged_panel.and_then(Toml::as_table) {
        if let Some(glass) = panel.get("glass") {
            let coerced = coerce_patch_value(
                &Toml::Float(0.0),
                glass,
            );
            out.as_table_mut()
                .expect("panel export table")
                .insert("glass".to_string(), coerced);
        }
    }
    out
}

fn overlay_subtree(defaults: &Toml, merged_shell: Option<&Toml>, key: &str) -> Toml {
    let default_sub = defaults.get(key).expect("shell subtree default");
    let merged_sub = merged_shell.and_then(|shell| shell.get(key));
    match merged_sub {
        Some(merged) => overlay_known_leaves(default_sub, merged),
        None => default_sub.clone(),
    }
}

fn overlay_shell_root_scalars(
    out: &mut toml::map::Map<String, Toml>,
    merged_shell: Option<&Toml>,
    defaults: &toml::map::Map<String, Toml>,
) {
    let merged_table = merged_shell.and_then(Toml::as_table);
    for (key, default_value) in defaults {
        if default_value.is_table() {
            continue;
        }
        let merged_value = match merged_table.and_then(|table| table.get(key)) {
            Some(value) => value,
            None => continue,
        };
        out.insert(key.clone(), coerce_patch_value(default_value, merged_value));
    }
}

pub(crate) fn export_shell_section_from_merged(merged: &Toml, default: &Toml) -> Toml {
    let defaults = write_shell_defaults();
    let default_table = defaults.as_table().expect("shell defaults table");
    let merged_shell = merged.get("shell");
    let overlay = merged_shell
        .map(|section| merged_overlay_for_parsed_section("shell", section))
        .unwrap_or_else(|| Toml::Table(toml::map::Map::new()));

    let mut out = default_table.clone();
    overlay_shell_root_scalars(&mut out, merged_shell, default_table);

    out.insert("panel".to_string(), shell_panel_export(merged_shell));

    for key in OVERLAY_SUBTREE_KEYS {
        out.insert(
            (*key).to_string(),
            overlay_subtree(&defaults, merged_shell, key),
        );
    }

    for key in MERGE_SUBTREE_KEYS {
        let default_sub = default_table.get(*key).expect("shell subtree default");
        let patch = overlay
            .get(*key)
            .cloned()
            .unwrap_or_else(|| Toml::Table(toml::map::Map::new()));
        out.insert(
            (*key).to_string(),
            merge_export_subtree(default_sub, &patch, merged, default, &format!("shell.{key}")),
        );
    }

    Toml::Table(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use super::super::config_export_rust::export_filtered_schema_section;
    use super::super::config_export_serialize::serialize_default_config;

    #[test]
    fn composed_shell_matches_filtered_merge() {
        let default = serialize_default_config();
        let merged: Toml = toml::from_str(
            r#"
[shell]
polkit_agent = true
corner_radius_scale = 1.25

[shell.panel]
glass = 0.25
transparency_mode = "solid"

[shell.animation]
speed = 0.5

[shell.launcher]
compact = true

[shell.launcher.providers.calculator]
prefix = "calc"
"#,
        )
        .unwrap();
        let filtered =
            export_filtered_schema_section("shell", &merged, &default, write_shell_defaults);
        let composed = export_shell_section_from_merged(&merged, &default);
        assert_eq!(composed, filtered);
    }

    #[test]
    fn panel_glass_uses_f32_bits() {
        let merged: Toml = toml::from_str(
            r#"
[shell.panel]
glass = 0.25000000000000006
"#,
        )
        .unwrap();
        let panel = shell_panel_export(merged.get("shell"));
        assert_eq!(
            panel.get("glass").and_then(|v| v.as_float()).map(f32_bits),
            Some(0.25)
        );
    }
}
