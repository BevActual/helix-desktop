//! In-memory Network1 backend for the Rust service scaffold (no NetworkManager yet).
//! Nested / offline sessions get a mock Wi-Fi radio + a few access points so the
//! control-center network page can render against `helix-desktop`.

use serde_json::{json, Value};

#[derive(Clone, Debug)]
pub struct AccessPoint {
    pub path: String,
    pub ssid: String,
    pub strength: u8,
    pub secured: bool,
    pub enterprise: bool,
    pub active: bool,
    pub saved: bool,
}

#[derive(Clone, Debug)]
pub struct Vpn {
    pub path: String,
    pub name: String,
    pub active: bool,
}

#[derive(Clone, Debug)]
pub struct NetworkBackend {
    pub available: bool,
    pub enabled: bool,
    pub connected: bool,
    pub resolving: bool,
    pub scanning: bool,
    pub ssid: String,
    pub signal: u8,
    pub address: String,
    pub interface: String,
    /// 0 = unknown, 1 = wifi (matches C++ kind for wifi primary).
    pub kind: u32,
    pub access_points: Vec<AccessPoint>,
    pub vpns: Vec<Vpn>,
    /// Last PSK for GetWifiSecret (scaffold only; never logged).
    wifi_secret: String,
}

impl NetworkBackend {
    pub fn new() -> Self {
        Self {
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
            access_points: vec![
                AccessPoint {
                    path: "/org/helix/mock/ap/1".into(),
                    ssid: "Helix Lab".into(),
                    strength: 78,
                    secured: true,
                    enterprise: false,
                    active: true,
                    saved: true,
                },
                AccessPoint {
                    path: "/org/helix/mock/ap/2".into(),
                    ssid: "Cafe Open".into(),
                    strength: 55,
                    secured: false,
                    enterprise: false,
                    active: false,
                    saved: false,
                },
                AccessPoint {
                    path: "/org/helix/mock/ap/3".into(),
                    ssid: "Neighbour".into(),
                    strength: 40,
                    secured: true,
                    enterprise: false,
                    active: false,
                    saved: false,
                },
            ],
            vpns: vec![Vpn {
                path: "/org/helix/mock/vpn/1".into(),
                name: "Work VPN".into(),
                active: false,
            }],
            wifi_secret: String::new(),
        }
    }

    pub fn snapshot(&self) -> Value {
        json!({
            "available": self.available,
            "enabled": self.enabled,
            "connected": self.connected,
            "resolving": self.resolving,
            "scanning": self.scanning,
            "ssid": self.ssid,
            "signal": self.signal,
            "address": self.address,
            "interface": self.interface,
            "kind": self.kind,
            "access-points": self.access_points.iter().map(|ap| json!({
                "path": ap.path,
                "device": "/org/helix/mock/device/1",
                "ssid": ap.ssid,
                "strength": ap.strength,
                "secured": ap.secured,
                "enterprise": ap.enterprise,
                "key-management": if ap.secured { 2 } else { 0 },
                "active": ap.active,
                "frequency": 5180,
                "saved": ap.saved,
            })).collect::<Vec<_>>(),
            "vpns": self.vpns.iter().map(|vpn| json!({
                "path": vpn.path,
                "name": vpn.name,
                "active": vpn.active,
            })).collect::<Vec<_>>(),
        })
    }

    pub fn dns_snapshot(&self) -> Value {
        json!({
            "profile": 0,
            "servers": ["1.1.1.1", "1.0.0.1"],
            "nextdns": "",
        })
    }

    pub fn wifi_secret(&self) -> &str {
        &self.wifi_secret
    }

    pub fn set_enabled(&mut self, enabled: bool) {
        self.enabled = enabled;
        if !enabled {
            self.connected = false;
            self.ssid.clear();
            self.signal = 0;
            for ap in &mut self.access_points {
                ap.active = false;
            }
        }
    }

    pub fn scan(&mut self) {
        self.scanning = false;
    }

    pub fn connect(&mut self, path: &str, secret: Option<&str>) -> Result<(), String> {
        if !self.enabled {
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
        self.connected = true;
        self.resolving = false;
        self.ssid = ap.ssid.clone();
        self.signal = ap.strength;
        Ok(())
    }

    pub fn disconnect(&mut self) -> Result<(), String> {
        if !self.connected {
            return Err("Not connected".into());
        }
        self.connected = false;
        self.ssid.clear();
        self.signal = 0;
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
            self.connected = false;
            self.ssid.clear();
            self.signal = 0;
        }
        self.wifi_secret.clear();
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
