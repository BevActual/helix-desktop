use std::collections::HashMap;

use zbus::zvariant::{OwnedValue, Value};

pub fn owned_str(text: &str) -> OwnedValue {
    OwnedValue::try_from(Value::from(text)).expect("str")
}

pub fn owned_bool(value: bool) -> OwnedValue {
    OwnedValue::try_from(Value::from(value)).expect("bool")
}

pub fn owned_f64(value: f64) -> OwnedValue {
    OwnedValue::try_from(Value::from(value)).expect("f64")
}

pub fn owned_u64(value: u64) -> OwnedValue {
    OwnedValue::try_from(Value::from(value)).expect("u64")
}

pub fn dict_entry(fields: HashMap<&str, OwnedValue>) -> OwnedValue {
    let map: HashMap<String, OwnedValue> = fields
        .into_iter()
        .map(|(key, value)| (key.to_string(), value))
        .collect();
    OwnedValue::try_from(Value::from(map)).expect("dict")
}

pub fn owned_to_json(value: &OwnedValue) -> Option<serde_json::Value> {
    value_to_json(&**value)
}

fn normalize_dict_key(key: &str) -> String {
    let trimmed = key.trim();
    if trimmed.len() >= 2 && trimmed.starts_with('"') && trimmed.ends_with('"') {
        trimmed[1..trimmed.len() - 1].to_string()
    } else {
        key.to_string()
    }
}

fn value_to_json(value: &Value<'_>) -> Option<serde_json::Value> {
    match value {
        Value::Bool(flag) => Some(serde_json::Value::Bool(*flag)),
        Value::U8(number) => Some(serde_json::json!(f64::from(*number))),
        Value::U16(number) => Some(serde_json::json!(f64::from(*number))),
        Value::U32(number) => Some(serde_json::json!(f64::from(*number))),
        Value::U64(number) => Some(serde_json::json!(*number as f64)),
        Value::I16(number) => Some(serde_json::json!(f64::from(*number))),
        Value::I32(number) => Some(serde_json::json!(f64::from(*number))),
        Value::I64(number) => Some(serde_json::json!(*number as f64)),
        Value::F64(number) => Some(serde_json::json!(*number)),
        Value::Str(text) => Some(serde_json::Value::String(text.to_string())),
        Value::Array(items) => {
            let array = items.iter().filter_map(value_to_json).collect::<Vec<_>>();
            Some(serde_json::Value::Array(array))
        }
        Value::Dict(entries) => {
            let mut object = serde_json::Map::new();
            for (key, nested) in entries.iter() {
                let key_text = match key {
                    Value::Str(text) => text.as_str().to_string(),
                    _ => value_to_json(key)
                        .and_then(|value| value.as_str().map(str::to_string))
                        .unwrap_or_default(),
                };
                if key_text.is_empty() {
                    continue;
                }
                if let Some(json) = value_to_json(nested) {
                    object.insert(normalize_dict_key(&key_text), json);
                }
            }
            Some(serde_json::Value::Object(object))
        }
        Value::Value(inner) => value_to_json(&**inner),
        _ => None,
    }
}

pub fn json_to_owned(value: &serde_json::Value) -> Option<OwnedValue> {
    match value {
        serde_json::Value::Null => None,
        serde_json::Value::Bool(flag) => Some(owned_bool(*flag)),
        serde_json::Value::Number(number) => {
            if let Some(unsigned) = number.as_u64() {
                Some(owned_u64(unsigned))
            } else {
                number.as_f64().map(owned_f64)
            }
        }
        serde_json::Value::String(text) => Some(owned_str(text)),
        serde_json::Value::Array(items) => {
            let owned: Vec<OwnedValue> = items.iter().filter_map(json_to_owned).collect();
            OwnedValue::try_from(Value::from(owned)).ok()
        }
        serde_json::Value::Object(map) => {
            let mut out = HashMap::new();
            for (key, nested) in map {
                if let Some(value) = json_to_owned(nested) {
                    out.insert(key.clone(), value);
                }
            }
            OwnedValue::try_from(Value::from(out)).ok()
        }
    }
}
