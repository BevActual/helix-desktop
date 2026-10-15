//! Bluetooth1 backend: BlueZ on the system bus when available, otherwise in-memory mock.

mod bluez;
mod mock;

use serde_json::Value;

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
    pub has_battery: bool,
    pub battery: u8,
    pub has_rssi: bool,
    pub rssi: i16,
}

#[derive(Clone, Debug)]
pub struct BluetoothFields {
    pub available: bool,
    pub enabled: bool,
    pub scanning: bool,
    pub name: String,
    pub discoverable: bool,
    pub pairable: bool,
    pub soft_blocked: bool,
    pub hard_blocked: bool,
}

pub(crate) fn snapshot_from_parts(fields: &BluetoothFields, devices: &[Device]) -> Value {
    serde_json::json!({
        "available": fields.available,
        "enabled": fields.enabled,
        "scanning": fields.scanning,
        "name": fields.name,
        "discoverable": fields.discoverable,
        "pairable": fields.pairable,
        "soft-blocked": fields.soft_blocked,
        "hard-blocked": fields.hard_blocked,
        "devices": devices.iter().map(|device| serde_json::json!({
            "path": device.path,
            "address": device.address,
            "name": device.name,
            "kind": device.kind,
            "paired": device.paired,
            "trusted": device.trusted,
            "connected": device.connected,
            "connecting": device.connecting,
            "has-battery": device.has_battery,
            "battery": device.battery,
            "has-rssi": device.has_rssi,
            "rssi": device.rssi,
        })).collect::<Vec<_>>(),
    })
}

#[derive(Clone, Debug)]
enum Inner {
    Mock(mock::MockBackend),
    Bluez(bluez::BluezBackend),
}

#[derive(Clone, Debug)]
pub struct BluetoothBackend {
    inner: Inner,
}

impl BluetoothBackend {
    pub fn new() -> Self {
        // Without BlueZ the real backend reports an unavailable snapshot and attaches on a
        // later refresh; only `HELIX_DESKTOP_MOCK_BLUETOOTH` selects the in-memory mock.
        let inner = if mock::force_mock() {
            Inner::Mock(mock::MockBackend::new())
        } else {
            if !bluez::probe() {
                tracing::info!(
                    "BlueZ not on the system bus yet; bluetooth unavailable until it appears"
                );
            }
            Inner::Bluez(bluez::BluezBackend::new())
        };
        Self { inner }
    }

    pub fn is_available(&self) -> bool {
        match &self.inner {
            Inner::Mock(backend) => backend.is_available(),
            Inner::Bluez(backend) => backend.is_available(),
        }
    }

    /// Unique bus name of the BlueZ daemon (`None` when absent or mocked).
    pub fn bus_owner(&self) -> Option<String> {
        match &self.inner {
            Inner::Mock(_) => None,
            Inner::Bluez(_) => bluez::bus_owner(),
        }
    }

    pub fn uses_bluez(&self) -> bool {
        matches!(&self.inner, Inner::Bluez(_))
    }

    pub fn refresh(&mut self) -> bool {
        match &mut self.inner {
            Inner::Mock(_) => false,
            Inner::Bluez(backend) => backend.refresh(),
        }
    }

    pub fn snapshot(&self) -> Value {
        match &self.inner {
            Inner::Mock(backend) => backend.snapshot(),
            Inner::Bluez(backend) => backend.snapshot(),
        }
    }

    pub fn set_enabled(&mut self, enabled: bool) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.set_enabled(enabled),
            Inner::Bluez(backend) => backend.set_enabled(enabled),
        }
    }

    pub fn set_scanning(&mut self, on: bool) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.set_scanning(on),
            Inner::Bluez(backend) => backend.set_scanning(on),
        }
    }

    pub fn connect(&mut self, path: &str) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.connect(path),
            Inner::Bluez(backend) => backend.connect(path),
        }
    }

    pub fn disconnect(&mut self, path: &str) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.disconnect(path),
            Inner::Bluez(backend) => backend.disconnect(path),
        }
    }

    pub fn pair(&mut self, path: &str) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.pair(path),
            Inner::Bluez(backend) => backend.pair(path),
        }
    }

    pub fn forget(&mut self, path: &str) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.forget(path),
            Inner::Bluez(backend) => backend.forget(path),
        }
    }

    pub fn set_trusted(&mut self, path: &str, trusted: bool) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.set_trusted(path, trusted),
            Inner::Bluez(backend) => backend.set_trusted(path, trusted),
        }
    }

    pub fn cancel_pair(&mut self, path: &str) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.cancel_pair(path),
            Inner::Bluez(backend) => backend.cancel_pair(path),
        }
    }
}
