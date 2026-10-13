use std::collections::HashMap;
use std::sync::Arc;

use tokio::sync::Mutex;
use zbus::fdo;
use zbus::message::Header;
use zbus::object_server::SignalEmitter;
use zbus::zvariant::{OwnedValue, Value};
use zbus::{interface, Connection};

use crate::dbus_value::{json_to_owned, owned_bool, owned_str};
use crate::peer::resolve_peer;
use crate::state::State;

pub struct Configuration {
    pub state: Arc<Mutex<State>>,
}

fn value_from_zvariant(value: &Value<'_>) -> Option<serde_json::Value> {
    match value {
        Value::Bool(flag) => Some(serde_json::Value::Bool(*flag)),
        Value::F64(number) => Some(serde_json::json!(*number)),
        Value::I64(number) => Some(serde_json::json!(*number as f64)),
        Value::U64(number) => Some(serde_json::json!(*number as f64)),
        Value::I32(number) => Some(serde_json::json!(f64::from(*number))),
        Value::U32(number) => Some(serde_json::json!(f64::from(*number))),
        Value::Str(text) => Some(serde_json::Value::String(text.to_string())),
        Value::Value(inner) => value_from_zvariant(inner),
        _ => None,
    }
}

fn require_desktop(state: &State, sender: &str) -> fdo::Result<()> {
    match &state.desktop {
        Some(owner) if owner == sender => Ok(()),
        Some(_) => Err(fdo::Error::AccessDenied(
            "Only the registered shell may perform this action".into(),
        )),
        None => Err(fdo::Error::Failed(
            "No Helix desktop is registered".into(),
        )),
    }
}

fn string_from_owned(value: &OwnedValue) -> fdo::Result<String> {
    match &**value {
        Value::Str(text) => Ok(text.to_string()),
        Value::Value(inner) => match &**inner {
            Value::Str(text) => Ok(text.to_string()),
            _ => Err(fdo::Error::InvalidArgs(
                "Application IDs must be text".into(),
            )),
        },
        _ => Err(fdo::Error::InvalidArgs(
            "Application IDs must be text".into(),
        )),
    }
}

fn string_list_from_owned(items: &[OwnedValue]) -> fdo::Result<serde_json::Value> {
    let mut out = Vec::with_capacity(items.len());
    for item in items {
        out.push(serde_json::Value::String(string_from_owned(item)?));
    }
    Ok(serde_json::Value::Array(out))
}

#[interface(name = "org.helix.Desktop.Configuration1")]
impl Configuration {
    async fn get_snapshot(&self) -> HashMap<String, OwnedValue> {
        let state = self.state.lock().await;
        let mut map = HashMap::new();
        map.insert("available".into(), owned_bool(true));
        map.insert("helix-desktop".into(), owned_bool(state.desktop.is_some()));
        map.insert("audio".into(), owned_bool(true));
        map.insert("network".into(), owned_bool(true));
        map.insert("bluetooth".into(), owned_bool(true));
        map.insert("implementation".into(), owned_str("rust"));
        map.insert(
            "settings".into(),
            owned_bool(!state.config.catalog().is_empty()),
        );
        let caps: serde_json::Map<String, serde_json::Value> = state
            .desktop_capabilities
            .iter()
            .map(|(key, value)| (key.clone(), serde_json::Value::Bool(*value)))
            .collect();
        if let Some(owned) = json_to_owned(&serde_json::Value::Object(caps)) {
            map.insert("desktop-capabilities".into(), owned);
        }
        map
    }

    async fn get_settings(&self) -> Vec<OwnedValue> {
        let state = self.state.lock().await;
        state
            .config
            .catalog()
            .iter()
            .filter_map(json_to_owned)
            .collect()
    }

