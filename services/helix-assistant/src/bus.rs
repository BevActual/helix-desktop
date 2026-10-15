//! D-Bus plumbing: JSON <-> variant conversion (the same mapping as C++ `toVariant`/`fromVariant`) and a collector of
//! `Completed(operation, ok, text)` signals, subscribed before the call so a fast reply is never missed.

use std::collections::{BTreeMap, HashMap};
use std::time::{Duration, Instant};

use futures_util::StreamExt;
use serde_json::{json, Value as Json};
use zbus::zvariant::{OwnedValue, Value};
use zbus::proxy::SignalStream;

pub fn to_variant(value: &Json) -> Option<OwnedValue> {
    let wrapped = match value {
        Json::Null => return None,
        Json::Bool(flag) => Value::from(*flag),
        Json::Number(number) => match number.as_u64() {
            Some(unsigned) => Value::from(unsigned),
            None => match number.as_i64() {
                Some(signed) => Value::from(signed),
                None => Value::from(number.as_f64()?),
            },
        },
        Json::String(text) => Value::from(text.as_str()),
        Json::Array(items) => {
            let owned: Vec<OwnedValue> = items.iter().filter_map(to_variant).collect();
            Value::from(owned)
        }
        Json::Object(map) => {
            let owned: HashMap<String, OwnedValue> = map
                .iter()
                .filter_map(|(key, nested)| to_variant(nested).map(|v| (key.clone(), v)))
                .collect();
            Value::from(owned)
        }
    };
    OwnedValue::try_from(wrapped).ok()
}

/// The `a{sv}` argument of `Action`.
pub fn to_vardict(object: &Json) -> HashMap<String, OwnedValue> {
    object
        .as_object()
        .map(|map| {
            map.iter()
                .filter_map(|(key, nested)| to_variant(nested).map(|v| (key.clone(), v)))
                .collect()
        })
        .unwrap_or_default()
}

pub fn from_variant(value: &Value<'_>) -> Json {
    match value {
        Value::Bool(flag) => json!(*flag),
        Value::U8(n) => json!(*n),
        Value::U16(n) => json!(*n),
        Value::U32(n) => json!(*n),
        Value::U64(n) => json!(*n),
        Value::I16(n) => json!(*n),
        Value::I32(n) => json!(*n),
        Value::I64(n) => json!(*n),
        Value::F64(n) => json!(*n),
        Value::Str(text) => json!(text.as_str()),
        Value::ObjectPath(path) => json!(path.as_str()),
        Value::Array(items) => Json::Array(items.iter().map(from_variant).collect()),
        Value::Dict(entries) => {
            let mut object = serde_json::Map::new();
            for (key, nested) in entries.iter() {
                if let Value::Str(key) = key {
                    object.insert(key.to_string(), from_variant(nested));
                }
            }
            Json::Object(object)
        }
        Value::Value(inner) => from_variant(inner),
        _ => Json::Null,
    }
}

#[derive(Debug, Clone)]
pub struct Done {
    pub ok: bool,
    pub result: Json,
}

/// Completed signals of one service, collected as they arrive.
pub struct Waiter {
    stream: SignalStream<'static>,
    seen: BTreeMap<u64, Done>,
}

impl Waiter {
    pub fn new(stream: SignalStream<'static>) -> Self {
        Self { stream, seen: BTreeMap::new() }
    }

    fn take(&mut self, message: &zbus::Message) {
        let Ok((operation, ok, text)) = message.body().deserialize::<(u64, bool, String)>() else {
            return;
        };
        let result = serde_json::from_str(&text).unwrap_or_else(|_| json!({"error": text}));
        self.seen.insert(operation, Done { ok, result });
    }

    pub async fn wait(&mut self, operation: u64, seconds: u64) -> Done {
        let end = Instant::now() + Duration::from_secs(seconds);
        loop {
            if let Some(done) = self.seen.remove(&operation) {
                return done;
            }
            let left = end.saturating_duration_since(Instant::now());
            match tokio::time::timeout(left, self.stream.next()).await {
                Ok(Some(message)) => self.take(&message),
                _ => return Done { ok: false, result: json!({"error": "timed out"}) },
            }
        }
    }
}
