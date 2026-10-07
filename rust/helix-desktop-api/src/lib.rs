//! Read `audio.sound_volume` from Configuration1 and follow its Changed signal.
//! The shell shows that number. This crate does not call Agent1: the shell must not hold grants
//! or talk to the model.

mod contract;

pub use contract::{parse_interfaces, protocol_xml, Interface, BUS_NAME, OBJECT_PATH};

use futures_util::StreamExt;
use zbus::proxy;
use zbus::zvariant::{Dict, OwnedValue, Value};

#[proxy(
    interface = "org.helix.Desktop.Configuration1",
    default_service = "org.helix.Desktop",
    default_path = "/org/helix/Desktop"
)]
trait Configuration {
    fn get_settings(&self) -> zbus::Result<Vec<OwnedValue>>;

    fn update(&self, id: &str, value: Value<'_>) -> zbus::Result<()>;

    #[zbus(signal)]
    fn changed(&self) -> zbus::Result<()>;
}

/// Interface-sound volume from a GetSettings catalog, as a fraction in 0..=1.
pub fn sound_volume(entries: &[OwnedValue]) -> Option<f64> {
    for entry in entries {
        let Some(dict) = as_dict(entry) else { continue };
        let Some(id) = dict_string(&dict, "id") else {
            continue;
        };
        if id != "audio.sound_volume" {
            continue;
        }
        return dict_number(&dict, "value").map(|volume| volume.clamp(0.0, 1.0));
    }
    None
}

pub fn sound_volume_label(volume: Option<f64>) -> String {
    match volume {
        Some(volume) => {
            let percent = (volume.clamp(0.0, 1.0) * 100.0).round() as u8;
            format!("Sound {percent}%")
        }
        None => "Sound unavailable".to_string(),
    }
}

fn as_dict(entry: &OwnedValue) -> Option<Dict<'_, '_>> {
    let value = match &**entry {
        Value::Value(inner) => inner.as_ref(),
        other => other,
    };
    match value {
        Value::Dict(dict) => dict.try_clone().ok(),
        _ => None,
    }
}

fn dict_string(dict: &Dict<'_, '_>, key: &str) -> Option<String> {
    let key = Value::from(key);
    let value = dict.iter().find(|(candidate, _)| *candidate == &key)?.1;
    value_string(value)
}

fn dict_number(dict: &Dict<'_, '_>, key: &str) -> Option<f64> {
    let key = Value::from(key);
    let value = dict.iter().find(|(candidate, _)| *candidate == &key)?.1;
    value_number(value)
}

fn value_string(value: &Value<'_>) -> Option<String> {
    match value {
        Value::Str(text) => Some(text.to_string()),
        Value::Value(inner) => value_string(inner),
        _ => None,
    }
}

fn value_number(value: &Value<'_>) -> Option<f64> {
    match value {
        Value::F64(number) => Some(*number),
        Value::I64(number) => Some(*number as f64),
        Value::U64(number) => Some(*number as f64),
        Value::I32(number) => Some(f64::from(*number)),
        Value::U32(number) => Some(f64::from(*number)),
        Value::Value(inner) => value_number(inner),
        _ => None,
    }
}

pub struct Desktop {
    connection: zbus::Connection,
}

impl Desktop {
    pub async fn session() -> zbus::Result<Self> {
        Ok(Self {
            connection: zbus::Connection::session().await?,
        })
    }

    pub async fn sound_volume(&self) -> zbus::Result<Option<f64>> {
        let proxy = ConfigurationProxy::new(&self.connection).await?;
        Ok(sound_volume(&proxy.get_settings().await?))
    }

    /// Calls `on_volume` with the current value, then again after every Configuration1.Changed.
    pub async fn watch_sound_volume<F>(&self, mut on_volume: F) -> zbus::Result<()>
    where
        F: FnMut(Option<f64>) + Send,
    {
        let proxy = ConfigurationProxy::new(&self.connection).await?;
        on_volume(sound_volume(&proxy.get_settings().await?));
        let mut changes = proxy.receive_changed().await?;
        while changes.next().await.is_some() {
            on_volume(sound_volume(&proxy.get_settings().await?));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use zbus::zvariant::{Dict, OwnedValue, Signature, Value};

    use super::{sound_volume, sound_volume_label, BUS_NAME, OBJECT_PATH};

    fn setting(id: &str, value: impl Into<Value<'static>>) -> OwnedValue {
        let mut dict = Dict::new(
            &Signature::try_from("s").unwrap(),
            &Signature::try_from("v").unwrap(),
        );
        dict.append(Value::from("id"), Value::Value(Box::new(Value::from(id))))
            .unwrap();
        dict.append(Value::from("value"), Value::Value(Box::new(value.into())))
            .unwrap();
        OwnedValue::try_from(Value::Dict(dict)).unwrap()
    }

    #[test]
    fn reads_the_sound_volume_setting() {
        let entries = vec![
            setting("audio.enable_sounds", Value::from(true)),
            setting("audio.sound_volume", Value::from(0.25f64)),
        ];
        assert_eq!(sound_volume(&entries), Some(0.25));
        assert_eq!(sound_volume_label(Some(0.5)), "Sound 50%");
        assert_eq!(sound_volume_label(Some(1.4)), "Sound 100%");
        assert_eq!(sound_volume_label(None), "Sound unavailable");
        assert_eq!(BUS_NAME, "org.helix.Desktop");
        assert_eq!(OBJECT_PATH, "/org/helix/Desktop");
    }

    #[test]
    fn wrapped_variants_still_parse() {
        let mut dict = Dict::new(
            &Signature::try_from("s").unwrap(),
            &Signature::try_from("v").unwrap(),
        );
        dict.append(
            Value::from("id"),
            Value::Value(Box::new(Value::from("audio.sound_volume"))),
        )
        .unwrap();
        dict.append(
            Value::from("value"),
            Value::Value(Box::new(Value::from(0.5f64))),
        )
        .unwrap();
        let wrapped = OwnedValue::try_from(Value::Value(Box::new(Value::Dict(dict)))).unwrap();
        assert_eq!(sound_volume(&[wrapped]), Some(0.5));
    }

    #[test]
    fn client_calls_only_methods_the_contract_lists() {
        let interfaces = super::parse_interfaces(super::protocol_xml());
        let configuration = &interfaces["org.helix.Desktop.Configuration1"];
        for method in ["GetSettings", "Update"] {
            assert!(
                configuration.methods.iter().any(|name| name == method),
                "{method}"
            );
        }
        assert!(configuration.signals.iter().any(|name| name == "Changed"));
    }
}
