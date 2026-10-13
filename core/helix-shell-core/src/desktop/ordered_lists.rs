//! Ordered-list settings (`UpdateOrderedList`) beyond bootstrap `application-list` rows.

use serde_json::{json, Value};

use super::keybinds::{self, KEYBIND_IDS};
use super::settings::ConfigStore;

const MAX_LIST: usize = 256;

pub const CONTROL_CENTER_TABS: &[(&str, &str)] = &[
    ("network", "Network"),
    ("bluetooth", "Bluetooth"),
    ("tailscale", "Tailscale"),
    ("notifications", "Notifications"),
    ("audio", "Sound"),
    ("screen-time", "Screen Time"),
    ("theme", "Appearance"),
    ("control-center", "Control Center"),
    ("launcher", "Launcher"),
    ("privacy-security", "Privacy & Security"),
    ("system", "System Monitor"),
    ("packages", "Software"),
    ("clock", "Clock"),
    ("desktop", "Desktop & Dock"),
    ("monitor", "Displays"),
    ("wallpaper", "Wallpaper"),
    ("power", "Power"),
    ("keybinds", "Keyboard"),
    ("shell", "Shell"),
    ("printers", "Printers"),
    ("camera", "Camera"),
    ("calendar", "Calendar"),
    ("weather", "Weather"),
    ("media", "Media"),
    ("capture", "Capture"),
    ("clipboard", "Clipboard"),
    ("ai", "AI Usage"),
    ("templates", "Templates"),
    ("hooks", "Automation"),
    ("niri", "Niri"),
];

const KEY_CHORD_META: &[(&str, &str)] = &[
    ("keybinds.validate", "Confirm or activate"),
    ("keybinds.cancel", "Cancel or close"),
    ("keybinds.left", "Move left"),
    ("keybinds.right", "Move right"),
    ("keybinds.up", "Move up"),
    ("keybinds.down", "Move down"),
    ("keybinds.tab_next", "Next tab"),
    ("keybinds.tab_previous", "Previous tab"),
    ("keybinds.copy", "Copy selected item"),
    ("keybinds.save", "Save selected item"),
    ("keybinds.delete", "Delete selected item"),
];

pub fn requires_update_ordered_list(id: &str) -> bool {
    if id.starts_with("keybinds.")
        || id == "control_center.shortcuts"
        || id == "control_center.visible_tabs"
        || id == "theme.templates.builtin_ids"
        || id == "theme.templates.community_ids"
        || id == "wallpaper.transition"
        || id == "notification.filter"
    {
        return true;
    }
    if let Some(rest) = id.strip_prefix("bar:") {
        if let Some((_, field)) = rest.rsplit_once(':') {
            return matches!(field, "start" | "center" | "end");
        }
    }
    super::ordered_catalog::is_display_list_id(id)
}

pub fn supports_ordered_type(type_name: &str) -> bool {
    matches!(
        type_name,
        "application-list"
            | "widget-list"
            | "display-list"
            | "notification-rules"
            | "tab-list"
            | "shortcut-list"
            | "key-chords"
            | "template-list"
            | "transition-list"
    )
}

fn string_array(value: &Value) -> Option<Vec<String>> {
    value
        .as_array()
        .map(|items| {
            items
                .iter()
                .filter_map(|item| item.as_str().map(str::to_string))
                .collect()
        })
}

fn read_string_list(store: &ConfigStore, toml_id: &str) -> Vec<String> {
    store
        .get_raw_list(toml_id)
        .and_then(|value| string_array(&value))
        .unwrap_or_default()
}

fn overridden_list(store: &ConfigStore, toml_id: &str) -> bool {
    store.overridden_path(toml_id)
}

fn key_chord_entry(store: &ConfigStore, id: &str, title: &str) -> Value {
    let chords = read_string_list(store, id);
    json!({
        "id": id,
        "page": "keyboard",
        "title": title,
        "group": "Helix Shell Navigation",
        "type": "key-chords",
        "value": chords,
        "overridden": overridden_list(store, id),
    })
}

fn visible_control_center_tabs(store: &ConfigStore) -> Vec<String> {
    let hidden = read_string_list(store, "control_center.hidden_tabs");
    let mut visible = Vec::new();
    for (id, _) in CONTROL_CENTER_TABS {
        let mut shown = !hidden.iter().any(|tab| tab == id);
        if *id == "ai" && hidden.iter().any(|tab| tab == "ai-usage") {
            shown = false;
        }
        if *id == "capture"
            && (hidden.iter().any(|tab| tab == "screenshot")
                || hidden.iter().any(|tab| tab == "screen-record"))
        {
            shown = false;
        }
        if shown {
            visible.push(id.to_string());
        }
    }
    visible
}

