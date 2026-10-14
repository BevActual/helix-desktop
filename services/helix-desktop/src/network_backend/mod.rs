//! Network1 backend: NetworkManager on the system bus when available, otherwise in-memory mock.
//!
//! Nested harnesses set `HELIX_DESKTOP_MOCK_NETWORK=1` for deterministic smoke tests.

mod dns;
mod enterprise;
mod mock;
mod nm;

pub use dns::parse_dns_args;
pub use enterprise::EnterpriseCredentials;

use serde_json::{json, Value};

#[derive(Clone, Debug)]
pub struct AccessPoint {
    pub path: String,
    pub device_path: String,
    pub ssid: String,
    pub strength: u8,
    pub secured: bool,
    pub enterprise: bool,
    pub key_management: u32,
    pub active: bool,
    pub frequency: u32,
    pub saved: bool,
}

#[derive(Clone, Debug)]
pub struct Vpn {
    pub path: String,
    pub name: String,
    pub active: bool,
}

#[derive(Clone, Debug)]
pub struct NetworkFields {
    pub available: bool,
    pub enabled: bool,
    pub connected: bool,
    pub resolving: bool,
    pub scanning: bool,
    pub ssid: String,
    pub signal: u8,
    pub address: String,
    pub interface: String,
    pub kind: u32,
    pub cellular_active: bool,
    pub wired_available: bool,
    pub cellular_available: bool,
}

fn snapshot_from_parts(fields: &NetworkFields, access_points: &[AccessPoint], vpns: &[Vpn]) -> Value {
    json!({
        "available": fields.available,
        "enabled": fields.enabled,
        "connected": fields.connected,
        "resolving": fields.resolving,
        "scanning": fields.scanning,
        "ssid": fields.ssid,
        "signal": fields.signal,
        "address": fields.address,
        "interface": fields.interface,
        "kind": fields.kind,
        "cellular-active": fields.cellular_active,
        "wired": fields.wired_available,
        "cellular": fields.cellular_available,
        "access-points": access_points.iter().map(|ap| json!({
            "path": ap.path,
            "device": ap.device_path,
            "ssid": ap.ssid,
            "strength": ap.strength,
            "secured": ap.secured,
            "enterprise": ap.enterprise,
            "key-management": ap.key_management,
            "active": ap.active,
            "frequency": ap.frequency,
            "saved": ap.saved,
        })).collect::<Vec<_>>(),
        "vpns": vpns.iter().map(|vpn| json!({
            "path": vpn.path,
            "name": vpn.name,
            "active": vpn.active,
        })).collect::<Vec<_>>(),
    })
}

#[derive(Clone, Debug)]
enum Inner {
    Mock(mock::MockBackend),
    Nm(nm::NmBackend),
}

#[derive(Clone, Debug)]
pub struct NetworkBackend {
    inner: Inner,
}

impl NetworkBackend {
    pub fn new() -> Self {
        let inner = if mock::force_mock() {
            Inner::Mock(mock::MockBackend::new())
        } else if nm::probe() {
            Inner::Nm(nm::NmBackend::new())
        } else {
            tracing::info!("NetworkManager unavailable; using mock network backend");
            Inner::Mock(mock::MockBackend::new())
        };
        Self { inner }
    }

    pub fn is_available(&self) -> bool {
        match &self.inner {
            Inner::Mock(backend) => backend.is_available(),
            Inner::Nm(backend) => backend.is_available(),
        }
    }

    pub fn uses_network_manager(&self) -> bool {
        matches!(&self.inner, Inner::Nm(_))
    }

    pub fn refresh(&mut self) -> bool {
        match &mut self.inner {
            Inner::Mock(_) => false,
            Inner::Nm(backend) => backend.refresh(),
        }
    }

    pub fn snapshot(&self) -> Value {
        match &self.inner {
            Inner::Mock(backend) => backend.snapshot(),
            Inner::Nm(backend) => backend.snapshot(),
        }
    }

    pub fn dns_snapshot(&self) -> Value {
        match &self.inner {
            Inner::Mock(backend) => backend.dns_snapshot(),
            Inner::Nm(backend) => backend.dns_snapshot(),
        }
    }

    pub fn wifi_secret(&self) -> &str {
        match &self.inner {
            Inner::Mock(backend) => backend.wifi_secret(),
            Inner::Nm(backend) => backend.wifi_secret(),
        }
    }

    pub fn set_enabled(&mut self, enabled: bool) {
        match &mut self.inner {
            Inner::Mock(backend) => backend.set_enabled(enabled),
            Inner::Nm(backend) => backend.set_enabled(enabled),
        }
    }

    pub fn scan(&mut self) {
        match &mut self.inner {
            Inner::Mock(backend) => backend.scan(),
            Inner::Nm(backend) => backend.scan(),
        }
    }

    pub fn connect(&mut self, path: &str, secret: Option<&str>) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.connect(path, secret),
            Inner::Nm(backend) => backend.connect(path, secret),
        }
    }

    pub fn disconnect(&mut self) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.disconnect(),
            Inner::Nm(backend) => backend.disconnect(),
        }
    }

    pub fn forget(&mut self, ssid: &str) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.forget(ssid),
            Inner::Nm(backend) => backend.forget(ssid),
        }
    }

    pub fn set_vpn(&mut self, path: &str, active: bool) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.set_vpn(path, active),
            Inner::Nm(backend) => backend.set_vpn(path, active),
        }
    }

    pub fn connect_enterprise(
        &mut self,
        path: &str,
        credentials: &EnterpriseCredentials,
    ) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.connect_enterprise(path, credentials),
            Inner::Nm(backend) => backend.connect_enterprise(path, credentials),
        }
    }

    pub fn set_dns(&mut self, dns_override: dns::DnsOverride) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.set_dns(dns_override),
            Inner::Nm(backend) => backend.set_dns(dns_override),
        }
    }

    pub fn activate_wired(&mut self) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.activate_wired(),
            Inner::Nm(backend) => backend.activate_wired(),
        }
    }

    pub fn activate_cellular(&mut self) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.activate_cellular(),
            Inner::Nm(backend) => backend.activate_cellular(),
        }
    }

    pub fn deactivate_cellular(&mut self) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.deactivate_cellular(),
            Inner::Nm(backend) => backend.deactivate_cellular(),
        }
    }
}
