//! Desktop D-Bus client. Configuration, Audio, Network, Bluetooth snapshots and actions, and the
//! Agent Surface **confirmer** role only (`RegisterConfirmer`, `Confirm`, the two confirmation signals).
//! The shell must never call Hello, RequestAccess, Action on Agent Surface, or the manager methods.

mod activity;
mod agent;
mod contract;
mod json;
mod manager;

pub use activity::{ActivityClient, ACTIVITY_METHODS, ACTIVITY_SIGNALS};
pub use agent::{details_from_owned, ConfirmerClient, CONFIRMER_METHODS, CONFIRMER_SIGNALS};
pub use contract::{parse_interfaces, protocol_xml, Interface, BUS_NAME, OBJECT_PATH};
pub use json::{args_from_json, owned_to_json, to_json};
pub use manager::{ManagerClient, MANAGER_METHODS};

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

/// The service interfaces that follow the snapshot / action / changed pattern.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Domain {
    Audio,
    Network,
    Bluetooth,
    /// Settings. Has no `Action`; use [`Desktop::settings`] and [`Desktop::update_setting`], and `watch` for changes.
    Configuration,
}

impl Domain {
    pub fn interface(self) -> &'static str {
        match self {
            Self::Audio => "org.helix.Desktop.Audio1",
            Self::Network => "org.helix.Desktop.Network1",
            Self::Bluetooth => "org.helix.Desktop.Bluetooth1",
            Self::Configuration => "org.helix.Desktop.Configuration1",
        }
    }
}

pub struct ServiceStarts {
    changes: zbus::fdo::NameOwnerChangedStream,
}

impl ServiceStarts {
    pub async fn next(&mut self) -> Option<()> {
        while let Some(change) = self.changes.next().await {
            let Ok(args) = change.args() else { continue };
            if args.name().as_str() == BUS_NAME && args.new_owner().is_some() {
                return Some(());
            }
        }
        None
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

    pub fn connection(&self) -> &zbus::Connection {
        &self.connection
    }

    /// Claim the desktop role so the shell may register as Agent Surface confirmer.
    pub async fn register_desktop(&self) -> zbus::Result<()> {
        let proxy = self.domain_proxy(Domain::Configuration).await?;
        proxy.call("RegisterDesktop", &()).await
    }

    /// A watch on who owns the service's name: `next()` completes each time a (new) process takes it. A signal, not a poll.
    pub async fn watch_service_starts(&self) -> zbus::Result<ServiceStarts> {
        let bus = zbus::fdo::DBusProxy::new(&self.connection).await?;
        Ok(ServiceStarts {
            changes: bus.receive_name_owner_changed().await?,
        })
    }

    pub fn manager(&self) -> ManagerClient {
        ManagerClient::new(self.connection.clone())
    }

    pub fn activity(&self) -> ActivityClient {
        ActivityClient::new(self.connection.clone())
    }

    pub fn confirmer(&self) -> ConfirmerClient {
        ConfirmerClient::new(self.connection.clone())
    }

    async fn domain_proxy(&self, domain: Domain) -> zbus::Result<zbus::Proxy<'static>> {
        zbus::Proxy::new(&self.connection, BUS_NAME, OBJECT_PATH, domain.interface()).await
    }

    /// The domain's `GetSnapshot` as JSON (for audio: `{available, sinks, sources, programs}`).
    pub async fn snapshot(&self, domain: Domain) -> zbus::Result<serde_json::Value> {
        let proxy = self.domain_proxy(domain).await?;
        let snapshot: std::collections::HashMap<String, OwnedValue> =
            proxy.call("GetSnapshot", &()).await?;
        let mut object = serde_json::Map::new();
        for (key, value) in &snapshot {
            object.insert(key.clone(), owned_to_json(value));
        }
        Ok(serde_json::Value::Object(object))
    }

    /// Starts one of the domain's actions and returns the operation id. The service checks everything again.
    pub async fn action(
        &self,
        domain: Domain,
        action: &str,
        args: &serde_json::Value,
    ) -> zbus::Result<u64> {
        let proxy = self.domain_proxy(domain).await?;
        let args = args_from_json(args).map_err(zbus::Error::Failure)?;
        proxy.call("Action", &(action, args)).await
    }

    /// Calls `on_change` after every `Changed` signal of the domain. Does not poll. Returns when the stream ends.
    pub async fn watch<F>(&self, domain: Domain, mut on_change: F) -> zbus::Result<()>
    where
        F: FnMut() + Send,
    {
        let proxy = self.domain_proxy(domain).await?;
        let mut changes = proxy.receive_signal("Changed").await?;
        while changes.next().await.is_some() {
            on_change();
        }
        Ok(())
    }

    /// Answers the question the service asked the pairing client (`Bluetooth1.Reply`). `cancel` refuses it.
    pub async fn bluetooth_reply(&self, response: &str, cancel: bool) -> zbus::Result<()> {
        let proxy = self.domain_proxy(Domain::Bluetooth).await?;
        proxy.call("Reply", &(response, cancel)).await
    }

