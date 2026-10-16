//! Ordered-list catalog rows from static metadata, config, and bar/widget tables.

use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};

use serde_json::{json, Value};
use toml::Value as Toml;

use super::settings::ConfigStore;

pub const WALLPAPER_TRANSITIONS: &[(&str, &str)] = &[
    ("fade", "Fade"),
    ("wipe", "Wipe"),
    ("disc", "Disc"),
    ("stripes", "Stripes"),
    ("zoom", "Zoom"),
    ("honeycomb", "Honeycomb"),
];

struct ListMeta {
    id: &'static str,
    page: &'static str,
    title: &'static str,
    group: &'static str,
    list_type: &'static str,
    /// Options come from live compositor output names when available.
    monitor_picker: bool,
}

pub fn is_display_list_id(id: &str) -> bool {
    DISPLAY_LISTS.iter().any(|meta| meta.id == id)
}

const DISPLAY_LISTS: &[ListMeta] = &[
    ListMeta {
        id: "lockscreen.monitors",
        page: "privacy-security",
        title: "Lock-screen displays",
        group: "Lock Screen",
        list_type: "display-list",
        monitor_picker: true,
    },
    ListMeta {
        id: "notification.monitors",
        page: "notifications",
        title: "Notification displays",
        group: "Displays",
        list_type: "display-list",
        monitor_picker: true,
    },
    ListMeta {
        id: "osd.monitors",
        page: "desktop-dock",
        title: "Indicator displays",
        group: "On-Screen Indicators",
        list_type: "display-list",
        monitor_picker: true,
    },
    ListMeta {
        id: "dock.monitors",
        page: "desktop-dock",
        title: "Dock displays",
        group: "Dock",
        list_type: "display-list",
        monitor_picker: true,
    },
    ListMeta {
        id: "shell.mpris.blacklist",
        page: "desktop-dock",
        title: "Hidden media players",
        group: "Media",
        list_type: "display-list",
        monitor_picker: false,
    },
    ListMeta {
        id: "brightness.ignore_mmids",
        page: "displays",
        title: "Ignored DDC monitor IDs",
        group: "Brightness",
        list_type: "display-list",
        monitor_picker: false,
    },
    ListMeta {
        id: "control_center.network.hidden_ssids",
        page: "network",
        title: "Hidden networks",
        group: "Wi-Fi Preferences",
        list_type: "display-list",
        monitor_picker: false,
    },
];

fn string_list_value(store: &ConfigStore, id: &str) -> Value {
    store
        .get_raw_list(id)
        .unwrap_or_else(|| Value::Array(Vec::new()))
}

fn display_list_options(meta: &ListMeta, value: &Value, display_names: &[String]) -> Vec<Value> {
    let mut options = BTreeSet::new();
    if meta.monitor_picker {
        for name in display_names {
            options.insert(name.clone());
        }
    }
    if let Some(items) = value.as_array() {
        for item in items {
            if let Some(text) = item.as_str() {
                options.insert(text.to_string());
            }
        }
    }
    options.into_iter().map(Value::String).collect()
}

fn display_list_entry(store: &ConfigStore, meta: &ListMeta, display_names: &[String]) -> Value {
    let value = string_list_value(store, meta.id);
    let options = display_list_options(meta, &value, display_names);
    json!({
        "id": meta.id,
        "page": meta.page,
        "title": meta.title,
        "group": meta.group,
        "type": meta.list_type,
        "value": value,
        "options": options,
        "minimum": 0,
        "maximum": 256,
        "overridden": store.overridden_path(meta.id),
    })
}

fn transition_list_entry(store: &ConfigStore) -> Value {
    let options: Vec<Value> = WALLPAPER_TRANSITIONS
        .iter()
        .map(|(id, _)| Value::String((*id).to_string()))
        .collect();
    let labels: Vec<Value> = WALLPAPER_TRANSITIONS
        .iter()
        .map(|(_, label)| Value::String((*label).to_string()))
        .collect();
    let default_value: Vec<Value> = options.clone();
    let value = store
        .get_raw_list("wallpaper.transition")
        .unwrap_or(Value::Array(default_value));
    json!({
        "id": "wallpaper.transition",
        "page": "appearance",
        "title": "Transition styles",
        "group": "Wallpaper",
        "type": "transition-list",
        "value": value,
        "options": options,
        "option-labels": labels,
        "minimum": 0,
        "maximum": 256,
        "overridden": store.overridden_path("wallpaper.transition"),
    })
}

fn builtin_template_path() -> Option<PathBuf> {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let candidates = [
        manifest.join("../../assets/templates/builtin.toml"),
        manifest.join("../../../assets/templates/builtin.toml"),
    ];
    candidates.into_iter().find(|path| path.is_file())
}