fn tab_list_entry(store: &ConfigStore) -> Value {
    let tab_count = CONTROL_CENTER_TABS.len();
    let options: Vec<Value> = CONTROL_CENTER_TABS
        .iter()
        .map(|(id, _)| Value::String((*id).to_string()))
        .collect();
    let labels: Vec<Value> = CONTROL_CENTER_TABS
        .iter()
        .map(|(_, name)| Value::String((*name).to_string()))
        .collect();
    json!({
        "id": "control_center.visible_tabs",
        "page": "desktop-dock",
        "title": "Visible pages",
        "group": "Control Center",
        "type": "tab-list",
        "value": visible_control_center_tabs(store),
        "options": options,
        "option-labels": labels,
        "minimum": 0,
        "maximum": tab_count,
        "overridden": overridden_list(store, "control_center.hidden_tabs"),
        "description": "Choose which built-in pages appear in Control Center. Home always remains available; existing plugin visibility is preserved.",
    })
}

/// Extra catalog rows merged after bootstrap `GetSettings` entries.
pub fn extension_entries(store: &ConfigStore, include_desktop: bool) -> Vec<Value> {
    let mut out = Vec::new();
    for (id, title) in KEY_CHORD_META {
        if KEYBIND_IDS.contains(id) {
            out.push(key_chord_entry(store, id, title));
        }
    }
    out.push(tab_list_entry(store));
    out.push(super::control_center_shortcuts::catalog_entry(store));
    out.push(super::notification_rules::catalog_entry(store));
    out.extend(super::ordered_catalog::entries(store, include_desktop));
    out
}

pub fn catalog_entry<'a>(catalog: &'a [Value], id: &str) -> Option<&'a Value> {
    catalog.iter().find(|entry| entry.get("id").and_then(Value::as_str) == Some(id))
}

pub fn current_value(_store: &ConfigStore, catalog: &[Value], id: &str) -> Option<Value> {
    if let Some(entry) = catalog_entry(catalog, id) {
        return entry.get("value").cloned();
    }
    None
}

fn validate_string_list(
    entry: &Value,
    value: &Value,
    allow_slash: bool,
    require_known_options: bool,
    unknown_option_msg: &str,
) -> Result<Value, String> {
    let items = value.as_array().ok_or("Expected at most 256 application IDs")?;
    if items.len() > MAX_LIST {
        return Err("Expected at most 256 application IDs".into());
    }
    let options = entry.get("options").and_then(Value::as_array);
    let mut seen = std::collections::BTreeSet::new();
    let mut out = Vec::new();
    for item in items {
        let text = item.as_str().ok_or("Application IDs must be text")?;
        if text.is_empty()
            || text.len() > 255
            || (!allow_slash && text.contains('/'))
            || text.chars().any(|c| c.is_control())
            || !seen.insert(text.to_string())
        {
            return Err("Invalid or duplicate list entry".into());
        }
        if require_known_options {
            let known = options
                .and_then(|opts| opts.iter().find(|opt| opt.as_str() == Some(text)))
                .is_some();
            if !known {
                let current = entry.get("value").and_then(Value::as_array);
                let already_selected = current
                    .and_then(|items| items.iter().find(|item| item.as_str() == Some(text)))
                    .is_some();
                if !already_selected {
                    return Err(unknown_option_msg.into());
                }
            }
        }
        out.push(Value::String(text.to_string()));
    }
    Ok(Value::Array(out))
}

