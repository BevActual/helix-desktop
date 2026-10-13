//! In-memory Bluetooth1 backend for the Rust service scaffold (no BlueZ yet).

use serde_json::{json, Value};

#[derive(Clone, Debug)]
pub struct Device {
    pub path: String,
    pub address: String,
    pub name: String,
    pub kind: u32,
    pub paired: bool,
    pub trusted: bool,
    pub connected: bool,
    pub connecting: bool,
    pub battery: Option<u8>,
}

#[derive(Clone, Debug)]
pub struct BluetoothBackend {
    pub available: bool,
    pub enabled: bool,
    pub scanning: bool,
    pub name: String,
    pub discoverable: bool,
    pub pairable: bool,
    pub soft_blocked: bool,
    pub hard_blocked: bool,
    pub devices: Vec<Device>,
}

impl BluetoothBackend {
    pub fn new() -> Self {
        Self {
            available: true,
            enabled: true,
            scanning: false,
            name: "Helix".into(),
            discoverable: false,
            pairable: true,
            soft_blocked: false,
            hard_blocked: false,
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
                    battery: Some(72),
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
                    battery: None,
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
                    battery: None,
                },
            ],
        }
    }

    pub fn snapshot(&self) -> Value {
        json!({
            "available": self.available,
            "enabled": self.enabled,
            "scanning": self.scanning,
            "name": self.name,
            "discoverable": self.discoverable,
            "pairable": self.pairable,
            "soft-blocked": self.soft_blocked,
            "hard-blocked": self.hard_blocked,
            "devices": self.devices.iter().map(|device| json!({
                "path": device.path,
                "address": device.address,
                "name": device.name,
                "kind": device.kind,
                "paired": device.paired,
                "trusted": device.trusted,
                "connected": device.connected,
                "connecting": device.connecting,
                "has-battery": device.battery.is_some(),
                "battery": device.battery.unwrap_or(0),
                "has-rssi": false,
                "rssi": 0,
            })).collect::<Vec<_>>(),
        })
    }

    pub fn set_enabled(&mut self, enabled: bool) -> Result<(), String> {
        if enabled && self.hard_blocked {
            return Err("Bluetooth is blocked by a hardware switch".into());
        }
        self.enabled = enabled;
        if !enabled {
            self.scanning = false;
            for device in &mut self.devices {
                device.connected = false;
                device.connecting = false;
            }
        }
        Ok(())
    }

    pub fn set_scanning(&mut self, on: bool) -> Result<(), String> {
        if !self.enabled {
            return Err("Bluetooth is off".into());
        }
        self.scanning = on;
        Ok(())
    }

    fn device_mut(&mut self, path: &str) -> Result<&mut Device, String> {
        self.devices
            .iter_mut()
            .find(|device| device.path == path)
            .ok_or_else(|| "Device is no longer available".into())
    }

    pub fn connect(&mut self, path: &str) -> Result<(), String> {
        if !self.enabled {
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
        if !self.enabled {
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
        if !self.enabled {
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
