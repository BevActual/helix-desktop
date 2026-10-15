//! In-process `config_export::serialize` parity (incremental port).
//!
//! Milestone B default tree: [`config_export_serialize::serialize_default_config`].
//! Milestone C effective export: rebuild dynamic `bar` / `idle` / widgets, overlay merged leaves,
//! then [`config_export_serialize::finalize_serialized_export`]. Future: parse merged TOML into
//! `Config` and `writeTable` per section instead of leaf patch (see `docs/design/config-export-rust.md`).

use std::sync::OnceLock;

use toml::Value as Toml;

use super::config_export_serialize::{finalize_serialized_export, f32_bits, serialize_default_config};
use super::settings::{ConfigPaths, ConfigStore};

fn default_export_root() -> &'static Toml {
    static ROOT: OnceLock<Toml> = OnceLock::new();
    ROOT.get_or_init(serialize_default_config)
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

fn merged_widget_names(merged: &Toml) -> Vec<String> {
    merged
        .get("widget")
        .and_then(Toml::as_table)
        .map(|widgets| widgets.keys().cloned().collect())
        .unwrap_or_default()
}

fn widget_type_name(widget: &Toml, name: &str) -> String {
    widget
        .get("type")
        .and_then(Toml::as_str)
        .map(str::to_string)
        .unwrap_or_else(|| name.to_string())
}

fn widget_template_for_type(
    export_widgets: &toml::map::Map<String, Toml>,
    type_name: &str,
) -> Toml {
    for widget in export_widgets.values() {
        if widget.get("type").and_then(Toml::as_str) == Some(type_name) {
            return widget.clone();
        }
    }
    let mut table = toml::map::Map::new();
    table.insert("type".to_string(), Toml::String(type_name.to_string()));
    Toml::Table(table)
}

