use std::collections::HashMap;
use std::sync::Arc;

use tokio::sync::Mutex;
use zbus::fdo;
use zbus::interface;
use zbus::message::Header;
use zbus::object_server::SignalEmitter;
use zbus::zvariant::{OwnedValue, Value};

use crate::dbus_value::json_to_owned;
use crate::desktop_operations::{
    DesktopOperation, OperationCancel, OperationWait, BLUETOOTH_INTERFACE,
};
use crate::state::State;

pub struct Bluetooth {
    pub state: Arc<Mutex<State>>,
}

struct ActionPlan {
    operation: u64,
    wait: OperationWait,
    cancel: Option<OperationCancel>,
}

fn sender_name(header: &Header<'_>) -> fdo::Result<String> {
    Ok(header
        .sender()
        .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
        .to_string())
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
        #[zbus(header)] header: Header<'_>,
        action: &str,
        args: HashMap<String, OwnedValue>,
    ) -> fdo::Result<u64> {
        let sender = sender_name(&header)?;
        let claim_pair = action == "pair";
        if claim_pair {
            let state = self.state.lock().await;
            if state.agents.bluetooth_owner().is_some() {
                return Err(fdo::Error::Failed("Another client is pairing".into()));
            }
            state.agents.set_bluetooth_owner(sender.clone());
        }

        let state_ref = Arc::clone(&self.state);
        let action_name = action.to_string();
        let plan = tokio::task::spawn_blocking(move || {
            let mut state = state_ref.blocking_lock();
            let operation = state.alloc_operation();
            let result: fdo::Result<ActionPlan> = match action_name.as_str() {
                "enable" => {
                    let enabled = arg_bool(&args, "value")?;
                    state
                        .bluetooth
                        .set_enabled(enabled)
                        .map_err(fdo::Error::Failed)?;
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::Immediate,
                        cancel: None,
                    })
                }
                "scan" => {
                    state
                        .bluetooth
                        .set_scanning(true)
                        .map_err(fdo::Error::Failed)?;
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::BluetoothDiscovering { enabled: true },
                        cancel: None,
                    })
                }
                "stop-scan" => {
                    state
                        .bluetooth
                        .set_scanning(false)
                        .map_err(fdo::Error::Failed)?;
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::BluetoothDiscovering { enabled: false },
                        cancel: None,
                    })
                }
                "connect" => {
                    let path = arg_str(&args, "path")?;
                    state.bluetooth.connect(&path).map_err(fdo::Error::Failed)?;
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::BluetoothConnected { path },
                        cancel: None,
                    })
                }
                "disconnect" => {
                    let path = arg_str(&args, "path")?;
                    state
                        .bluetooth
                        .disconnect(&path)
                        .map_err(fdo::Error::Failed)?;
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::BluetoothDisconnected { path },
                        cancel: None,
                    })
                }
                "pair" => {
                    let path = arg_str(&args, "path")?;
                    let result = state.bluetooth.pair(&path).map_err(fdo::Error::Failed);
                    if result.is_err() {
                        state.agents.cancel_bluetooth_pending();
                        state.agents.clear_bluetooth_owner();
                    }
                    result?;
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::BluetoothPaired { path: path.clone() },
                        cancel: Some(OperationCancel::BluetoothPair { path }),
                    })
                }
                "cancel-pair" => {
                    let path = arg_str(&args, "path")?;
                    state.agents.cancel_bluetooth_pending();
                    state
                        .bluetooth
                        .cancel_pair(&path)
                        .map_err(fdo::Error::Failed)?;
                    state.agents.clear_bluetooth_owner();
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::BluetoothCancelPair,
                        cancel: None,
                    })
                }
                "forget" => {
                    let path = arg_str(&args, "path")?;
                    state.bluetooth.forget(&path).map_err(fdo::Error::Failed)?;
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::BluetoothForgotten { path },
                        cancel: None,
                    })
                }
                "trust" => {
                    let path = arg_str(&args, "path")?;
                    let trusted = arg_bool(&args, "value")?;
                    state
                        .bluetooth
                        .set_trusted(&path, trusted)
                        .map_err(fdo::Error::Failed)?;
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::BluetoothTrusted { path, trusted },
                        cancel: None,
                    })
                }
                other => Err(fdo::Error::Failed(format!(
                    "Unknown bluetooth action {other}"
                ))),
            };
            result
        })
        .await
        .map_err(|error| fdo::Error::Failed(error.to_string()))??;

        let finished_now = {
            let mut locked = self.state.lock().await;
            locked.push_operation(
                plan.operation,
                BLUETOOTH_INTERFACE,
                sender.clone(),
                action.to_string(),
                plan.wait.clone(),
                plan.cancel.clone(),
            );
            let done = locked.operation_done(&plan.wait);
            if done {
                locked
                    .desktop_operations
                    .retain(|op| op.id != plan.operation);
                let op = DesktopOperation {
                    id: plan.operation,
                    interface: BLUETOOTH_INTERFACE,
                    sender: sender.clone(),
                    action: action.to_string(),
                    wait: plan.wait.clone(),
                    deadline: std::time::Instant::now(),
                    cancel: plan.cancel.clone(),
                };
                locked.finish_operation_side_effects(&op, true);
            }
            done
        };
        Self::changed(&emitter).await.ok();
        if finished_now {
            Self::completed(&emitter, plan.operation, true, "")
                .await
                .ok();
        }
        Ok(plan.operation)
    }

    async fn reply(
        &self,
        #[zbus(header)] header: Header<'_>,
        response: &str,
        cancel: bool,
    ) -> fdo::Result<()> {
        let sender = sender_name(&header)?;
        let mut state = self.state.lock().await;
        if state.agents.bluetooth_owner().as_deref() != Some(sender.as_str()) {
            return Err(fdo::Error::InvalidArgs(
                "No pairing request for this client".into(),
            ));
        }
        if !state.agents.bt_has_pending() {
            return Err(fdo::Error::InvalidArgs(
                "No pairing request for this client".into(),
            ));
        }
        state
            .agents
            .submit_bluetooth_reply(response, cancel)
            .map_err(|message| fdo::Error::InvalidArgs(message.into()))?;
        if cancel {
            let cancelled = crate::desktop_operations::cancel_auth_operations(
                &mut state,
                BLUETOOTH_INTERFACE,
                &sender,
            );
            let connection = state.bus.clone();
            drop(state);
            if let Some(connection) = connection {
                for id in cancelled {
                    let _ = connection
                        .emit_signal(
                            Some(sender.as_str()),
                            crate::desktop_operations::OBJECT_PATH,
                            BLUETOOTH_INTERFACE,
                            "Completed",
                            &(id, false, "Cancelled"),
                        )
                        .await;
                }
            }
        }
        Ok(())
    }

    async fn cancel(&self, #[zbus(header)] header: Header<'_>, operation: u64) -> fdo::Result<()> {
        let sender = sender_name(&header)?;
        let mut state = self.state.lock().await;
        state
            .cancel_operation(BLUETOOTH_INTERFACE, &sender, operation)
            .map_err(|message| fdo::Error::InvalidArgs(message.into()))?;
        let connection = state
            .bus
            .clone()
            .ok_or_else(|| fdo::Error::Failed("session bus unavailable".into()))?;
        let _ = connection
            .emit_signal(
                Some(sender.as_str()),
                crate::desktop_operations::OBJECT_PATH,
                BLUETOOTH_INTERFACE,
                "Completed",
                &(operation, false, "Cancelled"),
            )
            .await;
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
