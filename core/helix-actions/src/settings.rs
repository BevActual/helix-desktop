//! Settings as actions: find a setting, read it, change it. Every setting in the Settings catalogue
//! (`helix_config` `full_catalog`, the rows `org.helix.Desktop.Configuration1.GetSettings` returns) becomes reachable without a
//! hand-written tool per setting, the way App Intents expose entities through a query: a model finds the setting it means, then sets it.
//!
//! The functions here work on those catalogue rows (JSON objects with `id`, `page`, `title`, `group`, `type`, `value`, `minimum`,
//! `maximum`, and sometimes `options` / `option-labels`). The settings store still validates every value it is given; this module
//! only refuses what is plainly wrong before it gets there, and decides how risky a change is.

use serde_json::{json, Value};

use crate::{Action, Risk, Undo};

pub const FIND: &str = "settings.find";
pub const GET: &str = "settings.get";
pub const SET: &str = "settings.set";

/// The three Settings actions.
pub fn actions() -> Vec<Action> {
    vec![
        Action {
            id: FIND.into(),
            title: "Find a setting".into(),
            summary: "Search Helix's settings by what they do (\"dark mode\", \"dock size\", \"night light\"). Returns each match's id, \
                      title, page, current value and the values it accepts. Use before settings.set."
                .into(),
            params: json!({
                "type": "object",
                "properties": {
                    "query": { "type": "string", "maxLength": 120, "description": "Words describing the setting." },
                    "page": { "type": "string", "maxLength": 40, "description": "Only settings on this Settings page (optional)." }
                },
                "required": ["query"],
                "additionalProperties": false
            }),
            risk: Risk::Observe,
            undo: Undo::None,
            selection: None,
        },
        Action {
            id: GET.into(),
            title: "Read a setting".into(),
            summary: "The current value of one setting, by the id settings.find returned.".into(),
            params: json!({
                "type": "object",
                "properties": { "setting": { "type": "string", "maxLength": 120 } },
                "required": ["setting"],
                "additionalProperties": false
            }),
            risk: Risk::Observe,
            undo: Undo::None,
            selection: None,
        },
        Action {
            id: SET.into(),
            title: "Change a setting".into(),
            summary: "Change one setting to a new value, by the id settings.find returned. The value must be one it accepts. The change \
                      is recorded and can be undone."
                .into(),
            params: json!({
                "type": "object",
                "properties": {
                    "setting": { "type": "string", "maxLength": 120 },
                    "value": { "description": "The new value: true/false, a number, or one of the accepted words." }
                },
                "required": ["setting", "value"],
                "additionalProperties": false
            }),
            risk: Risk::Act,
            undo: Undo::Restore,
            selection: None,
        },
    ]
}

/// How risky it is for an agent to change this setting.
pub fn risk_to_change(id: &str) -> Risk {
    let blocked_prefix = ["hooks.", "storage."];
    // A setting that names a program to run, or lets the computer open without a password, is never an agent's to change.
    if blocked_prefix.iter().any(|prefix| id.starts_with(prefix))
        || id.contains("command")
        || id == "lockscreen.allow_empty_password"
    {
        return Risk::Blocked;
    }
    let confirm_prefix = ["lockscreen.", "idle.", "location.", "ai.", "tailscale."];
    if confirm_prefix.iter().any(|prefix| id.starts_with(prefix)) || id == "shell.avatar_path" {
        return Risk::Confirm;
    }
    Risk::Act
}

/// How private reading this setting is.
pub fn risk_to_read(id: &str) -> Risk {
    if id.starts_with("location.") {
        Risk::ObservePrivate
    } else {
        Risk::Observe
    }
}

/// The JSON Schema of the values one setting accepts.
pub fn value_schema(row: &Value) -> Value {
    let kind = row.get("type").and_then(Value::as_str).unwrap_or("string");
    let options = options_of(row);
    if !options.is_empty() {
        return json!({ "enum": options });
    }
    match kind {
        "boolean" => json!({ "type": "boolean" }),
        "number" | "integer" => {
            let mut schema = json!({ "type": kind });
            if let Some(min) = row.get("minimum").filter(|v| v.is_number()) {
                schema["minimum"] = min.clone();
            }
            if let Some(max) = row.get("maximum").filter(|v| v.is_number()) {
                schema["maximum"] = max.clone();
            }
            schema
        }
        "application-list" => json!({ "type": "array", "items": { "type": "string" } }),
        _ => json!({ "type": "string", "maxLength": 400 }),
    }
}

fn options_of(row: &Value) -> Vec<Value> {
    row.get("options")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default()
}

