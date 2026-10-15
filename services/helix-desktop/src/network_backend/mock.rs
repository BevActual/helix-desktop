//! In-memory Network1 backend for nested / offline sessions.

use serde_json::{json, Value};

use super::dns::DnsOverride;
use super::enterprise::{EnterpriseCredentials, Validation};
use super::{AccessPoint, NetworkFields, Vpn};

#[derive(Clone, Debug)]
pub struct MockBackend {
    fields: NetworkFields,
    access_points: Vec<AccessPoint>,
    vpns: Vec<Vpn>,
    wifi_secret: String,
    dns: Value,
}

pub fn force_mock() -> bool {
    matches!(
        std::env::var("HELIX_DESKTOP_MOCK_NETWORK").ok().as_deref(),
        Some("1") | Some("true") | Some("yes")
    )
}

impl MockBackend {
    pub fn new() -> Self {
        Self {
            fields: NetworkFields {
                available: true,
                enabled: true,
                connected: true,
                resolving: false,
                scanning: false,
                ssid: "Helix Lab".into(),
                signal: 78,
                address: "10.0.0.42".into(),
                interface: "wlan0".into(),
                kind: 1,
                cellular_active: false,
                wired_available: true,
                cellular_available: false,
            },
            access_points: vec![
                AccessPoint {
                    path: "/org/helix/mock/ap/1".into(),
                    device_path: "/org/helix/mock/device/1".into(),
                    ssid: "Helix Lab".into(),
                    strength: 78,
                    secured: true,
                    enterprise: false,
                    key_management: 2,
                    active: true,
                    frequency: 5180,
                    saved: true,
                },
                AccessPoint {
                    path: "/org/helix/mock/ap/2".into(),
                    device_path: "/org/helix/mock/device/1".into(),
                    ssid: "Cafe Open".into(),
                    strength: 55,
                    secured: false,
                    enterprise: false,
                    key_management: 0,
                    active: false,
                    frequency: 5180,
                    saved: false,
                },
                AccessPoint {
                    path: "/org/helix/mock/ap/3".into(),
                    device_path: "/org/helix/mock/device/1".into(),
                    ssid: "Neighbour".into(),
                    strength: 40,
                    secured: true,
                    enterprise: false,
                    key_management: 2,
                    active: false,
                    frequency: 5180,
                    saved: false,
                },
            ],
            vpns: vec![Vpn {
                path: "/org/helix/mock/vpn/1".into(),
                name: "Work VPN".into(),
                active: false,
            }],
            wifi_secret: String::new(),
            dns: json!({
                "profile": 0,
                "servers": [],
                "nextdns": "",
            }),
        }
    }

    pub fn is_available(&self) -> bool {
        true
    }

    pub fn snapshot(&self) -> Value {
        super::snapshot_from_parts(&self.fields, &self.access_points, &self.vpns)
    }

    pub fn dns_snapshot(&self) -> Value {
        self.dns.clone()
    }

    pub fn wifi_secret(&self) -> &str {
        &self.wifi_secret
    }

    pub fn set_enabled(&mut self, enabled: bool) {
        self.fields.enabled = enabled;
        if !enabled {
            self.fields.connected = false;
            self.fields.ssid.clear();
            self.fields.signal = 0;
            for ap in &mut self.access_points {
                ap.active = false;
            }
        }
    }

    pub fn scan(&mut self) {
        self.fields.scanning = false;
    }

    pub fn connect(&mut self, path: &str, secret: Option<&str>) -> Result<(), String> {
        if !self.fields.enabled {
            return Err("Wi-Fi is off".into());
        }
        let Some(index) = self.access_points.iter().position(|ap| ap.path == path) else {
            return Err("Network is no longer available".into());
        };
        if self.access_points[index].enterprise {
            return Err("Enterprise networks need the enterprise action".into());
        }
        if self.access_points[index].secured && !self.access_points[index].saved {
            let Some(password) = secret.filter(|text| !text.is_empty()) else {
                return Err("Password required".into());
            };
            self.wifi_secret = password.to_string();
        } else if let Some(password) = secret.filter(|text| !text.is_empty()) {
            self.wifi_secret = password.to_string();
        }
        for (i, ap) in self.access_points.iter_mut().enumerate() {
            ap.active = i == index;
            if i == index {
                ap.saved = true;
            }
        }
        let ap = &self.access_points[index];
        self.fields.connected = true;
        self.fields.resolving = false;
        self.fields.ssid = ap.ssid.clone();
        self.fields.signal = ap.strength;
        Ok(())
    }

