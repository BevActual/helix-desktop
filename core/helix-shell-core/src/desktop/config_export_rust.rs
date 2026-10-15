//! In-process `config_export::serialize` parity (incremental port).
//!
//! Today: schema-default export (`tests/golden/config_export_minimal.toml`) rebuilt for
//! dynamic `bar` / `idle.behavior` shapes, then user `config.toml` / `settings.toml` leaves
//! patched when those paths exist in the export tree. Unknown merged keys fall back to
//! `helix-config-export` (see `docs/design/config-export-rust.md`).

use std::sync::OnceLock;

use toml::Value as Toml;

use super::settings::{ConfigPaths, ConfigStore};

const DEFAULT_EXPORT_TOML: &str =
    include_str!("../../tests/golden/config_export_minimal.toml");

fn default_export_root() -> &'static Toml {
    static ROOT: OnceLock<Toml> = OnceLock::new();
    ROOT.get_or_init(|| {
        DEFAULT_EXPORT_TOML
            .parse()
            .expect("config_export_minimal.toml must parse")
    })
}

fn dot_path_exists(root: &Toml, path: &str) -> bool {
    let mut node = root;
    for part in path.split('.') {
        let table = match node.as_table() {
            Some(table) => table,
            None => return false,
        };
        node = match table.get(part) {
            Some(child) => child,
            None => return false,
        };
    }
    true
}

fn dot_path_get(root: &Toml, path: &str) -> Option<Toml> {
    let mut node = root;
    for part in path.split('.') {
        node = node.as_table()?.get(part)?;
    }
    Some(node.clone())
}

fn collect_leaf_paths(value: &Toml, prefix: &str, out: &mut Vec<String>) {
    match value {
        Toml::Table(table) => {
            if table.is_empty() {
                if !prefix.is_empty() {
                    out.push(prefix.to_string());
                }
                return;
            }
            for (key, child) in table {
                let path = if prefix.is_empty() {
                    key.clone()
                } else {
                    format!("{prefix}.{key}")
                };
                collect_leaf_paths(child, &path, out);
            }
        }
        _ => {
            if !prefix.is_empty() {
                out.push(prefix.to_string());
            }
        }
    }
}

fn merged_bar_names(merged: &Toml) -> Vec<String> {
    merged
        .get("bar")
        .and_then(Toml::as_table)
        .map(|bar| {
            bar.keys()
                .filter(|key| *key != "order")
                .cloned()
                .collect::<Vec<_>>()
        })
        .unwrap_or_default()
}

/// Extra `[bar.<name>]` tables are allowed before `config_export::serialize` runs in C++.
fn is_allowed_extra_bar_path(path: &str, merged: &Toml) -> bool {
    let parts = path.split('.').collect::<Vec<_>>();
    if parts.first().map(|s| *s) != Some("bar") {
        return false;
    }
    if parts.len() == 2 && parts[1] == "order" {
        return true;
    }
    if parts.len() < 3 {
        return false;
    }
    merged_bar_names(merged).iter().any(|name| name == parts[1])
}

/// Merged config may only touch export paths, or extra `bar.<name>` subtrees from user config.
fn merged_export_is_patchable(store: &ConfigStore) -> bool {
    let merged = store.merged_user_toml();
    let default = default_export_root();
    let mut leaves = Vec::new();
    collect_leaf_paths(&merged, "", &mut leaves);
    leaves.iter().all(|path| {
        dot_path_exists(default, path) || is_allowed_extra_bar_path(path, &merged)
    })
}

/// When user config names specific idle behaviors, export only those (C++ `idleSchema` / `behavior_order`).
fn rebuild_idle_from_merged(root: &mut Toml, merged: &Toml) {
    let merged_behavior = merged
        .get("idle")
        .and_then(Toml::as_table)
        .and_then(|idle| idle.get("behavior"))
        .and_then(Toml::as_table);
    let merged_behavior = match merged_behavior {
        Some(table) => table,
        None => return,
    };
    if merged_behavior.is_empty() {
        return;
    }
    let names: Vec<String> = merged_behavior.keys().cloned().collect();

    let export_idle = match root.get_mut("idle").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    let export_behavior = match export_idle.get_mut("behavior").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    let templates: toml::map::Map<String, Toml> = export_behavior.clone();
    let fallback = templates
        .get("lock")
        .cloned()
        .unwrap_or(Toml::Table(Default::default()));

    let stale: Vec<String> = export_behavior
        .keys()
        .filter(|key| !names.contains(key))
        .cloned()
        .collect();
    for key in stale {
        export_behavior.remove(&key);
    }
    for name in &names {
        export_behavior
            .entry(name.clone())
            .or_insert_with(|| templates.get(name).cloned().unwrap_or_else(|| fallback.clone()));
    }
    export_idle.insert(
        "behavior_order".to_string(),
        Toml::Array(names.into_iter().map(Toml::String).collect()),
    );
}