fn load_builtin_templates() -> Vec<(String, String)> {
    let Some(path) = builtin_template_path() else {
        return Vec::new();
    };
    let Ok(text) = std::fs::read_to_string(path) else {
        return Vec::new();
    };
    let Ok(root) = text.parse::<Toml>() else {
        return Vec::new();
    };
    let Some(catalog) = root.get("catalog").and_then(Toml::as_table) else {
        return Vec::new();
    };
    let mut out = Vec::new();
    for (id, entry) in catalog {
        let name = entry
            .get("name")
            .and_then(Toml::as_str)
            .unwrap_or(id.as_str());
        out.push((id.clone(), name.to_string()));
    }
    out.sort_by(|left, right| left.0.cmp(&right.0));
    out
}

fn community_templates_cache_dir(store: &ConfigStore) -> PathBuf {
    store
        .paths()
        .settings_toml
        .parent()
        .map(|dir| dir.join("community-templates"))
        .unwrap_or_else(|| PathBuf::from("/tmp/helix/community-templates"))
}

fn parse_community_catalog_entry(item: &Value) -> Option<(String, String)> {
    let id = item
        .get("id")
        .or_else(|| item.get("name"))
        .and_then(Value::as_str)
        .filter(|text| !text.is_empty())?;
    let display = item
        .get("display_name")
        .or_else(|| item.get("displayName"))
        .and_then(Value::as_str)
        .filter(|text| !text.is_empty())
        .unwrap_or(id);
    Some((id.to_string(), display.to_string()))
}

fn load_community_templates(store: &ConfigStore) -> Vec<(String, String)> {
    let cache_dir = community_templates_cache_dir(store);
    let mut templates = BTreeSet::new();
    let catalog_path = cache_dir.join("catalog.json");
    if let Ok(text) = fs::read_to_string(&catalog_path) {
        if let Ok(root) = serde_json::from_str::<Value>(&text) {
            let entries = root
                .as_array()
                .or_else(|| root.get("templates").and_then(Value::as_array));
            if let Some(items) = entries {
                for item in items {
                    if let Some(pair) = parse_community_catalog_entry(item) {
                        templates.insert(pair);
                    }
                }
            }
        }
    }
    if let Ok(entries) = fs::read_dir(&cache_dir) {
        for entry in entries.flatten() {
            if !entry.file_type().is_ok_and(|kind| kind.is_dir()) {
                continue;
            }
            let id = entry.file_name().to_string_lossy().into_owned();
            if id.starts_with('.') {
                continue;
            }
            let toml_path = entry.path().join("template.toml");
            if !toml_path.is_file() {
                continue;
            }
            let name = read_community_template_name(&toml_path, &id);
            templates.insert((id, name));
        }
    }
    templates.into_iter().collect()
}

fn read_community_template_name(path: &Path, fallback: &str) -> String {
    let Ok(text) = fs::read_to_string(path) else {
        return fallback.to_string();
    };
    let Ok(root) = text.parse::<Toml>() else {
        return fallback.to_string();
    };
    let Some(table) = root.as_table() else {
        return fallback.to_string();
    };
    let Some(catalog) = table.get("catalog").and_then(Toml::as_table) else {
        return fallback.to_string();
    };
    let Some(info) = catalog.get(fallback).and_then(Toml::as_table) else {
        return fallback.to_string();
    };
    info.get("name")
        .and_then(Toml::as_str)
        .filter(|text| !text.is_empty())
        .unwrap_or(fallback)
        .to_string()
}

fn template_options(templates: &[(String, String)]) -> (Vec<Value>, Vec<Value>) {
    let options = templates
        .iter()
        .map(|(template_id, _)| Value::String(template_id.clone()))
        .collect();
    let labels = templates
        .iter()
        .map(|(_, name)| Value::String(name.clone()))
        .collect();
    (options, labels)
}

fn template_list_entry(store: &ConfigStore, id: &str, title: &str) -> Value {
    let templates = if id == "theme.templates.community_ids" {
        load_community_templates(store)
    } else {
        load_builtin_templates()
    };
    let (options, labels) = template_options(&templates);
    let value = store
        .get_raw_list(id)
        .unwrap_or_else(|| Value::Array(Vec::new()));
    json!({
        "id": id,
        "page": "appearance",
        "title": title,
        "group": "Templates",
        "type": "template-list",
        "value": value,
        "options": options,
        "option-labels": labels,
        "minimum": 0,
        "maximum": 256,
        "overridden": store.overridden_path(id),
    })
}

pub(crate) fn bar_table(store: &ConfigStore) -> Option<toml::map::Map<String, Toml>> {
    store
        .merged_node("bar")
        .and_then(|node| node.as_table().cloned())
}

/// Widget instances a pristine C++ config defines (`Config::widgets`).
const DEFAULT_WIDGETS: [&str; 17] = [
    "active_window",
    "clock",
    "cpu",
    "date",
    "group:status",
    "input_volume",
    "keyboard_layout",
    "lock_keys",
    "media",
    "network_rx",
    "network_tx",
    "notifications",
    "output_volume",
    "ram",
    "spacer",
    "temp",
    "tray",
];

