//! Long-running Network1 / Bluetooth1 actions and targeted `Completed` signals.

use std::time::{Duration, Instant};

use serde_json::Value;
use zbus::Connection;

use crate::state::State;

pub const NETWORK_INTERFACE: &str = "org.helix.Desktop.Network1";
pub const BLUETOOTH_INTERFACE: &str = "org.helix.Desktop.Bluetooth1";
pub const OBJECT_PATH: &str = "/org/helix/Desktop";

const OPERATION_TIMEOUT: Duration = Duration::from_secs(45);

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum OperationWait {
    Immediate,
    NetworkConnected { ssid: String },
    NetworkDisconnected,
    NetworkForgotten { ssid: String },
    NetworkScanIdle,
    NetworkWireless { enabled: bool },
    NetworkVpn { path: String, active: bool },
    NetworkWired,
    NetworkCellular { active: bool },
    BluetoothDiscovering { enabled: bool },
    BluetoothPaired { path: String },
    BluetoothConnected { path: String },
    BluetoothDisconnected { path: String },
    BluetoothForgotten { path: String },
    BluetoothCancelPair,
    BluetoothTrusted { path: String, trusted: bool },
}

#[derive(Clone, Debug)]
pub enum OperationCancel {
    NetworkAuth,
    BluetoothPair { path: String },
}

#[derive(Clone, Debug)]
pub struct DesktopOperation {
    pub id: u64,
    pub interface: &'static str,
    pub sender: String,
    pub action: String,
    pub wait: OperationWait,
    pub deadline: Instant,
    pub cancel: Option<OperationCancel>,
}

impl State {
    pub fn push_operation(
        &mut self,
        id: u64,
        interface: &'static str,
        sender: String,
        action: String,
        wait: OperationWait,
        cancel: Option<OperationCancel>,
    ) {
        let deadline = Instant::now() + OPERATION_TIMEOUT;
        self.desktop_operations.push(DesktopOperation {
            id,
            interface,
            sender,
            action,
            wait,
            deadline,
            cancel,
        });
    }

    pub fn cancel_operation(
        &mut self,
        interface: &str,
        sender: &str,
        id: u64,
    ) -> Result<(), &'static str> {
        let index = self
            .desktop_operations
            .iter()
            .position(|op| op.id == id && op.interface == interface && op.sender == sender)
            .ok_or("Operation is no longer available to this client")?;
        let cancel = self.desktop_operations[index]
            .cancel
            .clone()
            .ok_or("This operation cannot be cancelled")?;
        self.run_cancel(&cancel);
        self.desktop_operations.remove(index);
        Ok(())
    }

    fn run_cancel(&mut self, cancel: &OperationCancel) {
        match cancel {
            OperationCancel::NetworkAuth => {
                self.agents.cancel_nm_secret("cancelled");
                let _ = self.network.disconnect();
                self.agents.clear_network_owner();
            }
            OperationCancel::BluetoothPair { path } => {
                let _ = self.bluetooth.cancel_pair(path);
                self.agents.cancel_bluetooth_pending();
                self.agents.clear_bluetooth_owner();
            }
        }
    }

    pub fn operation_done(&self, wait: &OperationWait) -> bool {
        match wait {
            OperationWait::Immediate => true,
            OperationWait::NetworkConnected { ssid } => {
                let snap = self.network.snapshot();
                snap.get("connected").and_then(Value::as_bool) == Some(true)
                    && snap.get("ssid").and_then(Value::as_str) == Some(ssid.as_str())
            }
            OperationWait::NetworkDisconnected => {
                let snap = self.network.snapshot();
                snap.get("connected").and_then(Value::as_bool) == Some(false)
                    && snap.get("resolving").and_then(Value::as_bool) != Some(true)
            }
            OperationWait::NetworkForgotten { ssid } => !self
                .network
                .snapshot()
                .get("access-points")
                .and_then(Value::as_array)
                .is_some_and(|list| {
                    list.iter().any(|entry| {
                        entry.get("ssid").and_then(Value::as_str) == Some(ssid.as_str())
                            && entry.get("saved").and_then(Value::as_bool) == Some(true)
                    })
                }),
            OperationWait::NetworkScanIdle => {
                self.network
                    .snapshot()
                    .get("scanning")
                    .and_then(Value::as_bool)
                    == Some(false)
            }
            OperationWait::NetworkWireless { enabled } => {
                self.network
                    .snapshot()
                    .get("enabled")
                    .and_then(Value::as_bool)
                    == Some(*enabled)
            }
            OperationWait::NetworkWired => {
                let snap = self.network.snapshot();
                snap.get("kind").and_then(|v| v.as_u64()) == Some(2)
                    && snap.get("connected").and_then(Value::as_bool) == Some(true)
            }
            OperationWait::NetworkCellular { active } => self
                .network
                .snapshot()
                .get("cellular-active")
                .and_then(Value::as_bool)
                == Some(*active),
            OperationWait::NetworkVpn { path, active } => self
                .network
                .snapshot()
                .get("vpns")
                .and_then(Value::as_array)
                .and_then(|list| {
                    list.iter().find(|entry| entry.get("path").and_then(Value::as_str) == Some(path))
                })
                .and_then(|entry| entry.get("active").and_then(Value::as_bool))
                == Some(*active),
            OperationWait::BluetoothDiscovering { enabled } => {
                self.bluetooth
                    .snapshot()
                    .get("scanning")
                    .and_then(Value::as_bool)
                    == Some(*enabled)
            }
            OperationWait::BluetoothPaired { path } => bluetooth_device(self, path, |device| {
                device.get("paired").and_then(Value::as_bool) == Some(true)
            }),
            OperationWait::BluetoothConnected { path } => bluetooth_device(self, path, |device| {
                device.get("connected").and_then(Value::as_bool) == Some(true)
            }),
            OperationWait::BluetoothDisconnected { path } => bluetooth_device(self, path, |device| {
                device.get("connected").and_then(Value::as_bool) == Some(false)
            }),
            OperationWait::BluetoothForgotten { path } => !bluetooth_has_path(self, path),
            OperationWait::BluetoothCancelPair => !self.agents.bt_has_pending(),
            OperationWait::BluetoothTrusted { path, trusted } => {
                bluetooth_device(self, path, |device| {
                    device.get("trusted").and_then(Value::as_bool) == Some(*trusted)
                })
            }
        }
    }

    pub fn finish_operation_side_effects(&mut self, op: &DesktopOperation, success: bool) {
        if !success {
            return;
        }
        if op.interface == NETWORK_INTERFACE
            && self.agents.network_owner().as_deref() == Some(op.sender.as_str())
            && (op.action == "connect" || op.action == "enterprise")
        {
            self.agents.cancel_nm_secret("connect finished");
            self.agents.clear_network_owner();
        }
        if op.interface == BLUETOOTH_INTERFACE
            && self.agents.bluetooth_owner().as_deref() == Some(op.sender.as_str())
            && op.action == "pair"
        {
            self.agents.cancel_bluetooth_pending();
            self.agents.clear_bluetooth_owner();
        }
    }
}