/// What a model or a person sees of one setting. The value is left out when reading it is private, so a search never leaks it.
pub fn summary(row: &Value) -> Value {
    let id = row.get("id").and_then(Value::as_str).unwrap_or_default();
    let mut out = json!({
        "setting": id,
        "title": row.get("title").cloned().unwrap_or(Value::Null),
        "page": row.get("page").cloned().unwrap_or(Value::Null),
        "group": row.get("group").cloned().unwrap_or(Value::Null),
        "accepts": value_schema(row),
        "change": risk_to_change(id).name(),
    });
    if risk_to_read(id) == Risk::Observe {
        out["value"] = row.get("value").cloned().unwrap_or(Value::Null);
    }
    out
}

/// The settings that match `query` best, most relevant first. Every word must appear in the id, title, page or group; the title
/// counts most. Settings an agent may never change are still found (so it can say why it cannot), marked `"change": "blocked"`.
pub fn search(rows: &[Value], query: &str, page: Option<&str>, limit: usize) -> Vec<Value> {
    let words: Vec<String> = query
        .split(|c: char| !c.is_alphanumeric())
        .filter(|w| !w.is_empty())
        .map(str::to_lowercase)
        .collect();
    let page = page.map(str::to_lowercase).filter(|p| !p.is_empty());
    let mut scored: Vec<(i32, &Value)> = rows
        .iter()
        .filter_map(|row| {
            let field = |key: &str| {
                row.get(key)
                    .and_then(Value::as_str)
                    .unwrap_or_default()
                    .to_lowercase()
            };
            let (id, title, row_page, group) =
                (field("id"), field("title"), field("page"), field("group"));
            if let Some(wanted) = &page {
                if &row_page != wanted {
                    return None;
                }
            }
            let mut score = 0;
            for word in &words {
                let mut hit = 0;
                if title
                    .split(|c: char| !c.is_alphanumeric())
                    .any(|w| w == word)
                {
                    hit = 6;
                } else if title.contains(word.as_str()) {
                    hit = 4;
                } else if group.contains(word.as_str()) || row_page.contains(word.as_str()) {
                    hit = 2;
                } else if id.contains(word.as_str()) {
                    hit = 1;
                }
                if hit == 0 {
                    return None;
                }
                score += hit;
            }
            Some((score, row))
        })
        .collect();
    scored.sort_by(|a, b| b.0.cmp(&a.0));
    scored
        .into_iter()
        .take(limit)
        .map(|(_, row)| summary(row))
        .collect()
}

/// The value as the setting will take it, or why not. Numbers are kept in range; words must be among the options when it lists them.
pub fn validate(row: &Value, value: &Value) -> Result<Value, String> {
    let title = row
        .get("title")
        .and_then(Value::as_str)
        .unwrap_or("This setting");
    let kind = row.get("type").and_then(Value::as_str).unwrap_or("string");
    let options = options_of(row);
    if !options.is_empty() {
        return if options.contains(value) {
            Ok(value.clone())
        } else {
            Err(format!("{title} accepts {}.", list(&options)))
        };
    }
    match kind {
        "boolean" => match value {
            Value::Bool(_) => Ok(value.clone()),
            Value::String(text) if text == "true" || text == "false" => {
                Ok(Value::Bool(text == "true"))
            }
            _ => Err(format!("{title} is on or off: true or false.")),
        },
        "number" | "integer" => {
            let number = match value {
                Value::Number(n) => n.as_f64(),
                Value::String(text) => text.trim().parse::<f64>().ok(),
                _ => None,
            }
            .ok_or_else(|| format!("{title} is a number."))?;
            let min = row.get("minimum").and_then(Value::as_f64);
            let max = row.get("maximum").and_then(Value::as_f64);
            if min.is_some_and(|m| number < m) || max.is_some_and(|m| number > m) {
                return Err(format!(
                    "{title} goes from {} to {}.",
                    min.map_or("any".into(), shown_number),
                    max.map_or("any".into(), shown_number)
                ));
            }
            Ok(if kind == "integer" {
                json!(number.round() as i64)
            } else {
                json!(number)
            })
        }
        "application-list" => Err(format!(
            "{title} is a list; change it on its Settings page."
        )),
        _ => match value {
            Value::String(text) if text.chars().count() <= 400 => Ok(value.clone()),
            Value::String(_) => Err(format!("{title} is too long.")),
            _ => Err(format!("{title} is text.")),
        },
    }
}

fn shown_number(n: f64) -> String {
    if n.fract() == 0.0 {
        format!("{n:.0}")
    } else {
        format!("{n}")
    }
}

fn list(options: &[Value]) -> String {
    let words: Vec<String> = options
        .iter()
        .map(|v| {
            v.as_str()
                .map_or_else(|| v.to_string(), |s| format!("\"{s}\""))
        })
        .collect();
    words.join(", ")
}

