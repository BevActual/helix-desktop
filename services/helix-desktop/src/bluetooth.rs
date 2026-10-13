use std::collections::HashMap;
use std::sync::Arc;

use tokio::sync::Mutex;
use zbus::fdo;
use zbus::object_server::SignalEmitter;
use zbus::zvariant::{OwnedValue, Value};
use zbus::interface;

use crate::dbus_value::json_to_owned;
use crate::state::State;

pub struct Bluetooth {
    pub state: Arc<Mutex<State>>,
}

fn arg_str(args: &HashMap<String, OwnedValue>, key: &str) -> fdo::Result<String> {
    let value = args
        .get(key)
        .ok_or_else(|| fdo::Error::InvalidArgs(format!("missing {key}")))?;
    match &**value {
        Value::Str(text) => Ok(text.to_string()),
        _ => Err(fdo::Error::InvalidArgs(format!("bad {key}"))),
    }
}

fn arg_bool(args: &HashMap<String, OwnedValue>, key: &str) -> fdo::Result<bool> {
    let value = args
        .get(key)
        .ok_or_else(|| fdo::Error::InvalidArgs(format!("missing {key}")))?;
    match &**value {
        Value::Bool(flag) => Ok(*flag),
        _ => Err(fdo::Error::InvalidArgs(format!("bad {key}"))),
    }
}

fn snapshot_map(value: &serde_json::Value) -> HashMap<String, OwnedValue> {
    let mut map = HashMap::new();
    if let Some(object) = value.as_object() {
        for (key, nested) in object {
            if let Some(owned) = json_to_owned(nested) {
                map.insert(key.clone(), owned);
            }
        }
    }
    map
}

#[interface(name = "org.helix.Desktop.Bluetooth1")]
impl Bluetooth {
    async fn get_snapshot(&self) -> HashMap<String, OwnedValue> {
        let state = self.state.lock().await;
        snapshot_map(&state.bluetooth.snapshot())
    }

    async fn action(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        action: &str,
        args: HashMap<String, OwnedValue>,
    ) -> fdo::Result<u64> {
        let mut state = self.state.lock().await;
        let operation = state.alloc_operation();
        match action {
            "enable" => {
                let enabled = arg_bool(&args, "value")?;
                state
                    .bluetooth
                    .set_enabled(enabled)
                    .map_err(fdo::Error::Failed)?;
            }
            "scan" => {
                state
                    .bluetooth
                    .set_scanning(true)
                    .map_err(fdo::Error::Failed)?;
            }
            "stop-scan" => {
                state
                    .bluetooth
                    .set_scanning(false)
                    .map_err(fdo::Error::Failed)?;
            }
            "connect" => {
                let path = arg_str(&args, "path")?;
                state
                    .bluetooth
                    .connect(&path)
                    .map_err(fdo::Error::Failed)?;
            }
            "disconnect" => {
                let path = arg_str(&args, "path")?;
                state
                    .bluetooth
                    .disconnect(&path)
                    .map_err(fdo::Error::Failed)?;
            }
            "pair" => {
                let path = arg_str(&args, "path")?;
                state.bluetooth.pair(&path).map_err(fdo::Error::Failed)?;
            }
            "forget" => {
                let path = arg_str(&args, "path")?;
                state
                    .bluetooth
                    .forget(&path)
                    .map_err(fdo::Error::Failed)?;
            }
            other => {
                return Err(fdo::Error::Failed(format!(
                    "Unknown bluetooth action {other}"
                )));
            }
        }
        drop(state);
        Self::changed(&emitter).await.ok();
        Self::completed(&emitter, operation, true, "").await.ok();
        Ok(operation)
    }

    async fn reply(&self, _response: &str, _cancel: bool) -> fdo::Result<()> {
        Ok(())
    }

    async fn cancel(&self, _operation: u64) -> fdo::Result<()> {
        Ok(())
    }

    #[zbus(signal)]
    async fn changed(emitter: &SignalEmitter<'_>) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn prompt(
        emitter: &SignalEmitter<'_>,
        details: HashMap<String, OwnedValue>,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn completed(
        emitter: &SignalEmitter<'_>,
        operation: u64,
        ok: bool,
        result: &str,
    ) -> zbus::Result<()>;
}