    async fn update(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        id: &str,
        value: Value<'_>,
    ) -> fdo::Result<()> {
        let json = value_from_zvariant(&value)
            .ok_or_else(|| fdo::Error::InvalidArgs("unsupported value type".into()))?;
        let mut state = self.state.lock().await;
        state
            .config
            .set(id, json.clone())
            .map_err(fdo::Error::InvalidArgs)?;
        if id == "audio.sound_volume" {
            if let Some(volume) = json.as_f64() {
                if let Some(sink) = state.audio.sinks.iter().find(|node| node.is_default) {
                    let sink_id = sink.id;
                    let _ = state.audio.set_volume(sink_id, volume);
                }
            }
        }
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn update_ordered_list(
        &self,
        _id: &str,
        _expected: Vec<OwnedValue>,
        _value: Vec<OwnedValue>,
    ) -> fdo::Result<()> {
        Err(fdo::Error::Failed(
            "UpdateOrderedList not implemented in Rust helix-desktop yet".into(),
        ))
    }

    async fn update_application_list(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        id: &str,
        expected: Vec<OwnedValue>,
        value: Vec<OwnedValue>,
    ) -> fdo::Result<()> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let expected_json = string_list_from_owned(&expected)?;
        let value_json = string_list_from_owned(&value)?;
        let mut state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        let def = helix_shell_core::settings::find_def(id)
            .ok_or_else(|| fdo::Error::InvalidArgs("Unknown ordered desktop list".into()))?;
        if def.type_name() != "application-list" {
            return Err(fdo::Error::InvalidArgs(
                "Unknown ordered desktop list".into(),
            ));
        }
        let current = state.config.get(id);
        if current != expected_json {
            return Err(fdo::Error::InvalidArgs(
                "The list changed. Refresh and try again".into(),
            ));
        }
        state
            .config
            .set(id, value_json)
            .map_err(fdo::Error::InvalidArgs)?;
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn reset(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        id: &str,
    ) -> fdo::Result<()> {
        let mut state = self.state.lock().await;
        state.config.reset(id).map_err(fdo::Error::InvalidArgs)?;
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn commit_overrides(&self, _baseline: &str, _candidate: &str) -> fdo::Result<()> {
        Err(fdo::Error::Failed(
            "CommitOverrides not implemented in Rust helix-desktop yet".into(),
        ))
    }

    async fn register_desktop(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] connection: &Connection,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
    ) -> fdo::Result<()> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let peer = resolve_peer(connection, &sender).await?;
        let mut state = self.state.lock().await;
        if let Some(owner) = &state.desktop {
            if owner != &peer.unique_name {
                return Err(fdo::Error::Failed(
                    "A Helix desktop is already registered".into(),
                ));
            }
        }
        state.desktop = Some(peer.unique_name);
        tracing::info!(client = %peer.client, "RegisterDesktop");
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn register_capabilities(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        capabilities: HashMap<String, OwnedValue>,
    ) -> fdo::Result<()> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let mut state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        let mut parsed = HashMap::new();
        for (key, value) in capabilities {
            if key != "gamma-control" {
                return Err(fdo::Error::InvalidArgs(format!(
                    "Unknown desktop capability {key}"
                )));
            }
            match &*value {
                Value::Bool(flag) => {
                    parsed.insert(key, *flag);
                }
                _ => {
                    return Err(fdo::Error::InvalidArgs(
                        "Capability values must be boolean".into(),
                    ));
                }
            }
        }
        let changed = state.desktop_capabilities != parsed;
        state.desktop_capabilities = parsed;
        drop(state);
        if changed {
            Self::changed(&emitter).await.ok();
        }
        Ok(())
    }

    async fn get_state(
        &self,
        #[zbus(header)] header: Header<'_>,
        owner: &str,
        key: &str,
        kind: &str,
    ) -> fdo::Result<HashMap<String, OwnedValue>> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        let json = state
            .runtime
            .get(owner, key, kind)
            .map_err(fdo::Error::InvalidArgs)?;
        let mut map = HashMap::new();
        if let Some(object) = json.as_object() {
            for (entry_key, value) in object {
                if let Some(owned) = json_to_owned(value) {
                    map.insert(entry_key.clone(), owned);
                }
            }
        }
        Ok(map)
    }

    async fn set_state(
        &self,
        #[zbus(header)] header: Header<'_>,
        owner: &str,
        key: &str,
        value: Value<'_>,
    ) -> fdo::Result<()> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let mut state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        match &value {
            Value::Bool(flag) => state
                .runtime
                .set_bool(owner, key, *flag)
                .map_err(fdo::Error::Failed)?,
            Value::Str(text) => state
                .runtime
                .set_string(owner, key, text)
                .map_err(fdo::Error::Failed)?,
            Value::Value(inner) => match &**inner {
                Value::Bool(flag) => state
                    .runtime
                    .set_bool(owner, key, *flag)
                    .map_err(fdo::Error::Failed)?,
                Value::Str(text) => state
                    .runtime
                    .set_string(owner, key, text)
                    .map_err(fdo::Error::Failed)?,
                _ => {
                    return Err(fdo::Error::InvalidArgs(
                        "Runtime state must be boolean or string".into(),
                    ));
                }
            },
            _ => {
                return Err(fdo::Error::InvalidArgs(
                    "Runtime state must be boolean or string".into(),
                ));
            }
        }
        Ok(())
    }

    async fn clear_state(
        &self,
        #[zbus(header)] header: Header<'_>,
        owner: &str,
    ) -> fdo::Result<()> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let mut state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        state
            .runtime
            .clear_owner(owner)
            .map_err(fdo::Error::Failed)?;
        Ok(())
    }

    #[zbus(signal)]
    async fn changed(emitter: &SignalEmitter<'_>) -> zbus::Result<()>;
}
