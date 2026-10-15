//! Application notification rules (`notification-rules` / `notification.filter`).

use serde_json::{json, Value};

use super::settings::ConfigStore;

const URGENCIES: &[&str] = &["low", "normal", "critical"];

/// Rules as the catalog reports them. Config stores them as named tables
/// (`[notification.filter.<name>]`, the C++ schema); an array of rule objects is accepted too.
pub fn read_rules(store: &ConfigStore) -> Value {
    match store.get_json_value("notification.filter") {
        Some(Value::Array(rules)) => Value::Array(rules),
        Some(Value::Object(named)) => {
            // `filter_order` (written with the rules) wins; anything unlisted follows sorted.
            let order: Vec<String> = store
                .get_json_value("notification.filter_order")
                .and_then(|v| v.as_array().cloned())
                .map(|items| items.iter().filter_map(|i| i.as_str().map(str::to_string)).collect())
                .unwrap_or_default();
            let mut names: Vec<String> = Vec::new();
            for name in order {
                if named.contains_key(&name) && !names.contains(&name) {
                    names.push(name);
                }
            }
            let mut rest: Vec<&String> = named.keys().filter(|n| !names.contains(*n)).collect();
            rest.sort();
            names.extend(rest.into_iter().cloned());
            let rules = names
                .iter()
                .filter_map(|name| Some((name.as_str(), named.get(name)?.as_object()?)))
                .map(|(name, rule)| rule_from_table(name, rule))
                .collect();
            Value::Array(rules)
        }
        _ => Value::Array(Vec::new()),
    }
}

fn rule_from_table(name: &str, rule: &serde_json::Map<String, Value>) -> Value {
    let flag = |key: &str, default: bool| rule.get(key).and_then(Value::as_bool).unwrap_or(default);
    let text = |key: &str| rule.get(key).and_then(Value::as_str).unwrap_or("").to_string();
    // Legacy `matches = [..]` fills `match` when it is empty.
    let mut match_app = text("match");
    if match_app.is_empty() {
        if let Some(first) = rule
            .get("matches")
            .and_then(Value::as_array)
            .and_then(|items| items.iter().filter_map(Value::as_str).find(|t| !t.trim().is_empty()))
        {
            match_app = first.trim().to_lowercase();
        }
    }
    let urgencies: Vec<Value> = rule
        .get("allowed_urgencies")
        .and_then(Value::as_array)
        .map(|items| items.iter().filter(|u| u.is_string()).cloned().collect())
        .unwrap_or_default();
    let mut out = json!({
        "name": name,
        "enabled": flag("enabled", true),
        "match": match_app,
        "match_content": text("match_content"),
        "show_toast": flag("show_toast", true),
        "save_history": flag("save_history", true),
        "play_sound": flag("play_sound", true),
        "bypass_dnd": flag("bypass_dnd", false),
        "allow_permanent": flag("allow_permanent", true),
        "allowed_urgencies": urgencies,
    });
    if let Some(duration) = rule.get("override_duration").and_then(Value::as_i64) {
        out["override_duration"] = json!(duration);
    }
    out
}

/// The named-table form persisted for validated rules (`name` becomes the table key), in the
/// C++ override layout: optional fields only when set.
pub fn to_named_tables(rules: &Value) -> Value {
    let mut named = serde_json::Map::new();
    for rule in rules.as_array().into_iter().flatten() {
        let Some(object) = rule.as_object() else { continue };
        let Some(name) = object.get("name").and_then(Value::as_str) else { continue };
        let mut row = serde_json::Map::new();
        row.insert("enabled".into(), object.get("enabled").cloned().unwrap_or(Value::Bool(true)));
        for key in ["match", "match_content"] {
            if let Some(text) = object.get(key).and_then(Value::as_str).filter(|t| !t.is_empty()) {
                row.insert(key.into(), Value::String(text.to_string()));
            }
        }
        for key in ["show_toast", "save_history", "play_sound", "bypass_dnd", "allow_permanent"] {
            if let Some(flag) = object.get(key) {
                row.insert(key.into(), flag.clone());
            }
        }
        if let Some(duration) = object.get("override_duration") {
            row.insert("override_duration".into(), duration.clone());
        }
        if let Some(urgencies) = object
            .get("allowed_urgencies")
            .and_then(Value::as_array)
            .filter(|u| !u.is_empty())
        {
            row.insert("allowed_urgencies".into(), Value::Array(urgencies.clone()));
        }
        named.insert(name.to_string(), Value::Object(row));
    }
    Value::Object(named)
}

/// Rule names in order, for `notification.filter_order`.
pub fn rule_names(rules: &Value) -> Value {
    Value::Array(
        rules
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(|rule| rule.get("name").and_then(Value::as_str))
            .map(|name| Value::String(name.to_string()))
            .collect(),
    )
}

