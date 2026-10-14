//! Desktop-only catalog rows from live `config.toml` (C++ `settingsCatalog` bar + idle extensions).

use serde_json::{json, Value};

use super::ordered_catalog::bar_names;
use super::settings::{ConfigStore, SETTING_UNAVAILABLE};

const IDLE_FIELDS: &[(&str, &str, &str, &str)] = &[
    ("enabled", "Enable timer", "boolean", ""),
    ("timeout", "Idle delay (seconds)", "number", "Zero disables this timer while unlocked."),
    (
        "locked_timeout",
        "Delay while locked (seconds)",
        "number",
        "Zero uses the normal idle delay.",
    ),
    ("action", "Action", "idle-action", ""),
    ("lock_before_suspend", "Lock before suspending", "boolean", ""),
];

const IDLE_ACTIONS: &[(&str, &str)] = &[
    ("lock", "Lock session"),
    ("screen_off", "Turn off displays"),
    ("suspend", "Suspend"),
    ("lock_and_suspend", "Lock and suspend"),
];

const BAR_SCALAR_FIELDS: &[(&str, &str, &str, &str, f64, f64)] = &[
    ("enabled", "Show bar", "", "boolean", 0.0, 0.0),
    ("position", "Position", "", "position", 0.0, 0.0),
    ("layer", "Layer", "", "shell-layer", 0.0, 0.0),
    ("auto_hide", "Automatically hide", "", "boolean", 0.0, 0.0),
    ("smart_auto_hide", "Hide behind windows", "", "boolean", 0.0, 0.0),
    ("reserve_space", "Reserve space for the bar", "", "boolean", 0.0, 0.0),
    (
        "show_on_workspace_switch",
        "Reveal when switching workspaces",
        "",
        "boolean",
        0.0,
        0.0,
    ),
    ("shadow", "Show shadow", "", "boolean", 0.0, 0.0),
    ("contact_shadow", "Contact shadow", "", "boolean", 0.0, 0.0),
    ("thickness", "Thickness", " · Size", "integer", 10.0, 300.0),
    ("scale", "Content scale", " · Size", "number", 0.5, 4.0),
    ("font_scale", "Font scale", " · Size", "number", 0.2, 2.5),
    ("padding", "End padding", " · Size", "integer", 0.0, 200.0),
    ("widget_spacing", "Widget spacing", " · Size", "integer", 0.0, 100.0),
    ("panel_overlap", "Panel overlap", " · Layout", "integer", -2.0, 3.0),
    ("margin_ends", "Inset from display ends", " · Layout", "integer", 0.0, 500.0),
    (
        "margin_edge",
        "Distance from display edge",
        " · Layout",
        "integer",
        0.0,
        100.0,
    ),
    (
        "margin_opposite_edge",
        "Inward reserved space",
        " · Layout",
        "integer",
        0.0,
        100.0,
    ),
    (
        "background_opacity",
        "Background opacity",
        " · Effects",
        "number",
        0.0,
        1.0,
    ),
    ("radius", "Corner radius", " · Shape", "integer", 0.0, 500.0),
    ("border_width", "Border width", " · Shape", "number", 0.0, 20.0),
    (
        "concave_edge_corners",
        "Concave edge corners",
        " · Shape",
        "boolean",
        0.0,
        0.0,
    ),
    ("font_weight", "Font weight", " · Typography", "integer", 100.0, 900.0),
    ("font_family", "Font family", " · Typography", "string", 0.0, 0.0),
];

/// Behaviors a pristine C++ config defines, in its order.
const DEFAULT_IDLE_BEHAVIORS: [&str; 3] = ["lock", "screen-off", "lock-and-suspend"];

fn idle_behavior_names(store: &ConfigStore) -> Vec<String> {
    // The built-in behaviors always exist; configured entries override them by name and
    // custom names follow in key order.
    let mut names: Vec<String> = DEFAULT_IDLE_BEHAVIORS.iter().map(|n| (*n).to_string()).collect();
    if let Some(table) = store
        .merged_node("idle.behavior")
        .and_then(|node| node.as_table().cloned())
    {
        let mut extra: Vec<String> = table
            .keys()
            .filter(|name| !DEFAULT_IDLE_BEHAVIORS.contains(&name.as_str()))
            .cloned()
            .collect();
        extra.sort();
        names.extend(extra);
    }
    names
}

fn idle_group_overridden(store: &ConfigStore) -> bool {
    store.overridden_path("idle.behavior")
}

