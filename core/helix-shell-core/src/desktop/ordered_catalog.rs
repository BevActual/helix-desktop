//! Ordered-list catalog rows from static metadata, config, and bar/widget tables.

use std::collections::BTreeSet;
use std::path::PathBuf;

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
    },
    ListMeta {
        id: "notification.monitors",
        page: "notifications",
        title: "Notification displays",
        group: "Displays",
        list_type: "display-list",
    },
    ListMeta {
        id: "osd.monitors",
        page: "desktop-dock",
        title: "Indicator displays",
        group: "On-Screen Indicators",
        list_type: "display-list",
    },
    ListMeta {
        id: "dock.monitors",
        page: "desktop-dock",
        title: "Dock displays",
        group: "Dock",
        list_type: "display-list",
    },
    ListMeta {
        id: "shell.mpris.blacklist",
        page: "desktop-dock",
        title: "Hidden media players",
        group: "Media",
        list_type: "display-list",
    },
    ListMeta {
        id: "brightness.ignore_mmids",
        page: "displays",
        title: "Ignored DDC monitor IDs",
        group: "Brightness",
        list_type: "display-list",
    },
    ListMeta {
        id: "control_center.network.hidden_ssids",
        page: "network",
        title: "Hidden networks",
        group: "Wi-Fi Preferences",
        list_type: "display-list",
    },
];

fn string_list_value(store: &ConfigStore, id: &str) -> Value {
    store
        .get_raw_list(id)
        .unwrap_or_else(|| Value::Array(Vec::new()))
}

fn display_list_entry(store: &ConfigStore, meta: &ListMeta) -> Value {
    json!({
        "id": meta.id,
        "page": meta.page,
        "title": meta.title,
        "group": meta.group,
        "type": meta.list_type,
        "value": string_list_value(store, meta.id),
        "options": [],
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

fn template_list_entry(store: &ConfigStore, id: &str, title: &str) -> Value {
    let templates = load_builtin_templates();
    let options: Vec<Value> = templates
        .iter()
        .map(|(template_id, _)| Value::String(template_id.clone()))
        .collect();
    let labels: Vec<Value> = templates
        .iter()
        .map(|(_, name)| Value::String(name.clone()))
        .collect();
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

fn bar_table(store: &ConfigStore) -> Option<toml::map::Map<String, Toml>> {
    store
        .merged_node("bar")
        .and_then(|node| node.as_table().cloned())
}

fn widget_names(store: &ConfigStore) -> Vec<String> {
    let mut names = BTreeSet::new();
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
                if let Some(array) = bar_table.and_then(|t| t.get(field)).and_then(Toml::as_array) {
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

fn bar_names(store: &ConfigStore) -> Vec<String> {
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
    let options: Vec<Value> = widget_names(store)
        .into_iter()
        .map(Value::String)
        .collect();
    let value = store
        .get_raw_list(storage.as_ref())
        .unwrap_or_else(|| Value::Array(Vec::new()));
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
pub fn entries(store: &ConfigStore, include_desktop: bool) -> Vec<Value> {
    let mut out = Vec::new();
    if include_desktop {
        for meta in DISPLAY_LISTS {
            out.push(display_list_entry(store, meta));
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
            out.push(bar_widget_entry(store, &bar_name, "center", "Center widgets"));
            out.push(bar_widget_entry(store, &bar_name, "end", "End widgets"));
        }
    }
    out
}