/// User `[widget.<name>]` tables may be absent from the schema-default export (C++ `seedBuiltinWidgets` + parse).
fn rebuild_widget_from_merged(root: &mut Toml, merged: &Toml) {
    let merged_widgets = match merged.get("widget").and_then(Toml::as_table) {
        Some(table) => table,
        None => return,
    };
    if merged_widgets.is_empty() {
        return;
    }
    let export_widgets = match root.get_mut("widget").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    let snapshot = export_widgets.clone();
    for (name, spec) in merged_widgets {
        let type_name = widget_type_name(spec, name);
        export_widgets
            .entry(name.clone())
            .or_insert_with(|| widget_template_for_type(&snapshot, &type_name));
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

/// Top-level tables that `config_export::serialize` never emits (C++ drops them at parse).
fn export_includes_section(default: &Toml, section: &str) -> bool {
    default
        .as_table()
        .is_some_and(|root| root.contains_key(section))
}

fn is_merged_leaf_exported(path: &str, default: &Toml) -> bool {
    let section = path.split('.').next().unwrap_or("");
    export_includes_section(default, section)
}

/// App-managed wallpaper keys kept in `config.toml` but omitted from `config_export::serialize`.
fn is_wallpaper_runtime_merged_path(path: &str) -> bool {
    matches!(
        path,
        "wallpaper.default"
            | "wallpaper.last"
            | "wallpaper.monitors"
            | "wallpaper.favorite"
    )
}

fn is_allowed_calendar_account_path(path: &str, merged: &Toml) -> bool {
    let parts = path.split('.').collect::<Vec<_>>();
    if parts.len() < 3 || parts[0] != "calendar" || parts[1] != "account" {
        return false;
    }
    merged
        .get("calendar")
        .and_then(|calendar| calendar.get("account"))
        .and_then(Toml::as_table)
        .is_some_and(|accounts| accounts.contains_key(parts[2]))
}

fn default_calendar_account_export_table() -> toml::map::Map<String, Toml> {
    let mut table = toml::map::Map::new();
    table.insert("calendars".to_string(), Toml::Array(vec![]));
    table.insert("color".to_string(), Toml::String(String::new()));
    table.insert(
        "credential_source".to_string(),
        Toml::String("secret-service".to_string()),
    );
    table.insert("name".to_string(), Toml::String(String::new()));
    table.insert("password_file".to_string(), Toml::String(String::new()));
    table.insert("path".to_string(), Toml::String(String::new()));
    table.insert("provider".to_string(), Toml::String(String::new()));
    table.insert("server_url".to_string(), Toml::String(String::new()));
    table.insert("type".to_string(), Toml::String(String::new()));
    table.insert("username".to_string(), Toml::String(String::new()));
    table
}

/// Legacy `url` keys in `[calendar.account.*]` are rejected/ignored at parse (use `server_url`).
fn is_calendar_account_parse_only_path(path: &str) -> bool {
    path.starts_with("calendar.account.") && path.ends_with(".url")
}

fn is_allowed_placement_widgets_path(section: &str, path: &str, merged: &Toml) -> bool {
    let order_path = format!("{section}.widget_order");
    if path == order_path {
        return merged
            .get(section)
            .and_then(|dw| dw.get("widget"))
            .and_then(Toml::as_table)
            .is_some_and(|widgets| !widgets.is_empty());
    }
    let parts = path.split('.').collect::<Vec<_>>();
    if parts.len() < 3 || parts[0] != section || parts[1] != "widget" {
        return false;
    }
    merged
        .get(section)
        .and_then(|dw| dw.get("widget"))
        .and_then(Toml::as_table)
        .is_some_and(|widgets| widgets.contains_key(parts[2]))
}

fn is_allowed_desktop_widgets_path(path: &str, merged: &Toml) -> bool {
    is_allowed_placement_widgets_path("desktop_widgets", path, merged)
}

fn is_allowed_lockscreen_widgets_path(path: &str, merged: &Toml) -> bool {
    is_allowed_placement_widgets_path("lockscreen_widgets", path, merged)
}

fn default_placement_widget_table(type_name: &str) -> toml::map::Map<String, Toml> {
    let mut table = toml::map::Map::new();
    table.insert("box_height".to_string(), Toml::Float(0.0));
    table.insert("box_width".to_string(), Toml::Float(0.0));
    table.insert("cx".to_string(), Toml::Float(0.0));
    table.insert("cy".to_string(), Toml::Float(0.0));
    table.insert("enabled".to_string(), Toml::Boolean(true));
    table.insert("output".to_string(), Toml::String(String::new()));
    table.insert("placement_height".to_string(), Toml::Float(0.0));
    table.insert("placement_width".to_string(), Toml::Float(0.0));
    table.insert("rotation".to_string(), Toml::Float(0.0));
    table.insert("type".to_string(), Toml::String(type_name.to_string()));
    // C++ `widgetsPlacementTable` always emits a `settings` subtable (even when empty).
    table.insert("settings".to_string(), Toml::Table(toml::map::Map::new()));
    table
}

fn rebuild_placement_widgets_from_merged(section: &str, root: &mut Toml, merged: &Toml) {
    let merged_section = merged.get(section).and_then(Toml::as_table);
    let merged_widgets = merged_section
        .and_then(|dw| dw.get("widget"))
        .and_then(Toml::as_table);
    let merged_widgets = match merged_widgets {
        Some(table) if !table.is_empty() => table,
        _ => return,
    };
    let export_section = match root.get_mut(section).and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    if let Some(order) = merged_section.and_then(|dw| dw.get("widget_order")) {
        export_section.insert("widget_order".to_string(), order.clone());
    }
    let widgets = export_section
        .entry("widget".to_string())
        .or_insert_with(|| Toml::Table(toml::map::Map::new()));
    let widgets = widgets.as_table_mut().unwrap();
    for (name, spec) in merged_widgets {
        let type_name = spec
            .get("type")
            .and_then(Toml::as_str)
            .unwrap_or("clock");
        widgets
            .entry(name.clone())
            .or_insert_with(|| Toml::Table(default_placement_widget_table(type_name)));
    }
}

fn rebuild_desktop_widgets_from_merged(root: &mut Toml, merged: &Toml) {
    rebuild_placement_widgets_from_merged("desktop_widgets", root, merged);
}

fn rebuild_lockscreen_widgets_from_merged(root: &mut Toml, merged: &Toml) {
    rebuild_placement_widgets_from_merged("lockscreen_widgets", root, merged);
}

fn rebuild_calendar_accounts_from_merged(root: &mut Toml, merged: &Toml) {
    let merged_accounts = merged
        .get("calendar")
        .and_then(|calendar| calendar.get("account"))
        .and_then(Toml::as_table);
    let merged_accounts = match merged_accounts {
        Some(table) if !table.is_empty() => table,
        _ => return,
    };
    let calendar = match root.get_mut("calendar").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    let accounts = calendar
        .entry("account".to_string())
        .or_insert_with(|| Toml::Table(toml::map::Map::new()));
    let accounts = accounts.as_table_mut().unwrap();
    for name in merged_accounts.keys() {
        accounts
            .entry(name.clone())
            .or_insert_with(|| Toml::Table(default_calendar_account_export_table()));
    }
}

/// Schema fields emitted only when set (e.g. `shell.panel.glass`) still live under an exported ancestor.
fn is_allowed_merged_extra_leaf(path: &str, merged: &Toml, default: &Toml) -> bool {
    if dot_path_exists(default, path) || dot_path_get(merged, path).is_none() {
        return false;
    }
    if !is_merged_leaf_exported(path, default) {
        return false;
    }
    let mut prefix = path;
    while let Some((parent, _)) = prefix.rsplit_once('.') {
        if dot_path_exists(default, parent) {
            return true;
        }
        prefix = parent;
    }
    false
}

fn default_launcher_provider_export_table() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("prefix".to_string(), Toml::String(String::new()));
    Toml::Table(table)
}

fn rebuild_launcher_providers_from_merged(root: &mut Toml, merged: &Toml) {
    let merged_providers = merged
        .get("shell")
        .and_then(|shell| shell.get("launcher"))
        .and_then(|launcher| launcher.get("providers"))
        .and_then(Toml::as_table);
    let merged_providers = match merged_providers {
        Some(table) if !table.is_empty() => table,
        _ => return,
    };
    let shell = match root.get_mut("shell").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    let launcher = shell
        .entry("launcher".to_string())
        .or_insert_with(|| Toml::Table(toml::map::Map::new()));
    let launcher = launcher.as_table_mut().unwrap();
    let providers = launcher
        .entry("providers".to_string())
        .or_insert_with(|| Toml::Table(toml::map::Map::new()));
    let providers = providers.as_table_mut().unwrap();
    for name in merged_providers.keys() {
        providers
            .entry(name.clone())
            .or_insert_with(|| default_launcher_provider_export_table());
    }
}

fn is_merged_widget_path(path: &str, merged: &Toml) -> bool {
    let parts = path.split('.').collect::<Vec<_>>();
    if parts.first().map(|s| *s) != Some("widget") || parts.len() < 2 {
        return false;
    }
    merged_widget_names(merged).iter().any(|name| name == parts[1])
}

/// Merged config may only touch export paths, or extra `bar.<name>` / `widget.<name>` subtrees from user config.
fn merged_export_is_patchable(store: &ConfigStore) -> bool {
    let merged = store.merged_for_config_export();
    let default = default_export_root();
    let mut leaves = Vec::new();
    collect_leaf_paths(&merged, "", &mut leaves);
    leaves.iter().all(|path| {
        if !is_merged_leaf_exported(path, default) {
            return true;
        }
        if is_wallpaper_runtime_merged_path(path) || is_calendar_account_parse_only_path(path) {
            return true;
        }
        dot_path_exists(default, path)
            || is_allowed_extra_bar_path(path, &merged)
            || is_merged_widget_path(path, &merged)
            || is_allowed_calendar_account_path(path, &merged)
            || is_allowed_desktop_widgets_path(path, &merged)
            || is_allowed_lockscreen_widgets_path(path, &merged)
            || is_allowed_merged_extra_leaf(path, &merged, default)
    })
}

/// Behavior names in export order: explicit `behavior_order` first, then unlisted behaviors
/// in merged table order (C++ `idleSchema` finalizer).
fn merged_idle_behavior_names(merged: &Toml) -> Option<Vec<String>> {
    let idle = merged.get("idle").and_then(Toml::as_table)?;
    let behavior = idle.get("behavior").and_then(Toml::as_table)?;
    if behavior.is_empty() {
        return None;
    }
    let mut names = Vec::new();
    if let Some(order) = idle.get("behavior_order").and_then(Toml::as_array) {
        for value in order {
            if let Some(name) = value.as_str() {
                if behavior.contains_key(name) {
                    names.push(name.to_string());
                }
            }
        }
    }
    for key in behavior.keys() {
        if !names.iter().any(|name| name == key) {
            names.push(key.clone());
        }
    }
    Some(names)
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
    let names = merged_idle_behavior_names(merged).unwrap_or_default();
    if names.is_empty() {
        return;
    }

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
const BAR_RADIUS_CORNER_KEYS: [&str; 4] = [
    "radius_top_left",
    "radius_top_right",
    "radius_bottom_left",
    "radius_bottom_right",
];

/// C++ `BarConfig` applies a single user `radius` to every corner on export.
fn sync_bar_radii_from_merged(root: &mut Toml, merged: &Toml) {
    let merged_bar = match merged.get("bar").and_then(Toml::as_table) {
        Some(table) => table,
        None => return,
    };
    let export_bar = match root.get_mut("bar").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    for (name, merged_entry) in merged_bar {
        if name == "order" {
            continue;
        }
        let merged_table = match merged_entry.as_table() {
            Some(table) => table,
            None => continue,
        };
        let base = merged_table.get("radius").and_then(Toml::as_integer);
        if base.is_none() {
            continue;
        }
        let user_set_corner = BAR_RADIUS_CORNER_KEYS
            .iter()
            .any(|key| merged_table.contains_key(*key));
        if user_set_corner {
            continue;
        }
        let export_table = match export_bar.get_mut(name).and_then(Toml::as_table_mut) {
            Some(table) => table,
            None => continue,
        };
        let base = base.unwrap();
        export_table.insert("radius".to_string(), Toml::Integer(base));
        for key in BAR_RADIUS_CORNER_KEYS {
            export_table.insert(key.to_string(), Toml::Integer(base));
        }
    }
}

/// C++ `barConfigTable` emits each monitor override as a full resolved bar table + `match`.
fn rebuild_bar_monitors_from_merged(root: &mut Toml, merged: &Toml) {
    let merged_bar = match merged.get("bar").and_then(Toml::as_table) {
        Some(table) => table,
        None => return,
    };
    let export_bar = match root.get_mut("bar").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    for bar_name in merged_bar_names(merged) {
        let merged_monitors = merged_bar
            .get(&bar_name)
            .and_then(|bar| bar.get("monitor"))
            .and_then(Toml::as_table);
        let merged_monitors = match merged_monitors {
            Some(table) if !table.is_empty() => table,
            _ => continue,
        };
        let template = export_bar.get(&bar_name).and_then(|bar| {
            let mut table = bar.as_table()?.clone();
            table.remove("monitor");
            Some(Toml::Table(table))
        });
        let template = match template {
            Some(table) => table,
            None => continue,
        };
        let export_bar_table = match export_bar.get_mut(&bar_name).and_then(Toml::as_table_mut) {
            Some(table) => table,
            None => continue,
        };
        let monitors = export_bar_table
            .entry("monitor".to_string())
            .or_insert_with(|| Toml::Table(toml::map::Map::new()));
        let monitors = monitors.as_table_mut().unwrap();
        for mon_name in merged_monitors.keys() {
            monitors
                .entry(mon_name.clone())
                .or_insert_with(|| {
                    let mut seeded = template.clone();
                    if let Toml::Table(table) = &mut seeded {
                        table.insert("match".to_string(), Toml::String(mon_name.clone()));
                        // Monitor overrides only emit `position` when the override sets it.
                        table.remove("position");
                    }
                    seeded
                });
        }
    }
}

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
        (Toml::Float(_), Toml::Integer(value)) => Toml::Float(f32_bits(*value as f64)),
        (Toml::Float(_), Toml::Float(value)) => Toml::Float(f32_bits(*value)),
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
            if patch_table.is_empty() {
                return template.clone();
            }
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
    let default = default_export_root();
    let mut root = default.clone();
    let merged = store.merged_for_config_export();
    rebuild_bar_from_merged(&mut root, &merged);
    rebuild_bar_monitors_from_merged(&mut root, &merged);
    rebuild_widget_from_merged(&mut root, &merged);
    rebuild_launcher_providers_from_merged(&mut root, &merged);
    rebuild_calendar_accounts_from_merged(&mut root, &merged);
    rebuild_desktop_widgets_from_merged(&mut root, &merged);
    rebuild_lockscreen_widgets_from_merged(&mut root, &merged);
    rebuild_idle_from_merged(&mut root, &merged);
    let mut leaves = Vec::new();
    collect_leaf_paths(&merged, "", &mut leaves);
    for path in leaves {
        if !is_merged_leaf_exported(&path, default)
            || is_wallpaper_runtime_merged_path(&path)
            || is_calendar_account_parse_only_path(&path)
        {
            continue;
        }
        let value = match dot_path_get(&merged, &path) {
            Some(value) => value,
            None => continue,
        };
        if let Some(template) = dot_path_get(&root, &path) {
            set_dot_path(&mut root, &path, coerce_patch_value(&template, &value));
        } else if is_allowed_extra_bar_path(&path, &merged)
            || is_merged_widget_path(&path, &merged)
            || is_allowed_calendar_account_path(&path, &merged)
            || is_allowed_desktop_widgets_path(&path, &merged)
            || is_allowed_lockscreen_widgets_path(&path, &merged)
            || is_allowed_merged_extra_leaf(&path, &merged, default)
        {
            set_dot_path(&mut root, &path, value);
        }
    }
    sync_bar_radii_from_merged(&mut root, &merged);
    finalize_serialized_export(&mut root);
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

    fn first_unpatchable_merged_leaf(store: &ConfigStore) -> Option<String> {
        let merged = store.merged_for_config_export();
        let default = default_export_root();
        let mut leaves = Vec::new();
        collect_leaf_paths(&merged, "", &mut leaves);
        leaves.into_iter().find(|path| {
            if !is_merged_leaf_exported(path, default) {
                return false;
            }
            if is_wallpaper_runtime_merged_path(path) || is_calendar_account_parse_only_path(path) {
                return false;
            }
            !dot_path_exists(default, path)
                && !is_allowed_extra_bar_path(path, &merged)
                && !is_merged_widget_path(path, &merged)
                && !is_allowed_calendar_account_path(path, &merged)
                && !is_allowed_desktop_widgets_path(path, &merged)
                && !is_allowed_lockscreen_widgets_path(path, &merged)
                && !is_allowed_merged_extra_leaf(path, &merged, default)
        })
    }

    #[test]
    fn example_toml_is_rust_export_patchable() {
        let example = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../example.toml");
        if !example.is_file() {
            return;
        }
        let root = std::env::temp_dir().join(format!(
            "helix-export-example-{}",
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
        std::fs::copy(&example, config_dir.join("config.toml")).unwrap();
        std::fs::write(state_dir.join("settings.toml"), "\n").unwrap();
        let paths = ConfigPaths {
            config_dir,
            settings_toml: state_dir.join("settings.toml"),
        };
        let store = ConfigStore::load(paths.clone());
        if rust_export_toml(&paths).is_none() {
            let bad = first_unpatchable_merged_leaf(&store).unwrap_or_else(|| "?".into());
            panic!("example.toml not patchable for rust export (first leaf: {bad})");
        }
        let _ = std::fs::remove_dir_all(root);
    }
}

/// Effective export when the merged user config is patchable; else `None`.
pub fn rust_export_from_store(store: &ConfigStore) -> Option<Toml> {
    if !merged_export_is_patchable(store) {
        return None;
    }
    Some(patch_default_export(store))
}

pub fn rust_export_toml(paths: &ConfigPaths) -> Option<Toml> {
    rust_export_from_store(&ConfigStore::load(paths.clone()))
}