pub fn idle_catalog_entries(store: &ConfigStore) -> Vec<Value> {
    let group_overridden = idle_group_overridden(store);
    let mut out = Vec::new();
    for name in idle_behavior_names(store) {
        let behavior = store
            .get_json_value(&format!("idle:{name}:action"))
            .and_then(|v| v.as_str().map(|s| s.to_string()))
            .unwrap_or_else(|| default_idle_value(&name, "action").as_str().unwrap_or("").to_string());
        for (field, title, typ, description) in IDLE_FIELDS {
            if *field == "lock_before_suspend" && behavior != "suspend" {
                continue;
            }
            let id = format!("idle:{name}:{field}");
            let value = store
                .get_json_value(&id)
                .unwrap_or_else(|| default_idle_value(&name, field));
            let mut entry = json!({
                "id": id,
                "page": "power",
                "title": title,
                "group": format!("Idle · {name}"),
                "type": typ,
                "value": value,
                "minimum": if *field == "timeout" || *field == "locked_timeout" { 0 } else { 0 },
                "maximum": if *field == "timeout" || *field == "locked_timeout" { 86400 } else { 0 },
                "overridden": store.overridden_path(&id),
                "group-overridden": group_overridden,
            });
            if !description.is_empty() {
                entry["description"] = Value::String(description.to_string());
            }
            if *field == "action" {
                let mut options = Vec::new();
                let mut labels = Vec::new();
                for (opt, label) in IDLE_ACTIONS {
                    options.push(Value::String(opt.to_string()));
                    labels.push(Value::String(label.to_string()));
                }
                let action_is_command = store
                    .get_json_value(&format!("idle:{name}:action"))
                    .and_then(|v| v.as_str().map(|s| s == "command"))
                    .unwrap_or(false);
                if action_is_command
                    || store
                        .get_json_value(&format!("idle:{name}:command"))
                        .is_some()
                {
                    if !options.iter().any(|o| o.as_str() == Some("command")) {
                        options.push(Value::String("command".into()));
                        labels.push(Value::String("Existing custom command".into()));
                    }
                }
                entry["options"] = Value::Array(options);
                entry["option-labels"] = Value::Array(labels);
            }
            out.push(entry);
        }
    }
    out
}

fn default_idle_value(name: &str, field: &str) -> Value {
    // C++ defaults of the three built-in behaviors; unknown behaviors start disabled.
    let (timeout, action) = match name {
        "lock" => (600.0, "lock"),
        "screen-off" => (660.0, "screen_off"),
        "lock-and-suspend" => (900.0, "lock_and_suspend"),
        _ => (0.0, "lock"),
    };
    match field {
        "enabled" => Value::Bool(false),
        "timeout" => Value::from(timeout),
        "locked_timeout" => Value::from(0.0),
        "action" => Value::String(action.into()),
        "lock_before_suspend" => Value::Bool(true),
        _ => Value::Null,
    }
}

/// `BarConfig` defaults of the C++ config (also what a bar missing a key reports).
fn bar_field_value(store: &ConfigStore, bar_name: &str, field: &str) -> Value {
    let id = format!("bar:{bar_name}:{field}");
    store.get_json_value(&id).unwrap_or_else(|| match field {
        "enabled" | "reserve_space" | "show_on_workspace_switch" | "shadow" => Value::Bool(true),
        "position" | "layer" => Value::String("top".into()),
        "font_family" => Value::String(String::new()),
        "thickness" => Value::from(34.0),
        "scale" | "font_scale" => Value::from(1.0),
        "padding" | "margin_ends" => Value::from(12.0),
        "widget_spacing" | "margin_edge" => Value::from(8.0),
        "panel_overlap" => Value::from(1.0),
        "margin_opposite_edge" | "border_width" => Value::from(0.0),
        "background_opacity" => Value::from(0.72),
        "radius" => Value::from(18.0),
        "font_weight" => Value::from(500.0),
        _ => Value::Bool(false),
    })
}

pub fn bar_scalar_entries(store: &ConfigStore) -> Vec<Value> {
    let mut out = Vec::new();
    for bar_name in bar_names(store) {
        for (field, title, suffix, typ, minimum, maximum) in BAR_SCALAR_FIELDS {
            let id = format!("bar:{bar_name}:{field}");
            let group = if suffix.is_empty() {
                format!("Bar · {bar_name}")
            } else {
                format!("Bar · {bar_name}{suffix}")
            };
            let field_value = {
                let value = bar_field_value(store, &bar_name, field);
                if *typ == "integer" {
                    super::settings::as_integer(value)
                } else {
                    value
                }
            };
            let options: Option<(&[&str], &[&str])> = match *typ {
                "position" => Some((&["top", "bottom", "left", "right"], &["Top", "Bottom", "Left", "Right"])),
                "shell-layer" => Some((&["top", "overlay"], &["Normal", "Above fullscreen windows"])),
                _ => None,
            };
            let mut row = json!({
                "id": id,
                "page": "desktop-dock",
                "title": title,
                "group": group,
                "type": typ,
                "value": field_value,
                "minimum": minimum,
                "maximum": maximum,
                "overridden": store.overridden_path(&id),
            });
            if let Some((values, labels)) = options {
                row["options"] = json!(values);
                row["option-labels"] = json!(labels);
            }
            out.push(row);
        }
    }
    out
}

