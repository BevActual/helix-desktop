//! Control Center Home shortcut tiles (`shortcut-list` / `control_center.shortcuts`).

use serde_json::{json, Value};

use super::settings::ConfigStore;

pub const BUILT_INS: &[(&str, &str)] = &[
    ("wifi", "Wi-Fi"),
    ("bluetooth", "Bluetooth"),
    ("nightlight", "Night Light"),
    ("notification", "Do Not Disturb"),
    ("dark_mode", "Appearance"),
    ("caffeine", "Keep Awake"),
    ("audio", "Sound"),
    ("mic_mute", "Microphone"),
    ("power_profile", "Power Profile"),
    ("media", "Media"),
    ("system", "System Monitor"),
    ("keyboard_layout", "Keyboard Layout"),
    ("wallpaper", "Wallpaper"),
    ("session", "Session"),
    ("clipboard", "Clipboard"),
    ("screenshot", "Screenshot"),
    ("pick_color", "Color Picker"),
    ("ocr", "Extract Text"),
    ("scan_qr", "Scan QR Code"),
    ("screen_recorder", "Screen Recording"),
    ("screen_replay", "Screen Replay"),
    ("privacy", "Privacy"),
];

const SIZES: &[&str] = &["small", "wide", "large"];

pub fn shortcut_available(id: &str, store: &ConfigStore) -> bool {
    match id {
        "system" => store
            .get_json_value("system.monitor.enabled")
            .and_then(|v| v.as_bool())
            .unwrap_or(true),
        "clipboard" => store
            .get_json_value("shell.clipboard_enabled")
            .and_then(|v| v.as_bool())
            .unwrap_or(true),
        _ => true,
    }
}

pub fn read_shortcuts(store: &ConfigStore) -> Value {
    store
        .get_json_value("control_center.shortcuts")
        .and_then(|value| value.as_array().cloned())
        .map(Value::Array)
        .unwrap_or_else(|| Value::Array(Vec::new()))
}

pub fn catalog_entry(store: &ConfigStore) -> Value {
    let value = read_shortcuts(store);
    let mut options: Vec<Value> = Vec::new();
    let mut labels: Vec<Value> = Vec::new();
    for (id, label) in BUILT_INS {
        if shortcut_available(id, store) {
            options.push(Value::String((*id).to_string()));
            labels.push(Value::String((*label).to_string()));
        }
    }
    if let Some(items) = value.as_array() {
        for item in items {
            if let Some(type_name) = item.get("type").and_then(Value::as_str) {
                if !options.iter().any(|opt| opt.as_str() == Some(type_name)) {
                    options.push(Value::String(type_name.to_string()));
                    labels.push(Value::String(type_name.to_string()));
                }
            }
        }
    }
    json!({
        "id": "control_center.shortcuts",
        "page": "desktop-dock",
        "title": "Quick shortcuts",
        "group": "Control Center",
        "type": "shortcut-list",
        "value": value,
        "options": options,
        "option-labels": labels,
        "overridden": store.overridden_path("control_center.shortcuts"),
        "description": "Add, resize and arrange shortcuts in Control Center Home. Saved extension shortcuts remain editable.",
    })
}

pub fn validate(catalog: &[Value], value: &Value) -> Result<Value, String> {
    let entry = catalog
        .iter()
        .find(|row| row.get("id").and_then(Value::as_str) == Some("control_center.shortcuts"))
        .ok_or_else(|| "Unknown ordered desktop list".to_string())?;
    let options = entry
        .get("options")
        .and_then(Value::as_array)
        .ok_or_else(|| "Unknown ordered desktop list".to_string())?;
    let items = value.as_array().ok_or("Expected at most 256 shortcuts")?;
    if items.len() > 256 {
        return Err("Expected at most 256 shortcuts".into());
    }
    let mut out = Vec::new();
    for item in items {
        let object = item.as_object().ok_or("Shortcut entries require a type and size")?;
        if object.len() != 2 || !object.contains_key("type") || !object.contains_key("size") {
            return Err("Shortcut entries require a type and size".into());
        }
        let type_name = object
            .get("type")
            .and_then(Value::as_str)
            .ok_or("Shortcut entries require a type and size")?;
        let size = object
            .get("size")
            .and_then(Value::as_str)
            .ok_or("Shortcut entries require a type and size")?;
        if type_name.is_empty()
            || type_name.len() > 255
            || type_name.chars().any(|c| c.is_control())
            || !SIZES.contains(&size)
        {
            return Err("Invalid shortcut type or size".into());
        }
        if !options.iter().any(|opt| opt.as_str() == Some(type_name)) {
            return Err("Unknown or disabled shortcut".into());
        }
        out.push(json!({ "type": type_name, "size": size }));
    }
    Ok(Value::Array(out))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::{ConfigPaths, ConfigStore};
    use serde_json::json;

    fn temp_store() -> ConfigStore {
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let root = std::env::temp_dir().join(format!("helix-shortcuts-{nanos}"));
        let paths = ConfigPaths {
            config_dir: root.join("config"),
            settings_toml: root.join("state/settings.toml"),
        };
        std::fs::create_dir_all(&paths.config_dir).unwrap();
        std::fs::create_dir_all(paths.settings_toml.parent().unwrap()).unwrap();
        ConfigStore::load(paths)
    }

    #[test]
    fn shortcut_round_trip() {
        let store = temp_store();
        let catalog = store.full_catalog(true, &[]);
        let value = json!([{ "type": "wifi", "size": "wide" }]);
        let validated = validate(&catalog, &value).unwrap();
        let mut store = store;
        store
            .set_json_value("control_center.shortcuts", validated)
            .unwrap();
        let next = store.full_catalog(true, &[]);
        let entry = next
            .iter()
            .find(|row| row.get("id").and_then(|id| id.as_str()) == Some("control_center.shortcuts"))
            .unwrap();
        assert_eq!(entry.get("value"), Some(&value));
    }
}
