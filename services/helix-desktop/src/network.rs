use std::collections::HashMap;
use std::sync::Arc;

use tokio::sync::Mutex;
use zbus::fdo;
use zbus::object_server::SignalEmitter;
use zbus::zvariant::{OwnedValue, Value};
use zbus::interface;

use crate::dbus_value::json_to_owned;
use crate::state::State;

pub struct Network {
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

#[interface(name = "org.helix.Desktop.Network1")]
impl Network {
    async fn get_snapshot(&self) -> HashMap<String, OwnedValue> {
        let state = self.state.lock().await;
        snapshot_map(&state.network.snapshot())
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
                state.network.set_enabled(enabled);
            }
            "scan" => {
                state.network.scan();
            }
            "connect" => {
                let path = arg_str(&args, "path")?;
                let secret = match args.get("secret") {
                    Some(_) => Some(arg_str(&args, "secret")?),
                    None => None,
                };
                state
                    .network
                    .connect(&path, secret.as_deref())
                    .map_err(fdo::Error::Failed)?;
            }
            "disconnect" => {
                state.network.disconnect().map_err(fdo::Error::Failed)?;
            }
            "forget" => {
                let ssid = arg_str(&args, "ssid")?;
                state.network.forget(&ssid).map_err(fdo::Error::Failed)?;
            }
            "vpn-connect" => {
                let path = arg_str(&args, "path")?;
                state
                    .network
                    .set_vpn(&path, true)
                    .map_err(fdo::Error::Failed)?;
            }
            "vpn-disconnect" => {
                let path = arg_str(&args, "path")?;
                state
                    .network
                    .set_vpn(&path, false)
                    .map_err(fdo::Error::Failed)?;
            }
            "enterprise" => {
                return Err(fdo::Error::Failed(
                    "Enterprise connect not implemented in Rust helix-desktop yet".into(),
                ));
            }
            other => {
                return Err(fdo::Error::Failed(format!("Unknown network action {other}")));
            }
        }
        drop(state);
        Self::changed(&emitter).await.ok();
        Self::completed(&emitter, operation, true, "").await.ok();
        Ok(operation)
    }

    async fn get_wifi_secret(&self) -> fdo::Result<String> {
        let state = self.state.lock().await;
        Ok(state.network.wifi_secret().to_string())
    }

    async fn get_dns(&self) -> HashMap<String, OwnedValue> {
        let state = self.state.lock().await;
        snapshot_map(&state.network.dns_snapshot())
    }

    async fn set_dns(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        _args: HashMap<String, OwnedValue>,
    ) -> fdo::Result<HashMap<String, OwnedValue>> {
        let state = self.state.lock().await;
        let map = snapshot_map(&state.network.dns_snapshot());
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(map)
    }

    async fn reply(&self, _secret: &str, _cancel: bool) -> fdo::Result<()> {
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