pub fn is_bar_field(field: &str) -> bool {
    BAR_SCALAR_FIELDS.iter().any(|(id, ..)| *id == field)
}

pub fn is_idle_field(field: &str) -> bool {
    matches!(field, "enabled" | "timeout" | "locked_timeout" | "action" | "lock_before_suspend")
}

pub fn validate_bar_scalar(field: &str, value: Value) -> Result<Value, String> {
    let meta = BAR_SCALAR_FIELDS
        .iter()
        .find(|(id, ..)| *id == field)
        .ok_or_else(|| SETTING_UNAVAILABLE.to_string())?;
    let (_, _, _, typ, minimum, maximum) = meta;
    match *typ {
        "boolean" => value
            .as_bool()
            .map(Value::Bool)
            .ok_or_else(|| "This setting requires a boolean".into()),
        "integer" => {
            let number = value.as_f64().ok_or("This setting requires a number")?;
            if number.fract() != 0.0 {
                return Err("This setting requires a number".into());
            }
            let rounded = number.round();
            if rounded < *minimum || rounded > *maximum {
                return Err("Setting is outside its supported range".into());
            }
            Ok(Value::from(rounded as i64))
        }
        "number" => {
            let number = value.as_f64().ok_or("This setting requires a number")?;
            if number < *minimum || number > *maximum {
                return Err("Setting is outside its supported range".into());
            }
            Ok(Value::from(number))
        }
        "string" => value
            .as_str()
            .map(|text| Value::String(text.to_string()))
            .ok_or_else(|| "This setting requires text".into()),
        "position" | "shell-layer" => {
            let text = value.as_str().ok_or("This setting requires text")?;
            let allowed: &[&str] =
                if *typ == "position" { &["top", "bottom", "left", "right"] } else { &["top", "overlay"] };
            if allowed.contains(&text) {
                Ok(Value::String(text.to_string()))
            } else {
                Err("Unknown setting option".into())
            }
        }
        _ => Err(SETTING_UNAVAILABLE.into()),
    }
}

pub fn validate_idle_field(field: &str, value: Value) -> Result<Value, String> {
    match field {
        "enabled" | "lock_before_suspend" => value
            .as_bool()
            .map(Value::Bool)
            .ok_or_else(|| "This setting requires a boolean".into()),
        "timeout" | "locked_timeout" => {
            let number = value.as_f64().ok_or("This setting requires a number")?;
            if number < 0.0 || number > 86400.0 {
                return Err("Setting is outside its supported range".into());
            }
            Ok(Value::from(number))
        }
        "action" => {
            let text = value.as_str().ok_or("This setting requires text")?;
            if !IDLE_ACTIONS.iter().any(|(id, _)| *id == text) && text != "command" {
                return Err("Unknown setting option".into());
            }
            Ok(Value::String(text.to_string()))
        }
        _ => Err(SETTING_UNAVAILABLE.into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::desktop::settings::{ConfigPaths, ConfigStore};
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn store_with_bar() -> ConfigStore {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let root = std::env::temp_dir().join(format!("helix-dynamic-catalog-{nanos}"));
        let config_dir = root.join("config");
        let state_dir = root.join("state");
        fs::create_dir_all(&config_dir).unwrap();
        fs::write(
            config_dir.join("config.toml"),
            "[bar.main]\nenabled = true\nthickness = 40\n[idle.behavior.lock]\ntimeout = 420\nenabled = true\n",
        )
        .unwrap();
        ConfigStore::load(ConfigPaths {
            config_dir,
            settings_toml: state_dir.join("settings.toml"),
        })
    }

    #[test]
    fn includes_bar_and_idle_rows_when_desktop() {
        let store = store_with_bar();
        let rows = desktop_extension_entries(&store, true);
        assert!(rows.iter().any(|row| row["id"] == "bar:main:enabled"));
        assert!(rows.iter().any(|row| row["id"] == "idle:lock:timeout"));
        assert_eq!(store.get_json_value("idle:lock:timeout").and_then(|v| v.as_f64()), Some(420.0));
    }
}

pub fn desktop_extension_entries(store: &ConfigStore, include_desktop: bool) -> Vec<Value> {
    if !include_desktop {
        return Vec::new();
    }
    let mut out = Vec::new();
    out.extend(bar_scalar_entries(store));
    out.extend(idle_catalog_entries(store));
    out
}
