//! D-Bus values to JSON and back, for the service's `a{sv}` snapshots and action arguments. The service speaks JSON-shaped
//! variants (dictionaries with string keys, arrays, numbers, strings, booleans), so that is all this converts. Anything else
//! (file descriptors, unknown shapes) becomes `null` rather than an error: a snapshot with one odd field is still usable.

use std::collections::HashMap;

use serde_json::{json, Map, Number, Value as Json};
use zbus::zvariant::{OwnedValue, Value};

pub fn to_json(value: &Value<'_>) -> Json {
    match value {
        Value::U8(n) => json!(n),
        Value::Bool(b) => json!(b),
        Value::I16(n) => json!(n),
        Value::U16(n) => json!(n),
        Value::I32(n) => json!(n),
        Value::U32(n) => json!(n),
        Value::I64(n) => json!(n),
        Value::U64(n) => json!(n),
        // JSON has no NaN or infinity; a non-finite number is not a number to show.
        Value::F64(n) => Number::from_f64(*n).map_or(Json::Null, Json::Number),
        Value::Str(text) => Json::String(text.to_string()),
        Value::ObjectPath(path) => Json::String(path.to_string()),
        Value::Signature(signature) => Json::String(signature.to_string()),
        Value::Value(inner) => to_json(inner),
        Value::Array(array) => Json::Array(array.iter().map(to_json).collect()),
        Value::Structure(structure) => {
            Json::Array(structure.fields().iter().map(to_json).collect())
        }
        Value::Dict(dict) => {
            let mut object = Map::new();
            for (key, entry) in dict.iter() {
                if let Value::Str(key) = unwrap(key) {
                    object.insert(key.to_string(), to_json(entry));
                }
            }
            Json::Object(object)
        }
        _ => Json::Null,
    }
}

fn unwrap<'a, 'b>(value: &'a Value<'b>) -> &'a Value<'b> {
    match value {
        Value::Value(inner) => unwrap(inner),
        other => other,
    }
}

pub fn owned_to_json(value: &OwnedValue) -> Json {
    to_json(value)
}

/// The `a{sv}` argument of an Action call from a JSON object. Integers become unsigned 32-bit values (the service reads ids as
/// `uint32`), other numbers doubles. Nested objects, arrays and null are not arguments and are refused.
/// One catalog ordered-list row encoded as D-Bus `av` (each entry is a variant).
pub fn json_to_ordered_list_av(array: &Json) -> Result<Vec<OwnedValue>, String> {
    let items = array
        .as_array()
        .ok_or("ordered list baseline/candidate must be a JSON array")?;
    items.iter().map(json_to_list_element).collect()
}

fn json_to_list_element(value: &Json) -> Result<OwnedValue, String> {
    match value {
        Json::Null => Err("ordered list entries cannot be null".into()),
        Json::Bool(flag) => {
            OwnedValue::try_from(Value::Bool(*flag)).map_err(|error| error.to_string())
        }
        Json::Number(number) => {
            let converted = if let Some(whole) = number.as_i64() {
                Value::I64(whole)
            } else {
                Value::F64(number.as_f64().ok_or("number out of range")?)
            };
            OwnedValue::try_from(converted).map_err(|error| error.to_string())
        }
        Json::String(text) => {
            OwnedValue::try_from(Value::from(text.clone())).map_err(|error| error.to_string())
        }
        Json::Array(items) => {
            let owned: Vec<OwnedValue> = items
                .iter()
                .map(json_to_list_element)
                .collect::<Result<Vec<_>, _>>()?;
            OwnedValue::try_from(Value::from(owned)).map_err(|error| error.to_string())
        }
        Json::Object(map) => {
            let mut out = HashMap::new();
            for (key, nested) in map {
                out.insert(key.clone(), json_to_list_element(nested)?);
            }
            OwnedValue::try_from(Value::from(out)).map_err(|error| error.to_string())
        }
    }
}

pub fn args_from_json(args: &Json) -> Result<HashMap<String, OwnedValue>, String> {
    let object = args.as_object().ok_or("arguments must be an object")?;
    let mut out = HashMap::new();
    for (key, value) in object {
        let converted: Value<'static> = match value {
            Json::Bool(b) => Value::Bool(*b),
            Json::String(text) => Value::from(text.clone()),
            Json::Number(n) => match n.as_u64().and_then(|u| u32::try_from(u).ok()) {
                Some(unsigned) => Value::U32(unsigned),
                None => Value::F64(n.as_f64().ok_or("number out of range")?),
            },
            _ => return Err(format!("argument {key:?} has an unsupported type")),
        };
        out.insert(
            key.clone(),
            OwnedValue::try_from(converted).map_err(|error| error.to_string())?,
        );
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use zbus::zvariant::{Dict, Signature};

    fn dict(entries: Vec<(&str, Value<'static>)>) -> Value<'static> {
        let mut dict = Dict::new(
            &Signature::try_from("s").unwrap(),
            &Signature::try_from("v").unwrap(),
        );
        for (key, value) in entries {
            dict.append(Value::from(key.to_string()), Value::Value(Box::new(value)))
                .unwrap();
        }
        Value::Dict(dict)
    }

    #[test]
    fn a_snapshot_shaped_value_becomes_json() {
        let sink = dict(vec![
            ("id", Value::U64(48)),
            ("volume", Value::F64(0.44)),
            ("muted", Value::Bool(false)),
            ("name", Value::from("alsa_output.x".to_string())),
        ]);
        let snapshot = dict(vec![
            ("available", Value::Bool(true)),
            (
                "sinks",
                Value::Array(vec![Value::Value(Box::new(sink))].into()),
            ),
        ]);
        let json = to_json(&snapshot);
        assert_eq!(json["available"], true);
        assert_eq!(json["sinks"][0]["id"], 48);
        assert_eq!(json["sinks"][0]["volume"], 0.44);
        assert_eq!(json["sinks"][0]["name"], "alsa_output.x");
    }

    #[test]
    fn odd_values_become_null_instead_of_failing() {
        assert_eq!(to_json(&Value::F64(f64::NAN)), Json::Null);
        assert_eq!(to_json(&Value::F64(f64::INFINITY)), Json::Null);
        assert_eq!(to_json(&Value::I32(-3)), json!(-3));
    }

    #[test]
    fn action_arguments_use_the_types_the_service_reads() {
        let args =
            args_from_json(&json!({"id": 48, "value": 0.5, "input": false, "name": "x"})).unwrap();
        assert!(matches!(&*args["id"], Value::U32(48)));
        assert!(matches!(&*args["value"], Value::F64(v) if (*v - 0.5).abs() < 1e-12));
        assert!(matches!(&*args["input"], Value::Bool(false)));
        assert!(matches!(&*args["name"], Value::Str(_)));
    }

    #[test]
    fn arguments_that_are_not_plain_values_are_refused() {
        assert!(args_from_json(&json!({"a": [1]})).is_err());
        assert!(args_from_json(&json!({"a": {"b": 1}})).is_err());
        assert!(args_from_json(&json!({"a": null})).is_err());
        assert!(args_from_json(&json!([1, 2])).is_err());
        assert!(args_from_json(&json!("text")).is_err());
        // An integer too large for u32 is not silently truncated into an id.
        let big = args_from_json(&json!({"id": 5_000_000_000u64})).unwrap();
        assert!(matches!(&*big["id"], Value::F64(_)));
    }
}