    pub fn disconnect(&mut self) -> Result<(), String> {
        if !self.fields.connected {
            return Err("Not connected".into());
        }
        self.fields.connected = false;
        self.fields.ssid.clear();
        self.fields.signal = 0;
        for ap in &mut self.access_points {
            ap.active = false;
        }
        Ok(())
    }

    pub fn forget(&mut self, ssid: &str) -> Result<(), String> {
        let Some(ap) = self.access_points.iter_mut().find(|ap| ap.ssid == ssid) else {
            return Err("Network is no longer available".into());
        };
        if !ap.saved {
            return Err("Network is not saved".into());
        }
        let was_active = ap.active;
        ap.saved = false;
        ap.active = false;
        if was_active {
            self.fields.connected = false;
            self.fields.ssid.clear();
            self.fields.signal = 0;
        }
        self.wifi_secret.clear();
        Ok(())
    }

    pub fn connect_enterprise(
        &mut self,
        path: &str,
        credentials: &EnterpriseCredentials,
    ) -> Result<(), String> {
        if super::enterprise::validate(credentials) != Validation::Ok {
            return Err("Invalid enterprise credentials".into());
        }
        let Some(index) = self.access_points.iter().position(|ap| ap.path == path) else {
            return Err("Network is no longer available".into());
        };
        if !self.access_points[index].enterprise {
            return Err("Not an enterprise network".into());
        }
        for (i, ap) in self.access_points.iter_mut().enumerate() {
            ap.active = i == index;
            if i == index {
                ap.saved = true;
            }
        }
        let ap = &self.access_points[index];
        self.fields.connected = true;
        self.fields.ssid = ap.ssid.clone();
        self.fields.signal = ap.strength;
        Ok(())
    }

    pub fn set_dns(&mut self, dns_override: DnsOverride) -> Result<(), String> {
        if !self.fields.connected {
            return Err("Connect to a network before changing DNS".into());
        }
        self.dns = json!({
            "profile": dns_override.profile,
            "servers": dns_override.servers,
            "nextdns": dns_override.nextdns_id,
        });
        Ok(())
    }

    pub fn activate_wired(&mut self) -> Result<(), String> {
        if !self.fields.wired_available {
            return Err("Connection rejected".into());
        }
        self.fields.connected = true;
        self.fields.kind = 2;
        self.fields.interface = "eth0".into();
        self.fields.address = "10.0.0.10".into();
        self.fields.ssid.clear();
        self.fields.signal = 0;
        Ok(())
    }

    pub fn activate_cellular(&mut self) -> Result<(), String> {
        if !self.fields.cellular_available {
            return Err("Connection rejected".into());
        }
        self.fields.cellular_active = true;
        self.fields.connected = true;
        self.fields.kind = 4;
        self.fields.interface = "wwan0".into();
        Ok(())
    }

    pub fn deactivate_cellular(&mut self) -> Result<(), String> {
        if !self.fields.cellular_active {
            return Err("Connection rejected".into());
        }
        self.fields.cellular_active = false;
        self.fields.connected = false;
        self.fields.kind = 0;
        Ok(())
    }

    pub fn set_vpn(&mut self, path: &str, active: bool) -> Result<(), String> {
        if !self.vpns.iter().any(|vpn| vpn.path == path) {
            return Err("VPN is no longer available".into());
        }
        for entry in &mut self.vpns {
            entry.active = entry.path == path && active;
        }
        Ok(())
    }
}