    /// Calls `on_prompt` with every `Prompt` signal the service sends this client for a pairing (kind, device, passkey, pin, entered).
    /// Returns when the stream ends.
    pub async fn watch_bluetooth_prompt<F>(&self, mut on_prompt: F) -> zbus::Result<()>
    where
        F: FnMut(serde_json::Value) + Send,
    {
        let proxy = self.domain_proxy(Domain::Bluetooth).await?;
        let mut prompts = proxy.receive_signal("Prompt").await?;
        while let Some(message) = prompts.next().await {
            if let Ok((body,)) = message.body().deserialize::<(std::collections::HashMap<String, OwnedValue>,)>() {
                let mut object = serde_json::Map::new();
                for (key, value) in &body {
                    object.insert(key.clone(), owned_to_json(value));
                }
                on_prompt(serde_json::Value::Object(object));
            }
        }
        Ok(())
    }

    /// Calls `on_done` with `(operation, succeeded, message)` for every `Completed` signal of the domain.
    pub async fn watch_completed<F>(&self, domain: Domain, mut on_done: F) -> zbus::Result<()>
    where
        F: FnMut(u64, bool, String) + Send,
    {
        let proxy = self.domain_proxy(domain).await?;
        let mut done = proxy.receive_signal("Completed").await?;
        while let Some(message) = done.next().await {
            if let Ok((operation, ok, text)) = message.body().deserialize::<(u64, bool, String)>() {
                on_done(operation, ok, text);
            }
        }
        Ok(())
    }

    /// The service's settings list (`GetSettings`) as a JSON array of entries.
    pub async fn settings(&self) -> zbus::Result<serde_json::Value> {
        let proxy = self.domain_proxy(Domain::Configuration).await?;
        let entries: Vec<OwnedValue> = proxy.call("GetSettings", &()).await?;
        Ok(serde_json::Value::Array(
            entries.iter().map(owned_to_json).collect(),
        ))
    }

    /// `Configuration1.Update(id, value)` for a boolean, a string or a number (whole numbers as 64-bit integers). The service validates the id and the value again.
    pub async fn update_setting(&self, id: &str, value: &serde_json::Value) -> zbus::Result<()> {
        let proxy = self.domain_proxy(Domain::Configuration).await?;
        let value: Value<'static> = match value {
            serde_json::Value::Bool(flag) => Value::Bool(*flag),
            serde_json::Value::String(text) => Value::from(text.clone()),
            serde_json::Value::Number(number) => match number.as_i64() {
                Some(whole) => Value::I64(whole),
                None => Value::F64(number.as_f64().unwrap_or(0.0)),
            },
            _ => {
                return Err(zbus::Error::Failure(
                    "only a boolean, a string or a number can be set".into(),
                ))
            }
        };
        proxy.call("Update", &(id, value)).await
    }

    /// `Configuration1.UpdateOrderedList(id, baseline, candidate)` for key-chords and other ordered lists.
    /// Each entry is a string wrapped as a variant (`av`). The service compares `baseline` to what it holds and refuses a stale write.
    pub async fn update_ordered_list(
        &self,
        id: &str,
        baseline: &[String],
        candidate: &[String],
    ) -> zbus::Result<()> {
        let proxy = self.domain_proxy(Domain::Configuration).await?;
        let to_av = |items: &[String]| -> zbus::Result<Vec<OwnedValue>> {
            items
                .iter()
                .map(|text| {
                    OwnedValue::try_from(Value::from(text.as_str()))
                        .map_err(|error| zbus::Error::Failure(error.to_string()))
                })
                .collect()
        };
        let baseline = to_av(baseline)?;
        let candidate = to_av(candidate)?;
        proxy.call("UpdateOrderedList", &(id, baseline, candidate)).await
    }

    pub async fn audio_snapshot(&self) -> zbus::Result<serde_json::Value> {
        self.snapshot(Domain::Audio).await
    }

    pub async fn audio_action(&self, action: &str, args: &serde_json::Value) -> zbus::Result<u64> {
        self.action(Domain::Audio, action, args).await
    }

    pub async fn watch_audio<F>(&self, on_change: F) -> zbus::Result<()>
    where
        F: FnMut() + Send,
    {
        self.watch(Domain::Audio, on_change).await
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
        for domain in [
            super::Domain::Audio,
            super::Domain::Network,
            super::Domain::Bluetooth,
        ] {
            let interface = &interfaces[domain.interface()];
            for method in ["GetSnapshot", "Action"] {
                assert!(
                    interface.methods.iter().any(|name| name == method),
                    "{} {method}",
                    domain.interface()
                );
            }
            assert!(
                interface.signals.iter().any(|name| name == "Changed"),
                "{}",
                domain.interface()
            );
        }
    }
}
