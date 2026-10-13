//! In-memory Bluetooth1 backend for nested / offline sessions.

use serde_json::Value;

use super::{snapshot_from_parts, BluetoothFields, Device};

#[derive(Clone, Debug)]
pub struct MockBackend {
    fields: BluetoothFields,
    devices: Vec<Device>,
}

pub fn force_mock() -> bool {
    matches!(
        std::env::var("HELIX_DESKTOP_MOCK_BLUETOOTH").ok().as_deref(),
        Some("1") | Some("true") | Some("yes")
    )
}

impl MockBackend {
    pub fn new() -> Self {
        Self {
            fields: BluetoothFields {
                available: true,
                enabled: true,
                scanning: false,
                name: "Helix".into(),
                discoverable: false,
                pairable: true,
                soft_blocked: false,
                hard_blocked: false,
            },
            devices: vec![
                Device {
                    path: "/org/helix/mock/bt/1".into(),
                    address: "AA:BB:CC:11:22:33".into(),
                    name: "Helix Earbuds".into(),
                    kind: 1,
                    paired: true,
                    trusted: true,
                    connected: true,
                    connecting: false,
                    has_battery: true,
                    battery: 72,
                    has_rssi: false,
                    rssi: 0,
                },
                Device {
                    path: "/org/helix/mock/bt/2".into(),
                    address: "AA:BB:CC:44:55:66".into(),
                    name: "Phone".into(),
                    kind: 1,
                    paired: true,
                    trusted: true,
                    connected: false,
                    connecting: false,
                    has_battery: false,
                    battery: 0,
                    has_rssi: false,
                    rssi: 0,
                },
                Device {
                    path: "/org/helix/mock/bt/3".into(),
                    address: "AA:BB:CC:77:88:99".into(),
                    name: "Nearby Speaker".into(),
                    kind: 1,
                    paired: false,
                    trusted: false,
                    connected: false,
                    connecting: false,
                    has_battery: false,
                    battery: 0,
                    has_rssi: false,
                    rssi: 0,
                },
            ],
        }
    }

    pub fn is_available(&self) -> bool {
        true
    }

    pub fn snapshot(&self) -> Value {
        snapshot_from_parts(&self.fields, &self.devices)
    }

    pub fn set_enabled(&mut self, enabled: bool) -> Result<(), String> {
        if enabled && self.fields.hard_blocked {
            return Err("Bluetooth is blocked by a hardware switch".into());
        }
        self.fields.enabled = enabled;
        if !enabled {
            self.fields.scanning = false;
            for device in &mut self.devices {
                device.connected = false;
                device.connecting = false;
            }
        }
        Ok(())
    }

    pub fn set_scanning(&mut self, on: bool) -> Result<(), String> {
        if !self.fields.enabled {
            return Err("Bluetooth is off".into());
        }
        self.fields.scanning = on;
        Ok(())
    }

    fn device_mut(&mut self, path: &str) -> Result<&mut Device, String> {
        self.devices
            .iter_mut()
            .find(|device| device.path == path)
            .ok_or_else(|| "Device is no longer available".into())
    }

    pub fn connect(&mut self, path: &str) -> Result<(), String> {
        if !self.fields.enabled {
            return Err("Bluetooth is off".into());
        }
        let device = self.device_mut(path)?;
        if !device.paired {
            return Err("Device is not paired".into());
        }
        if device.connected {
            return Err("Already connected".into());
        }
        device.connecting = false;
        device.connected = true;
        Ok(())
    }

    pub fn disconnect(&mut self, path: &str) -> Result<(), String> {
        if !self.fields.enabled {
            return Err("Bluetooth is off".into());
        }
        let device = self.device_mut(path)?;
        if !device.connected {
            return Err("Not connected".into());
        }
        device.connected = false;
        device.connecting = false;
        Ok(())
    }

    pub fn pair(&mut self, path: &str) -> Result<(), String> {
        if !self.fields.enabled {
            return Err("Bluetooth is off".into());
        }
        let device = self.device_mut(path)?;
        if device.paired {
            return Err("Already paired".into());
        }
        device.paired = true;
        device.trusted = true;
        Ok(())
    }

    pub fn set_trusted(&mut self, path: &str, trusted: bool) -> Result<(), String> {
        let device = self.device_mut(path)?;
        device.trusted = trusted;
        Ok(())
    }

    pub fn cancel_pair(&mut self, _path: &str) -> Result<(), String> {
        Ok(())
    }

    pub fn forget(&mut self, path: &str) -> Result<(), String> {
        let Some(index) = self.devices.iter().position(|device| device.path == path) else {
            return Err("Device is no longer available".into());
        };
        if !self.devices[index].paired {
            return Err("Device is not paired".into());
        }
        self.devices.remove(index);
        Ok(())
    }
}
