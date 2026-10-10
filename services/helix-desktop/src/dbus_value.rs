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

pub fn json_to_owned(value: &serde_json::Value) -> Option<OwnedValue> {
    match value {
        serde_json::Value::Null => None,
        serde_json::Value::Bool(flag) => Some(owned_bool(*flag)),
        serde_json::Value::Number(number) => {
            if let Some(unsigned) = number.as_u64() {
                Some(owned_u64(unsigned))
            } else if let Some(float) = number.as_f64() {
                Some(owned_f64(float))
            } else {
                None
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