pub fn validate_update(catalog: &[Value], id: &str, value: &Value) -> Result<Value, String> {
    let entry = catalog_entry(catalog, id).ok_or_else(|| "Unknown ordered desktop list".to_string())?;
    let type_name = entry
        .get("type")
        .and_then(Value::as_str)
        .ok_or_else(|| "Unknown ordered desktop list".to_string())?;
    match type_name {
        "key-chords" => {
            let strings = value
                .as_array()
                .ok_or("Provide between one and sixteen key shortcuts")?
                .iter()
                .map(|item| {
                    item.as_str()
                        .ok_or_else(|| "Key shortcuts must be short text values".to_string())
                        .map(str::to_string)
                })
                .collect::<Result<Vec<_>, _>>()?;
            let validated = keybinds::update(catalog, id, &strings).map_err(|error| match error {
                keybinds::KeybindError::NotShown => "Unknown ordered desktop list".to_string(),
                keybinds::KeybindError::Empty => "Provide between one and sixteen key shortcuts".to_string(),
                keybinds::KeybindError::TooMany => "Provide between one and sixteen key shortcuts".to_string(),
                keybinds::KeybindError::BadChord => "Invalid key shortcut".to_string(),
                keybinds::KeybindError::Duplicate => "Duplicate key shortcut".to_string(),
                keybinds::KeybindError::SuperNotAllowed => "Invalid key shortcut".to_string(),
            })?;
            Ok(Value::Array(
                validated.into_iter().map(Value::String).collect(),
            ))
        }
        "application-list" => super::settings::validate_application_list(value.clone()),
        "tab-list" => validate_string_list(
            entry,
            value,
            false,
            true,
            "Unknown Control Center page",
        ),
        "display-list" => validate_string_list(entry, value, true, false, "Invalid or duplicate list entry"),
        "widget-list" => validate_string_list(entry, value, false, true, "Unknown widget instance"),
        "template-list" => validate_string_list(entry, value, false, true, "Unknown template"),
        "transition-list" => {
            if value.as_array().is_some_and(|a| a.is_empty()) {
                return Err("Choose at least one wallpaper transition".into());
            }
            validate_string_list(entry, value, false, true, "Unknown wallpaper transition")
        }
        "shortcut-list" => super::control_center_shortcuts::validate(catalog, value),
        "notification-rules" => super::notification_rules::validate(value),
        _ => Err("Unknown ordered desktop list".into()),
    }
}

pub fn apply_update(store: &mut ConfigStore, id: &str, value: Value) -> Result<(), String> {
    if id == "control_center.visible_tabs" {
        let visible = value
            .as_array()
            .ok_or("Expected tab list")?
            .iter()
            .filter_map(|item| item.as_str())
            .collect::<Vec<_>>();
        let mut builtin: std::collections::BTreeSet<String> = CONTROL_CENTER_TABS
            .iter()
            .map(|(tab, _)| (*tab).to_string())
            .collect();
        builtin.insert("ai-usage".into());
        builtin.insert("screenshot".into());
        builtin.insert("screen-record".into());
        let mut hidden = read_string_list(store, "control_center.hidden_tabs");
        hidden.retain(|tab| !builtin.contains(tab));
        for (tab, _) in CONTROL_CENTER_TABS {
            if !visible.iter().any(|shown| shown == tab) {
                hidden.push(tab.to_string());
            }
        }
        return store.set_raw_list("control_center.hidden_tabs", Value::Array(
            hidden.into_iter().map(Value::String).collect(),
        ));
    }
    store.set_raw_list(super::settings::storage_path(id).as_ref(), value)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::desktop::settings::{ConfigPaths, ConfigStore};

    fn temp_store() -> ConfigStore {
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let root = std::env::temp_dir().join(format!("helix-ordered-{nanos}"));
        let paths = ConfigPaths {
            config_dir: root.join("config"),
            settings_toml: root.join("state/settings.toml"),
        };
        std::fs::create_dir_all(&paths.config_dir).unwrap();
        std::fs::create_dir_all(paths.settings_toml.parent().unwrap()).unwrap();
        ConfigStore::load(paths)
    }

    #[test]
    fn wallpaper_transition_round_trip() {
        let store = temp_store();
        let catalog = store.full_catalog(true);
        let value = json!(["fade", "wipe"]);
        let validated =
            validate_update(&catalog, "wallpaper.transition", &value).unwrap();
        let mut store = store;
        apply_update(&mut store, "wallpaper.transition", validated).unwrap();
        let next = store.full_catalog(true);
        let current = current_value(&store, &next, "wallpaper.transition").unwrap();
        assert_eq!(current, value);
    }

    #[test]
    fn tab_visibility_round_trip() {
        let store = temp_store();
        let catalog = store.full_catalog(true);
        let visible = json!(["network", "bluetooth"]);
        let validated =
            validate_update(&catalog, "control_center.visible_tabs", &visible).unwrap();
        let mut store = store;
        apply_update(&mut store, "control_center.visible_tabs", validated).unwrap();
        let next = store.full_catalog(true);
        let current = current_value(&store, &next, "control_center.visible_tabs").unwrap();
        assert_eq!(current, visible);
        let hidden = store.get_raw_list("control_center.hidden_tabs").unwrap();
        assert!(hidden
            .as_array()
            .unwrap()
            .iter()
            .any(|tab| tab.as_str() == Some("theme")));
    }
}