fn widget_names(store: &ConfigStore) -> Vec<String> {
    let mut names: BTreeSet<String> = DEFAULT_WIDGETS.iter().map(|w| (*w).to_string()).collect();
    if let Some(table) = store
        .merged_node("widget")
        .and_then(|node| node.as_table().cloned())
    {
        for key in table.keys() {
            names.insert(key.clone());
        }
    }
    if let Some(bars) = bar_table(store) {
        for (name, bar) in bars {
            if name == "order" {
                continue;
            }
            let bar_table = bar.as_table();
            for field in ["start", "center", "end"] {
                if let Some(array) = bar_table
                    .and_then(|t| t.get(field))
                    .and_then(Toml::as_array)
                {
                    for item in array {
                        if let Some(text) = item.as_str() {
                            names.insert(text.to_string());
                        }
                    }
                }
            }
        }
    }
    names.into_iter().collect()
}

/// Whether no bar is configured, i.e. the C++ default bar (`default`) is in effect.
fn uses_default_bar(store: &ConfigStore) -> bool {
    let Some(bars) = bar_table(store) else {
        return true;
    };
    if bars
        .keys()
        .any(|name| *name != "order" && *name != "default")
    {
        return false;
    }
    if bars.contains_key("default") {
        return false;
    }
    match bars.get("order").and_then(Toml::as_array) {
        None => true,
        Some(order) if order.is_empty() => true,
        Some(order) => order.iter().all(|item| item.as_str() == Some("default")),
    }
}

pub(crate) fn bar_names(store: &ConfigStore) -> Vec<String> {
    if uses_default_bar(store) {
        return vec!["default".to_string()];
    }
    let Some(bars) = bar_table(store) else {
        return Vec::new();
    };
    if let Some(order) = bars.get("order").and_then(Toml::as_array) {
        let mut names = Vec::new();
        for item in order {
            if let Some(name) = item.as_str() {
                if bars.contains_key(name) {
                    names.push(name.to_string());
                }
            }
        }
        if !names.is_empty() {
            return names;
        }
    }
    bars.keys()
        .filter(|name| *name != "order")
        .cloned()
        .collect()
}

fn bar_widget_entry(store: &ConfigStore, bar_name: &str, field: &str, title: &str) -> Value {
    let id = format!("bar:{bar_name}:{field}");
    let storage = super::settings::storage_path(&id);
    let options: Vec<Value> = widget_names(store).into_iter().map(Value::String).collect();
    let default_widgets: &[&str] = if uses_default_bar(store) {
        match field {
            "start" => &["clock"],
            "end" => &["media", "notifications", "tray", "group:status"],
            _ => &[],
        }
    } else {
        &[]
    };
    let value = store
        .get_raw_list(storage.as_ref())
        .unwrap_or_else(|| json!(default_widgets));
    json!({
        "id": id,
        "page": "desktop-dock",
        "title": title,
        "group": format!("Bar · {bar_name} · Widgets"),
        "type": "widget-list",
        "value": value,
        "options": options,
        "minimum": 0,
        "maximum": 256,
        "overridden": store.overridden_path(storage.as_ref()),
    })
}

/// Static and config-backed ordered-list rows (display lists, templates, bar widgets).
pub fn entries(store: &ConfigStore, include_desktop: bool, display_names: &[String]) -> Vec<Value> {
    let mut out = Vec::new();
    if include_desktop {
        for meta in DISPLAY_LISTS {
            out.push(display_list_entry(store, meta, display_names));
        }
        out.push(transition_list_entry(store));
        out.push(template_list_entry(
            store,
            "theme.templates.builtin_ids",
            "Built-in template selection",
        ));
        out.push(template_list_entry(
            store,
            "theme.templates.community_ids",
            "Community template selection",
        ));
        for bar_name in bar_names(store) {
            out.push(bar_widget_entry(store, &bar_name, "start", "Start widgets"));
            out.push(bar_widget_entry(
                store,
                &bar_name,
                "center",
                "Center widgets",
            ));
            out.push(bar_widget_entry(store, &bar_name, "end", "End widgets"));
        }
    }
    out
}

#[cfg(test)]
mod bar_names_tests {
    use super::*;
    use crate::settings::{ConfigPaths, ConfigStore};
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn config_smoke_shape_resolves_main_bar() {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let home = std::env::temp_dir().join(format!("helix-bar-smoke-{nanos}"));
        fs::create_dir_all(home.join("helix")).unwrap();
        fs::create_dir_all(home.join("state/helix")).unwrap();
        fs::write(
            home.join("helix/config.toml"),
            r#"[bar]
order = ["main"]

[bar.main]
enabled = true
thickness = 34

[idle.behavior.lock]
timeout = 420
enabled = true
action = "lock"

[theme.templates]
builtin_ids = ["helix", "qt"]
"#,
        )
        .unwrap();
        // SAFETY: single-threaded test; `from_env` prefers HELIX_* over XDG_*.
        unsafe {
            std::env::remove_var("HELIX_CONFIG_HOME");
            std::env::remove_var("HELIX_STATE_HOME");
            std::env::set_var("XDG_CONFIG_HOME", &home);
            std::env::set_var("XDG_STATE_HOME", home.join("state"));
        }
        let store = ConfigStore::load(ConfigPaths::from_env());
        assert_eq!(bar_names(&store), vec!["main".to_string()]);
    }
}
