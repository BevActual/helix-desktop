//! Application notification rules (`notification-rules` / `notification.filter`).

use serde_json::{json, Value};

use super::settings::ConfigStore;

const URGENCIES: &[&str] = &["low", "normal", "critical"];

pub fn read_rules(store: &ConfigStore) -> Value {
    store
        .get_json_value("notification.filter")
        .and_then(|value| value.as_array().cloned())
        .map(Value::Array)
        .unwrap_or_else(|| Value::Array(Vec::new()))
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
            let number = duration
                .as_i64()
                .ok_or("Invalid notification duration")?;
            if number < -1 || number > 2_147_483_647 {
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
    use crate::desktop::settings::{ConfigPaths, ConfigStore};
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