/// Seed unnamed bars from `bar.default` export defaults (C++ `BarConfig` defaults per name).
fn rebuild_bar_from_merged(root: &mut Toml, merged: &Toml) {
    let merged_bar = match merged.get("bar").and_then(Toml::as_table) {
        Some(table) => table,
        None => return,
    };
    let mut names: Vec<String> = merged_bar
        .keys()
        .filter(|key| *key != "order")
        .cloned()
        .collect();
    if names.is_empty() {
        return;
    }

    let export_bar = match root.get_mut("bar").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    let template = export_bar
        .get("default")
        .cloned()
        .unwrap_or(Toml::Table(Default::default()));

    let stale: Vec<String> = export_bar
        .keys()
        .filter(|key| *key != "order" && !names.contains(key))
        .cloned()
        .collect();
    for key in stale {
        export_bar.remove(&key);
    }
    for name in &names {
        export_bar
            .entry(name.clone())
            .or_insert_with(|| template.clone());
    }
    let order = merged_bar
        .get("order")
        .and_then(Toml::as_array)
        .map(|items| {
            items
                .iter()
                .filter_map(|value| value.as_str().map(str::to_string))
                .filter(|name| names.contains(name))
                .collect::<Vec<_>>()
        })
        .filter(|order| !order.is_empty());
    let order = match order {
        Some(order) => order,
        None => {
            names.sort();
            names
        }
    };
    export_bar.insert(
        "order".to_string(),
        Toml::Array(order.into_iter().map(Toml::String).collect()),
    );
}

/// Match C++ `writeTable` scalar kinds using the default-export node at `path` as the template.
fn coerce_patch_value(template: &Toml, patch: &Toml) -> Toml {
    match (template, patch) {
        (Toml::Integer(_), Toml::Float(value)) if value.fract() == 0.0 => {
            Toml::Integer(*value as i64)
        }
        (Toml::Float(_), Toml::Integer(value)) => Toml::Float(*value as f64),
        (Toml::Array(template_items), Toml::Array(patch_items))
            if template_items.len() == patch_items.len() =>
        {
            Toml::Array(
                template_items
                    .iter()
                    .zip(patch_items.iter())
                    .map(|(template, patch)| coerce_patch_value(template, patch))
                    .collect(),
            )
        }
        (Toml::Table(template_table), Toml::Table(patch_table)) => {
            let mut table = toml::map::Map::new();
            for (key, patch_value) in patch_table {
                let coerced = match template_table.get(key) {
                    Some(template_value) => coerce_patch_value(template_value, patch_value),
                    None => patch_value.clone(),
                };
                table.insert(key.clone(), coerced);
            }
            Toml::Table(table)
        }
        (_, patch) => patch.clone(),
    }
}

fn set_dot_path(root: &mut Toml, path: &str, value: Toml) {
    let parts: Vec<&str> = path.split('.').collect();
    if parts.is_empty() {
        return;
    }
    if !matches!(root, Toml::Table(_)) {
        *root = Toml::Table(Default::default());
    }
    let mut table = root.as_table_mut().unwrap();
    for part in &parts[..parts.len() - 1] {
        table
            .entry((*part).to_string())
            .or_insert_with(|| Toml::Table(Default::default()));
        table = table
            .get_mut(*part)
            .and_then(Toml::as_table_mut)
            .expect("export patch path");
    }
    table.insert(parts[parts.len() - 1].to_string(), value);
}

fn patch_default_export(store: &ConfigStore) -> Toml {
    let mut root = default_export_root().clone();
    let merged = store.merged_user_toml();
    rebuild_bar_from_merged(&mut root, &merged);
    rebuild_idle_from_merged(&mut root, &merged);
    let mut leaves = Vec::new();
    collect_leaf_paths(&merged, "", &mut leaves);
    for path in leaves {
        if !dot_path_exists(&root, &path) {
            continue;
        }
        if let (Some(template), Some(value)) =
            (dot_path_get(&root, &path), dot_path_get(&merged, &path))
        {
            set_dot_path(&mut root, &path, coerce_patch_value(&template, &value));
        }
    }
    root
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn coerce_matches_export_scalar_kinds() {
        assert_eq!(
            coerce_patch_value(&Toml::Integer(34), &Toml::Float(40.0)),
            Toml::Integer(40)
        );
        assert_eq!(
            coerce_patch_value(&Toml::Float(1.0), &Toml::Integer(2)),
            Toml::Float(2.0)
        );
    }

    #[test]
    fn declines_unknown_merged_paths() {
        let root = std::env::temp_dir().join(format!(
            "helix-export-decline-{}",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let _ = std::fs::remove_dir_all(&root);
        let config_dir = root.join("cfg").join("helix");
        let state_dir = root.join("state").join("helix");
        std::fs::create_dir_all(&config_dir).unwrap();
        std::fs::create_dir_all(&state_dir).unwrap();
        std::fs::write(
            config_dir.join("config.toml"),
            "[shell]\nlist = [\"a\"]\n",
        )
        .unwrap();
        std::fs::write(state_dir.join("settings.toml"), "\n").unwrap();
        let paths = ConfigPaths {
            config_dir,
            settings_toml: state_dir.join("settings.toml"),
        };
        assert!(rust_export_toml(&paths).is_none());
        let _ = std::fs::remove_dir_all(root);
    }
}

/// Effective export when the merged user config is bootstrap-patchable; else `None`.
pub fn rust_export_toml(paths: &ConfigPaths) -> Option<Toml> {
    let store = ConfigStore::load(paths.clone());
    if !merged_export_is_patchable(&store) {
        return None;
    }
    Some(patch_default_export(&store))
}