pub fn catalog_entry(store: &ConfigStore) -> Value {
    json!({
        "id": "notification.filter",
        "page": "notifications",
        "title": "Application Rules",
        "group": "Application Rules",
        "type": "notification-rules",
        "value": read_rules(store),
        "options": [],
        "minimum": 0,
        "maximum": 256,
        "overridden": store.overridden_path("notification.filter"),
    })
}

fn read_text(object: &serde_json::Map<String, Value>, key: &str, limit: usize) -> Result<String, String> {
    let text = object
        .get(key)
        .and_then(Value::as_str)
        .ok_or("Notification rule text is invalid")?;
    if text.len() > limit || text.chars().any(|c| c.is_control()) {
        return Err("Notification rule text is invalid".into());
    }
    Ok(text.to_string())
}

fn read_bool(object: &serde_json::Map<String, Value>, key: &str) -> Result<bool, String> {
    object
        .get(key)
        .and_then(Value::as_bool)
        .ok_or_else(|| "Notification rule flags must be boolean".into())
}

pub fn validate(value: &Value) -> Result<Value, String> {
    let items = value
        .as_array()
        .ok_or("Expected at most 256 notification rules")?;
    if items.len() > 256 {
        return Err("Expected at most 256 notification rules".into());
    }
    let mut names = std::collections::BTreeSet::new();
    let mut out = Vec::new();
    for item in items {
        let object = item
            .as_object()
            .ok_or("Notification rules must be objects")?;
        let name = read_text(object, "name", 128)?;
        let match_app = read_text(object, "match", 512)?;
        let match_content = read_text(object, "match_content", 1024)?;
        if name.is_empty() || !names.insert(name.clone()) {
            return Err("Rule names must be unique and nonempty".into());
        }
        let enabled = read_bool(object, "enabled")?;
        let show_toast = read_bool(object, "show_toast")?;
        let save_history = read_bool(object, "save_history")?;
        let play_sound = read_bool(object, "play_sound")?;
        let bypass_dnd = read_bool(object, "bypass_dnd")?;
        let allow_permanent = read_bool(object, "allow_permanent")?;
        if enabled && match_app.is_empty() && match_content.is_empty() {
            return Err("Enabled rules need an application match or content expression".into());
        }
        if !match_content.is_empty()
            && regex::RegexBuilder::new(&match_content)
                .case_insensitive(true)
                .build()
                .is_err()
        {
            return Err("The content expression is invalid".into());
        }
        let urgencies_value = object
            .get("allowed_urgencies")
            .and_then(Value::as_array)
            .ok_or("Invalid notification urgencies")?;
        let mut urgencies = std::collections::BTreeSet::new();
        let mut allowed = Vec::new();
        for urgency in urgencies_value {
            let text = urgency.as_str().ok_or("Invalid notification urgency")?;
            if !URGENCIES.contains(&text) || !urgencies.insert(text.to_string()) {
                return Err("Invalid notification urgency".into());
            }
            allowed.push(Value::String(text.to_string()));
        }
        let mut rule = json!({
            "name": name,
            "enabled": enabled,
            "match": match_app,
            "match_content": match_content,
            "show_toast": show_toast,
            "save_history": save_history,
            "play_sound": play_sound,
            "bypass_dnd": bypass_dnd,
            "allow_permanent": allow_permanent,
            "allowed_urgencies": allowed,
        });
        if let Some(duration) = object.get("override_duration") {
            // D-Bus integers reach us as JSON floats (`1234.0`); only whole numbers are valid.
            let number = duration
                .as_i64()
                .or_else(|| {
                    duration
                        .as_f64()
                        .filter(|n| n.fract() == 0.0 && n.abs() < 9.0e15)
                        .map(|n| n as i64)
                })
                .ok_or("Invalid notification duration")?;
            if !(-1..=2_147_483_647).contains(&number) {
                return Err("Invalid notification duration".into());
            }
            rule["override_duration"] = json!(number);
        }
        out.push(rule);
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
        let root = std::env::temp_dir().join(format!("helix-notify-rules-{nanos}"));
        let paths = ConfigPaths {
            config_dir: root.join("config"),
            settings_toml: root.join("state/settings.toml"),
        };
        std::fs::create_dir_all(&paths.config_dir).unwrap();
        std::fs::create_dir_all(paths.settings_toml.parent().unwrap()).unwrap();
        ConfigStore::load(paths)
    }

    #[test]
    fn notification_rule_round_trip() {
        let value = json!([{
            "name": "Test",
            "enabled": true,
            "match": "firefox",
            "match_content": "",
            "show_toast": true,
            "save_history": true,
            "play_sound": false,
            "bypass_dnd": false,
            "allow_permanent": false,
            "allowed_urgencies": ["normal"],
        }]);
        let validated = validate(&value).unwrap();
        let store = temp_store();
        let mut store = store;
        store.set_json_value("notification.filter", validated).unwrap();
        let rules = read_rules(&store);
        assert_eq!(rules.as_array().unwrap().len(), 1);
    }
}
