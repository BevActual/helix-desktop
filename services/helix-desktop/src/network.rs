use std::collections::HashMap;
use std::sync::Arc;

use tokio::sync::Mutex;
use zbus::fdo;
use zbus::message::Header;
use zbus::object_server::SignalEmitter;
use zbus::zvariant::{OwnedValue, Value};
use zbus::interface;

use crate::dbus_value::{json_to_owned, owned_bool, owned_str};
use crate::desktop_operations::{
    DesktopOperation, OperationCancel, OperationWait, NETWORK_INTERFACE,
};
use crate::network_backend::{parse_dns_args, EnterpriseCredentials};
use crate::state::State;

pub struct Network {
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

fn arg_u8(args: &HashMap<String, OwnedValue>, key: &str) -> fdo::Result<u8> {
    let value = args
        .get(key)
        .ok_or_else(|| fdo::Error::InvalidArgs(format!("missing {key}")))?;
    match &**value {
        Value::U8(number) => Ok(*number),
        Value::U32(number) => u8::try_from(*number).map_err(|_| fdo::Error::InvalidArgs(format!("bad {key}"))),
        Value::I32(number) if *number >= 0 => Ok(*number as u8),
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

fn ssid_for_path(snapshot: &serde_json::Value, path: &str) -> Option<String> {
    snapshot
        .get("access-points")
        .and_then(|value| value.as_array())
        .and_then(|list| {
            list.iter().find(|entry| entry.get("path").and_then(|v| v.as_str()) == Some(path))
        })
        .and_then(|entry| entry.get("ssid").and_then(|v| v.as_str()))
        .map(str::to_string)
}

fn parse_enterprise(args: &HashMap<String, OwnedValue>) -> fdo::Result<EnterpriseCredentials> {
    let eap = arg_u8(args, "eap")?;
    let phase2 = arg_u8(args, "phase2")?;
    if eap > 1 || phase2 > 3 {
        return Err(fdo::Error::InvalidArgs(
            "Unknown enterprise authentication method".into(),
        ));
    }
    Ok(EnterpriseCredentials {
        identity: arg_str(args, "identity")?,
        anonymous_identity: args
            .get("anonymous")
            .map(|_| arg_str(args, "anonymous"))
            .transpose()?
            .unwrap_or_default(),
        password: arg_str(args, "secret")?,
        ca_cert_path: args
            .get("certificate")
            .map(|_| arg_str(args, "certificate"))
            .transpose()?
            .unwrap_or_default(),
        domain_suffix_match: args
            .get("domain")
            .map(|_| arg_str(args, "domain"))
            .transpose()?
            .unwrap_or_default(),
        eap,
        phase2,
    })
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
        #[zbus(header)] header: Header<'_>,
        action: &str,
        args: HashMap<String, OwnedValue>,
    ) -> fdo::Result<u64> {
        let sender = sender_name(&header)?;
        let claim_connect = action == "connect" || action == "enterprise";
        if claim_connect {
            let state = self.state.lock().await;
            if state.agents.network_owner().is_some() {
                return Err(fdo::Error::Failed("Another client is connecting".into()));
            }
            state.agents.set_network_owner(sender.clone());
        }

        let state_ref = Arc::clone(&self.state);
        let action_name = action.to_string();
        let plan = tokio::task::spawn_blocking(move || {
            let mut state = state_ref.blocking_lock();
            let operation = state.alloc_operation();
            let snapshot = state.network.snapshot();
            let result: fdo::Result<ActionPlan> = match action_name.as_str() {
                "enable" => {
                    let enabled = arg_bool(&args, "value")?;
                    state.network.set_enabled(enabled);
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::NetworkWireless { enabled },
                        cancel: None,
                    })
                }
                "scan" => {
                    state.network.scan();
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::NetworkScanIdle,
                        cancel: None,
                    })
                }
                "connect" => {
                    let path = arg_str(&args, "path")?;
                    let ssid = ssid_for_path(&snapshot, &path)
                        .ok_or_else(|| fdo::Error::Failed("Network is no longer available".into()))?;
                    let secret = match args.get("secret") {
                        Some(_) => Some(arg_str(&args, "secret")?),
                        None => None,
                    };
                    state
                        .network
                        .connect(&path, secret.as_deref())
                        .map_err(fdo::Error::Failed)?;
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::NetworkConnected { ssid },
                        cancel: Some(OperationCancel::NetworkAuth),
                    })
                }
                "enterprise" => {
                    let path = arg_str(&args, "path")?;
                    let ssid = ssid_for_path(&snapshot, &path)
                        .ok_or_else(|| fdo::Error::Failed("Network is no longer available".into()))?;
                    let credentials = parse_enterprise(&args)?;
                    state
                        .network
                        .connect_enterprise(&path, &credentials)
                        .map_err(fdo::Error::Failed)?;
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::NetworkConnected { ssid },
                        cancel: Some(OperationCancel::NetworkAuth),
                    })
                }
                "disconnect" => {
                    state.agents.cancel_nm_secret("disconnect");
                    state
                        .network
                        .disconnect()
                        .map_err(fdo::Error::Failed)?;
                    state.agents.clear_network_owner();
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::NetworkDisconnected,
                        cancel: None,
                    })
                }
                "forget" => {
                    let ssid = arg_str(&args, "ssid")?;
                    state.agents.cancel_nm_secret("forget");
                    state.network.forget(&ssid).map_err(fdo::Error::Failed)?;
                    state.agents.clear_network_owner();
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::NetworkForgotten { ssid },
                        cancel: None,
                    })
                }
                "vpn-connect" => {
                    let path = arg_str(&args, "path")?;
                    state
                        .network
                        .set_vpn(&path, true)
                        .map_err(fdo::Error::Failed)?;
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::NetworkVpn {
                            path,
                            active: true,
                        },
                        cancel: None,
                    })
                }
                "vpn-disconnect" => {
                    let path = arg_str(&args, "path")?;
                    state
                        .network
                        .set_vpn(&path, false)
                        .map_err(fdo::Error::Failed)?;
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::NetworkVpn {
                            path,
                            active: false,
                        },
                        cancel: None,
                    })
                }
                "wired" => {
                    state
                        .network
                        .activate_wired()
                        .map_err(fdo::Error::Failed)?;
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::NetworkWired,
                        cancel: None,
                    })
                }
                "cellular-connect" => {
                    state
                        .network
                        .activate_cellular()
                        .map_err(fdo::Error::Failed)?;
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::NetworkCellular { active: true },
                        cancel: None,
                    })
                }
                "cellular-disconnect" => {
                    state
                        .network
                        .deactivate_cellular()
                        .map_err(fdo::Error::Failed)?;
                    Ok(ActionPlan {
                        operation,
                        wait: OperationWait::NetworkCellular { active: false },
                        cancel: None,
                    })
                }
                other => Err(fdo::Error::Failed(format!("Unknown network action {other}"))),
            };
            if result.is_err() && claim_connect {
                state.agents.cancel_nm_secret("connect failed");
                state.agents.clear_network_owner();
            }
            result
        })
        .await
        .map_err(|error| fdo::Error::Failed(error.to_string()))??;

        let finished_now = {
            let mut locked = self.state.lock().await;
            locked.push_operation(
                plan.operation,
                NETWORK_INTERFACE,
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
                    interface: NETWORK_INTERFACE,
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
            Self::completed(&emitter, plan.operation, true, "").await.ok();
        }
        Ok(plan.operation)
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
        args: HashMap<String, OwnedValue>,
    ) -> fdo::Result<HashMap<String, OwnedValue>> {
        let parsed = parse_dns_args(&args)?;
        let state_ref = Arc::clone(&self.state);
        let result = tokio::task::spawn_blocking(move || {
            let mut state = state_ref.blocking_lock();
            state.network.set_dns(parsed)
        })
        .await
        .map_err(|error| fdo::Error::Failed(error.to_string()))?;
        let (success, message) = match result {
            Ok(()) => (true, String::new()),
            Err(error) => (
                false,
                if error.contains("Connect to a network") {
                    error
                } else {
                    "DNS could not be changed. Check the active connection and try again.".into()
                },
            ),
        };
        if success {
            Self::changed(&emitter).await.ok();
        }
        let mut map = HashMap::new();
        map.insert("success".to_string(), owned_bool(success));
        map.insert("message".to_string(), owned_str(&message));
        Ok(map)
    }

    async fn reply(
        &self,
        #[zbus(header)] header: Header<'_>,
        secret: &str,
        cancel: bool,
    ) -> fdo::Result<()> {
        let sender = sender_name(&header)?;
        let mut state = self.state.lock().await;
        if state.agents.network_owner().as_deref() != Some(sender.as_str()) {
            return Err(fdo::Error::InvalidArgs(
                "No credential request for this client".into(),
            ));
        }
        if !state.agents.nm_has_pending() {
            return Err(fdo::Error::InvalidArgs(
                "No credential request for this client".into(),
            ));
        }
        state
            .agents
            .submit_network_reply(secret, cancel)
            .map_err(|message| fdo::Error::InvalidArgs(message.into()))?;
        if cancel {
            let cancelled = crate::desktop_operations::cancel_auth_operations(
                &mut state,
                NETWORK_INTERFACE,
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
                            NETWORK_INTERFACE,
                            "Completed",
                            &(id, false, "Cancelled"),
                        )
                        .await;
                }
            }
        }
        Ok(())
    }

    async fn cancel(
        &self,
        #[zbus(header)] header: Header<'_>,
        operation: u64,
    ) -> fdo::Result<()> {
        let sender = sender_name(&header)?;
        let mut state = self.state.lock().await;
        state
            .cancel_operation(NETWORK_INTERFACE, &sender, operation)
            .map_err(|message| fdo::Error::InvalidArgs(message.into()))?;
        let connection = state
            .bus
            .clone()
            .ok_or_else(|| fdo::Error::Failed("session bus unavailable".into()))?;
        let _ = connection
            .emit_signal(
                Some(sender.as_str()),
                crate::desktop_operations::OBJECT_PATH,
                NETWORK_INTERFACE,
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