/// A value the way a person reads it: On/Off, the option's label, a plain number.
pub fn shown(row: &Value, value: &Value) -> String {
    let options = options_of(row);
    if let Some(at) = options.iter().position(|o| o == value) {
        if let Some(label) = row
            .get("option-labels")
            .and_then(Value::as_array)
            .and_then(|labels| labels.get(at))
            .and_then(Value::as_str)
            .filter(|l| !l.is_empty())
        {
            return label.to_string();
        }
    }
    match value {
        Value::Bool(true) => "On".into(),
        Value::Bool(false) => "Off".into(),
        Value::Number(n) => n.as_f64().map_or_else(|| n.to_string(), shown_number),
        Value::String(s) if s.is_empty() => "none".into(),
        Value::String(s) => s.clone(),
        Value::Null => "none".into(),
        other => other.to_string(),
    }
}

/// The sentence the history shows for a change: "Dark theme: Off → On".
pub fn sentence(row: &Value, before: &Value, after: &Value) -> String {
    let title = row
        .get("title")
        .and_then(Value::as_str)
        .unwrap_or("A setting");
    format!("{title}: {} → {}", shown(row, before), shown(row, after))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rows() -> Vec<Value> {
        vec![
            json!({"id": "theme.mode", "page": "appearance", "title": "Appearance", "group": "Colors", "type": "mode",
                   "value": "dark", "options": ["dark", "light", "auto"], "option-labels": ["Dark", "Light", "Automatic"]}),
            json!({"id": "dock.icon_size", "page": "dock", "title": "Icon size", "group": "Dock", "type": "integer",
                   "value": 52, "minimum": 32, "maximum": 96}),
            json!({"id": "nightlight.enabled", "page": "displays", "title": "Night Light", "group": "Night Light", "type": "boolean",
                   "value": false}),
            json!({"id": "location.latitude", "page": "datetime", "title": "Latitude", "group": "Location", "type": "number",
                   "value": 51.5}),
            json!({"id": "hooks.started", "page": "advanced", "title": "When the session starts", "group": "Hooks", "type": "string",
                   "value": ""}),
        ]
    }

    #[test]
    fn finding_a_setting_by_what_it_does() {
        let found = search(&rows(), "night light", None, 5);
        assert_eq!(found[0]["setting"], "nightlight.enabled");
        assert_eq!(found[0]["accepts"], json!({"type": "boolean"}));
        let found = search(&rows(), "icon size", Some("dock"), 5);
        assert_eq!(found.len(), 1);
        assert_eq!(found[0]["accepts"]["maximum"], 96);
        assert!(search(&rows(), "icon size", Some("appearance"), 5).is_empty());
        // Every word has to match somewhere.
        assert!(search(&rows(), "night banana", None, 5).is_empty());
    }

    #[test]
    fn a_private_value_is_not_shown_in_a_search() {
        let found = search(&rows(), "latitude", None, 5);
        assert_eq!(found[0]["setting"], "location.latitude");
        assert!(found[0].get("value").is_none());
        assert_eq!(found[0]["change"], "confirm");
    }

    #[test]
    fn values_are_checked_against_what_the_setting_accepts() {
        let r = rows();
        assert_eq!(validate(&r[0], &json!("light")), Ok(json!("light")));
        assert!(validate(&r[0], &json!("purple"))
            .unwrap_err()
            .contains("\"dark\""));
        assert_eq!(validate(&r[1], &json!(60.4)), Ok(json!(60)));
        assert_eq!(validate(&r[1], &json!("64")), Ok(json!(64)));
        assert_eq!(
            validate(&r[1], &json!(200)),
            Err("Icon size goes from 32 to 96.".into())
        );
        assert_eq!(validate(&r[2], &json!(true)), Ok(json!(true)));
        assert_eq!(validate(&r[2], &json!("true")), Ok(json!(true)));
        assert!(validate(&r[2], &json!(1)).is_err());
    }

    #[test]
    fn what_an_agent_may_never_change() {
        assert_eq!(risk_to_change("hooks.started"), Risk::Blocked);
        assert_eq!(
            risk_to_change("shell.screenshot.pipe_command"),
            Risk::Blocked
        );
        assert_eq!(
            risk_to_change("hot_corners.top_left.command"),
            Risk::Blocked
        );
        assert_eq!(
            risk_to_change("lockscreen.allow_empty_password"),
            Risk::Blocked
        );
        assert_eq!(risk_to_change("storage.key_file"), Risk::Blocked);
        assert_eq!(risk_to_change("lockscreen.enabled"), Risk::Confirm);
        assert_eq!(risk_to_change("ai.chat_model"), Risk::Confirm);
        assert_eq!(risk_to_change("theme.mode"), Risk::Act);
        assert_eq!(risk_to_change("dock.icon_size"), Risk::Act);
    }

    #[test]
    fn a_change_reads_as_a_sentence() {
        let r = rows();
        assert_eq!(
            sentence(&r[0], &json!("dark"), &json!("light")),
            "Appearance: Dark → Light"
        );
        assert_eq!(
            sentence(&r[2], &json!(false), &json!(true)),
            "Night Light: Off → On"
        );
        assert_eq!(
            sentence(&r[1], &json!(52), &json!(64)),
            "Icon size: 52 → 64"
        );
    }
}
