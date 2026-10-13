use std::collections::HashMap;
use std::sync::Arc;

use tokio::sync::Mutex;
use zbus::fdo;
use zbus::object_server::SignalEmitter;
use zbus::zvariant::{OwnedValue, Value};
use zbus::interface;

use crate::dbus_value::json_to_owned;
use crate::state::State;

pub struct Audio {
    pub state: Arc<Mutex<State>>,
}

fn arg_u32(args: &HashMap<String, OwnedValue>, key: &str) -> fdo::Result<u32> {
    let value = args
        .get(key)
        .ok_or_else(|| fdo::Error::InvalidArgs(format!("missing {key}")))?;
    match &**value {
        Value::U32(number) => Ok(*number),
        Value::U64(number) => u32::try_from(*number)
            .map_err(|_| fdo::Error::InvalidArgs(format!("bad {key}"))),
        Value::I32(number) if *number >= 0 => Ok(*number as u32),
        Value::I64(number) if *number >= 0 => u32::try_from(*number)
            .map_err(|_| fdo::Error::InvalidArgs(format!("bad {key}"))),
        _ => Err(fdo::Error::InvalidArgs(format!("bad {key}"))),
    }
}

fn arg_f64(args: &HashMap<String, OwnedValue>, key: &str) -> fdo::Result<f64> {
    let value = args
        .get(key)
        .ok_or_else(|| fdo::Error::InvalidArgs(format!("missing {key}")))?;
    match &**value {
        Value::F64(number) => Ok(*number),
        Value::I64(number) => Ok(*number as f64),
        Value::U64(number) => Ok(*number as f64),
        Value::I32(number) => Ok(f64::from(*number)),
        Value::U32(number) => Ok(f64::from(*number)),
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

#[interface(name = "org.helix.Desktop.Audio1")]
impl Audio {
    async fn get_snapshot(&self) -> HashMap<String, OwnedValue> {
        let state = self.state.lock().await;
        let snap = state.audio.snapshot();
        let mut map = HashMap::new();
        if let Some(object) = snap.as_object() {
            for (key, value) in object {
                if let Some(owned) = json_to_owned(value) {
                    map.insert(key.clone(), owned);
                }
            }
        }
        map
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
            "volume" => {
                let id = arg_u32(&args, "id")?;
                let volume = arg_f64(&args, "value")?;
                state
                    .audio
                    .set_volume(id, volume)
                    .map_err(fdo::Error::Failed)?;
                if state.audio.sinks.iter().any(|node| node.id == id) {
                    let _ = state
                        .config
                        .set("audio.sound_volume", serde_json::json!(volume.clamp(0.0, 1.0)));
                }
            }
            "mute" => {
                let id = arg_u32(&args, "id")?;
                let muted = arg_bool(&args, "value")?;
                state
                    .audio
                    .set_muted(id, muted)
                    .map_err(fdo::Error::Failed)?;
            }
            "default" => {
                let id = arg_u32(&args, "id")?;
                let input = if args.contains_key("input") {
                    arg_bool(&args, "input")?
                } else {
                    false
                };
                state
                    .audio
                    .set_default(id, input)
                    .map_err(fdo::Error::Failed)?;
            }
            other => return Err(fdo::Error::Failed(format!("Unknown audio action {other}"))),
        }
        drop(state);
        Self::changed(&emitter).await.ok();
        Self::completed(&emitter, operation, true, "").await.ok();
        Ok(operation)
    }

    async fn cancel(&self, _operation: u64) -> fdo::Result<()> {
        Ok(())
    }

    async fn start_input_meter(&self, _source: u32) -> fdo::Result<()> {
        Ok(())
    }

    async fn stop_input_meter(&self) -> fdo::Result<()> {
        Ok(())
    }

    #[zbus(signal)]
    async fn changed(emitter: &SignalEmitter<'_>) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn completed(
        emitter: &SignalEmitter<'_>,
        operation: u64,
        ok: bool,
        result: &str,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn input_level(
        emitter: &SignalEmitter<'_>,
        source: u32,
        peak: f64,
        available: bool,
    ) -> zbus::Result<()>;
}