fn bluetooth_has_path(state: &State, path: &str) -> bool {
    state
        .bluetooth
        .snapshot()
        .get("devices")
        .and_then(Value::as_array)
        .is_some_and(|list| {
            list.iter()
                .any(|entry| entry.get("path").and_then(Value::as_str) == Some(path))
        })
}

fn bluetooth_device(
    state: &State,
    path: &str,
    predicate: impl FnOnce(&Value) -> bool,
) -> bool {
    state
        .bluetooth
        .snapshot()
        .get("devices")
        .and_then(Value::as_array)
        .and_then(|list| {
            list.iter().find(|entry| entry.get("path").and_then(Value::as_str) == Some(path))
        })
        .is_some_and(predicate)
}

pub async fn poll_operations(state: &mut State, connection: &Connection) {
    let now = Instant::now();
    let mut finished: Vec<(DesktopOperation, bool, String)> = Vec::new();
    let pending = state.desktop_operations.clone();
    state.desktop_operations.clear();
    for op in pending {
        let success = state.operation_done(&op.wait);
        if success {
            finished.push((op, true, String::new()));
            continue;
        }
        if now >= op.deadline {
            finished.push((op, false, "Operation timed out".into()));
            continue;
        }
        state.desktop_operations.push(op);
    }
    for (op, ok, message) in finished {
        state.finish_operation_side_effects(&op, ok);
        let _ = connection
            .emit_signal(
                Some(op.sender.as_str()),
                OBJECT_PATH,
                op.interface,
                "Completed",
                &(op.id, ok, message.as_str()),
            )
            .await;
    }
}

/// Fails every pending operation of `interface` (`failOperations`): its backend daemon went away
/// or was replaced, so none of them can complete.
pub async fn fail_interface_operations(state: &mut State, connection: &Connection, interface: &str) {
    let (failed, kept): (Vec<_>, Vec<_>) = std::mem::take(&mut state.desktop_operations)
        .into_iter()
        .partition(|op| op.interface == interface);
    state.desktop_operations = kept;
    for op in failed {
        if interface == NETWORK_INTERFACE
            && state.agents.network_owner().as_deref() == Some(op.sender.as_str())
        {
            state.agents.cancel_nm_secret("service disconnected");
            state.agents.clear_network_owner();
        }
        if interface == BLUETOOTH_INTERFACE
            && state.agents.bluetooth_owner().as_deref() == Some(op.sender.as_str())
        {
            state.agents.cancel_bluetooth_pending();
            state.agents.clear_bluetooth_owner();
        }
        let _ = connection
            .emit_signal(
                Some(op.sender.as_str()),
                OBJECT_PATH,
                op.interface,
                "Completed",
                &(op.id, false, "Device service disconnected"),
            )
            .await;
    }
}

pub fn clear_operations_for_peer(state: &mut State, unique_name: &str) {
    state
        .desktop_operations
        .retain(|op| op.sender != unique_name);
}

/// When the user cancels a credential/pairing prompt, cancel matching auth operations (C++ `Reply` + cancel).
pub fn cancel_auth_operations(
    state: &mut State,
    interface: &str,
    sender: &str,
) -> Vec<u64> {
    let mut cancelled = Vec::new();
    let pending = state.desktop_operations.clone();
    state.desktop_operations.clear();
    for op in pending {
        let authentication =
            op.action == "pair" || op.action == "connect" || op.action == "enterprise";
        if op.interface == interface && op.sender == sender && authentication && op.cancel.is_some() {
            if let Some(cancel) = &op.cancel {
                state.run_cancel(cancel);
            }
            cancelled.push(op.id);
            continue;
        }
        state.desktop_operations.push(op);
    }
    cancelled
}
