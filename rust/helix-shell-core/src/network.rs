//! The control center's Wi-Fi model: the Network1 snapshot as what the page shows, and the only place that builds write requests.
//! Nothing here talks to D-Bus. The page names a network by the path it was given; this checks that path against the snapshot just read.
//!
//! Not supported yet, and refused here rather than half-done: a network that needs a password, and enterprise networks. The page shows
//! them with a note instead of a button.

use std::collections::BTreeMap;

use serde::Serialize;
use serde_json::{json, Value};

use crate::display::sanitize_for_display;

const MAX_SSID_CHARS: usize = 40;
const MAX_PATH_CHARS: usize = 200;
const MAX_NETWORKS: usize = 12;
const MAX_VPNS: usize = 8;

#[derive(Clone, Debug, PartialEq, Eq)]
struct AccessPoint {
    path: String,
    /// The name exactly as the service sent it. Used when talking back to the service; never shown.
    raw_ssid: String,
    /// The name made safe to show.
    ssid: String,
    strength: u8,
    secured: bool,
    enterprise: bool,
    active: bool,
    saved: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct Vpn {
    path: String,
    name: String,
    active: bool,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Network {
    available: bool,
    wifi_enabled: bool,
    connected: bool,
    resolving: bool,
    scanning: bool,
    ssid: String,
    signal: u8,
    aps: Vec<AccessPoint>,
    vpns: Vec<Vpn>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RequestError {
    Unavailable,
    WifiOff,
    UnknownNetwork,
    /// A secured network that has not been joined before: it needs a password this panel cannot ask for yet.
    NeedsPassword,
    Enterprise,
    AlreadyConnected,
    NotConnected,
    NotSaved,
    UnknownVpn,
}

/// What to send to Network1.Action.
#[derive(Clone, Debug, PartialEq)]
pub struct Request {
    pub action: &'static str,
    pub args: Value,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct NetworkRow {
    pub path: String,
    pub ssid: String,
    pub strength: u8,
    pub secured: bool,
    pub active: bool,
    pub saved: bool,
    /// `connect`: a click joins it. `none`: nothing to click (already connected). Otherwise why it cannot be joined here.
    pub action: &'static str,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct VpnRow {
    pub path: String,
    pub name: String,
    pub active: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct NetworkView {
    pub available: bool,
    pub wifi_enabled: bool,
    pub status: String,
    pub connected: bool,
    pub scanning: bool,
    pub networks: Vec<NetworkRow>,
    pub vpns: Vec<VpnRow>,
}

fn text(entry: &Value, key: &str) -> Option<String> {
    entry.get(key).and_then(Value::as_str).map(str::to_string)
}

fn flag(entry: &Value, key: &str) -> bool {
    entry.get(key).and_then(Value::as_bool).unwrap_or(false)
}

fn path_of(entry: &Value) -> Option<String> {
    let path = text(entry, "path")?;
    (path.starts_with('/') && path.chars().count() <= MAX_PATH_CHARS).then_some(path)
}

fn access_points(snapshot: &Value) -> Vec<AccessPoint> {
    let Some(list) = snapshot.get("access-points").and_then(Value::as_array) else {
        return Vec::new();
    };
    // One row per network name: the same network is often heard from several access points. Keep the strongest, but if any of them
    // is the one we are connected to, the network is the active one.
    let mut by_name: BTreeMap<String, AccessPoint> = BTreeMap::new();
    for entry in list {
        let Some(path) = path_of(entry) else { continue };
        let Some(raw_ssid) = text(entry, "ssid").filter(|name| !name.is_empty()) else {
            continue;
        };
        let ssid = sanitize_for_display(&raw_ssid, MAX_SSID_CHARS);
        if ssid.is_empty() {
            continue;
        }
        let point = AccessPoint {
            path,
            raw_ssid: raw_ssid.clone(),
            ssid,
            strength: entry
                .get("strength")
                .and_then(Value::as_u64)
                .map_or(0, |s| u8::try_from(s.min(100)).unwrap_or(100)),
            secured: flag(entry, "secured"),
            enterprise: flag(entry, "enterprise"),
            active: flag(entry, "active"),
            saved: flag(entry, "saved"),
        };
        match by_name.get_mut(&raw_ssid) {
            None => {
                by_name.insert(raw_ssid, point);
            }
            Some(existing) => {
                let active = existing.active || point.active;
                let saved = existing.saved || point.saved;
                // The access point we are joined to is the one to talk about; otherwise the strongest.
                if point.active || (!existing.active && point.strength > existing.strength) {
                    *existing = point;
                }
                existing.active = active;
                existing.saved = saved;
            }
        }
    }
    let mut points: Vec<AccessPoint> = by_name.into_values().collect();
    points.sort_by(|a, b| {
        b.active
            .cmp(&a.active)
            .then(b.saved.cmp(&a.saved))
            .then(b.strength.cmp(&a.strength))
            .then_with(|| a.ssid.to_lowercase().cmp(&b.ssid.to_lowercase()))
    });
    points.truncate(MAX_NETWORKS);
    points
}

fn vpns(snapshot: &Value) -> Vec<Vpn> {
    let Some(list) = snapshot.get("vpns").and_then(Value::as_array) else {
        return Vec::new();
    };
    list.iter()
        .filter_map(|entry| {
            let name = sanitize_for_display(&text(entry, "name")?, MAX_SSID_CHARS);
            (!name.is_empty()).then(|| Vpn {
                path: path_of(entry).unwrap_or_default(),
                name,
                active: flag(entry, "active"),
            })
        })
        .filter(|vpn| !vpn.path.is_empty())
        .take(MAX_VPNS)
        .collect()
}

impl Network {
    /// A missing, malformed or unavailable snapshot is "no network service".
    pub fn from_snapshot(snapshot: &Value) -> Self {
        if !flag(snapshot, "available") {
            return Self::default();
        }
        Self {
            available: true,
            wifi_enabled: flag(snapshot, "enabled"),
            connected: flag(snapshot, "connected"),
            resolving: flag(snapshot, "resolving"),
            scanning: flag(snapshot, "scanning"),
            ssid: sanitize_for_display(&text(snapshot, "ssid").unwrap_or_default(), MAX_SSID_CHARS),
            signal: snapshot
                .get("signal")
                .and_then(Value::as_u64)
                .map_or(0, |s| u8::try_from(s.min(100)).unwrap_or(100)),
            aps: access_points(snapshot),
            vpns: vpns(snapshot),
        }
    }

    pub fn view(&self) -> NetworkView {
        let status = if !self.available {
            "Network is unavailable".to_string()
        } else if !self.wifi_enabled {
            "Wi-Fi is off".to_string()
        } else if self.resolving {
            "Connecting…".to_string()
        } else if self.connected && !self.ssid.is_empty() {
            format!("Connected to {}", self.ssid)
        } else if self.connected {
            "Connected".to_string()
        } else {
            "Not connected".to_string()
        };
        let networks = if self.wifi_enabled {
            self.aps
                .iter()
                .map(|ap| NetworkRow {
                    path: ap.path.clone(),
                    ssid: ap.ssid.clone(),
                    strength: ap.strength,
                    secured: ap.secured,
                    active: ap.active,
                    saved: ap.saved,
                    action: if ap.active {
                        "none"
                    } else if ap.enterprise {
                        "enterprise"
                    } else if ap.secured && !ap.saved {
                        "password"
                    } else {
                        "connect"
                    },
                })
                .collect()
        } else {
            Vec::new()
        };
        NetworkView {
            available: self.available,
            wifi_enabled: self.wifi_enabled,
            status,
            connected: self.connected,
            scanning: self.scanning,
            networks,
            vpns: self
                .vpns
                .iter()
                .map(|vpn| VpnRow {
                    path: vpn.path.clone(),
                    name: vpn.name.clone(),
                    active: vpn.active,
                })
                .collect(),
        }
    }

    fn require_available(&self) -> Result<(), RequestError> {
        self.available
            .then_some(())
            .ok_or(RequestError::Unavailable)
    }

    fn require_wifi(&self) -> Result<(), RequestError> {
        self.require_available()?;
        self.wifi_enabled.then_some(()).ok_or(RequestError::WifiOff)
    }

    fn point(&self, path: &str) -> Result<&AccessPoint, RequestError> {
        self.aps
            .iter()
            .find(|ap| ap.path == path)
            .ok_or(RequestError::UnknownNetwork)
    }

    pub fn set_wifi(&self, enabled: bool) -> Result<Request, RequestError> {
        self.require_available()?;
        Ok(Request {
            action: "enable",
            args: json!({"value": enabled}),
        })
    }

    pub fn scan(&self) -> Result<Request, RequestError> {
        self.require_wifi()?;
        Ok(Request {
            action: "scan",
            args: json!({}),
        })
    }

    /// Joins a network that needs no new secret: open, or already saved. Anything else is refused with the reason.
    pub fn connect(&self, path: &str) -> Result<Request, RequestError> {
        self.require_wifi()?;
        let ap = self.point(path)?;
        if ap.active {
            return Err(RequestError::AlreadyConnected);
        }
        if ap.enterprise {
            return Err(RequestError::Enterprise);
        }
        if ap.secured && !ap.saved {
            return Err(RequestError::NeedsPassword);
        }
        Ok(Request {
            action: "connect",
            args: json!({"path": ap.path}),
        })
    }

    pub fn disconnect(&self) -> Result<Request, RequestError> {
        self.require_available()?;
        if !self.connected {
            return Err(RequestError::NotConnected);
        }
        Ok(Request {
            action: "disconnect",
            args: json!({}),
        })
    }

    /// Forgets a saved network, named by the path of its row.
    pub fn forget(&self, path: &str) -> Result<Request, RequestError> {
        self.require_available()?;
        let ap = self.point(path)?;
        if !ap.saved {
            return Err(RequestError::NotSaved);
        }
        Ok(Request {
            action: "forget",
            args: json!({"ssid": ap.raw_ssid}),
        })
    }

    pub fn set_vpn(&self, path: &str, active: bool) -> Result<Request, RequestError> {
        self.require_available()?;
        let vpn = self
            .vpns
            .iter()
            .find(|vpn| vpn.path == path)
            .ok_or(RequestError::UnknownVpn)?;
        Ok(Request {
            action: if active {
                "vpn-connect"
            } else {
                "vpn-disconnect"
            },
            args: json!({"path": vpn.path}),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ap(
        path: &str,
        ssid: &str,
        strength: u64,
        secured: bool,
        saved: bool,
        active: bool,
    ) -> Value {
        json!({"path": path, "device": "/dev/1", "ssid": ssid, "strength": strength, "secured": secured, "enterprise": false,
               "key-management": 2, "active": active, "frequency": 5180, "saved": saved})
    }
    fn snapshot() -> Value {
        json!({
            "available": true, "enabled": true, "connected": true, "resolving": false, "scanning": false,
            "ssid": "Home WiFi", "signal": 71, "address": "192.168.1.5", "interface": "wlan0", "kind": 1,
            "access-points": [
                ap("/ap/1", "Cafe", 80, false, false, false),
                ap("/ap/2", "Home WiFi", 71, true, true, true),
                ap("/ap/3", "Neighbour", 55, true, false, false),
                ap("/ap/4", "Old Place", 30, true, true, false),
            ],
            "vpns": [{"path": "/vpn/1", "name": "Work VPN", "active": false}]
        })
    }
    fn rows(network: &Network) -> Vec<(String, &'static str)> {
        network
            .view()
            .networks
            .iter()
            .map(|r| (r.ssid.clone(), r.action))
            .collect()
    }

    #[test]
    fn the_view_orders_active_then_saved_then_strongest_and_says_what_a_click_does() {
        let network = Network::from_snapshot(&snapshot());
        let view = network.view();
        assert_eq!(view.status, "Connected to Home WiFi");
        assert_eq!(
            rows(&network),
            [
                ("Home WiFi".into(), "none"),
                ("Old Place".into(), "connect"),
                ("Cafe".into(), "connect"),
                ("Neighbour".into(), "password")
            ]
        );
    }

    #[test]
    fn status_text_covers_each_state() {
        let mut data = snapshot();
        assert_eq!(
            Network::from_snapshot(&data).view().status,
            "Connected to Home WiFi"
        );
        data["connected"] = json!(false);
        assert_eq!(Network::from_snapshot(&data).view().status, "Not connected");
        data["resolving"] = json!(true);
        assert_eq!(Network::from_snapshot(&data).view().status, "Connecting…");
        data["resolving"] = json!(false);
        data["connected"] = json!(true);
        data["ssid"] = json!("");
        assert_eq!(
            Network::from_snapshot(&data).view().status,
            "Connected",
            "wired"
        );
        data["enabled"] = json!(false);
        let off = Network::from_snapshot(&data).view();
        assert_eq!(off.status, "Wi-Fi is off");
        assert!(off.networks.is_empty(), "no list while Wi-Fi is off");
        assert_eq!(
            Network::from_snapshot(&json!({})).view().status,
            "Network is unavailable"
        );
    }

    #[test]
    fn one_row_per_network_name_keeping_the_strongest_and_the_active_one() {
        let data = json!({"available": true, "enabled": true, "access-points": [
            ap("/ap/a", "Mesh", 40, true, true, false),
            ap("/ap/b", "Mesh", 90, true, false, false),
            ap("/ap/c", "Other", 60, false, false, false),
            ap("/ap/d", "Other", 20, false, false, true),
        ]});
        let network = Network::from_snapshot(&data);
        let view = network.view();
        let mesh = view.networks.iter().find(|r| r.ssid == "Mesh").unwrap();
        assert_eq!(
            (mesh.path.as_str(), mesh.strength, mesh.saved),
            ("/ap/b", 90, true),
            "strongest path, saved if any is"
        );
        let other = view.networks.iter().find(|r| r.ssid == "Other").unwrap();
        assert!(
            other.active && other.path == "/ap/d",
            "the joined access point wins over a stronger one"
        );
        assert_eq!(view.networks.len(), 2);
    }

    #[test]
    fn open_and_saved_networks_can_be_joined_and_others_are_refused_with_a_reason() {
        let network = Network::from_snapshot(&snapshot());
        assert_eq!(
            network.connect("/ap/1").unwrap(),
            Request {
                action: "connect",
                args: json!({"path": "/ap/1"})
            },
            "open"
        );
        assert!(network.connect("/ap/4").is_ok(), "secured but saved");
        assert_eq!(network.connect("/ap/3"), Err(RequestError::NeedsPassword));
        assert_eq!(
            network.connect("/ap/2"),
            Err(RequestError::AlreadyConnected)
        );
        assert_eq!(
            network.connect("/ap/999"),
            Err(RequestError::UnknownNetwork)
        );
        assert_eq!(network.connect(""), Err(RequestError::UnknownNetwork));
        let mut data = snapshot();
        data["access-points"][0]["enterprise"] = json!(true);
        assert_eq!(
            Network::from_snapshot(&data).connect("/ap/1"),
            Err(RequestError::Enterprise)
        );
    }

    #[test]
    fn nothing_can_be_requested_without_a_network_service_or_with_wifi_off() {
        for data in [
            json!({}),
            json!({"available": false}),
            json!("x"),
            json!(null),
        ] {
            let network = Network::from_snapshot(&data);
            assert_eq!(network.set_wifi(true), Err(RequestError::Unavailable));
            assert_eq!(network.scan(), Err(RequestError::Unavailable));
            assert_eq!(network.connect("/ap/1"), Err(RequestError::Unavailable));
            assert_eq!(network.disconnect(), Err(RequestError::Unavailable));
            assert_eq!(network.forget("/ap/1"), Err(RequestError::Unavailable));
            assert_eq!(
                network.set_vpn("/vpn/1", true),
                Err(RequestError::Unavailable)
            );
        }
        let mut data = snapshot();
        data["enabled"] = json!(false);
        let off = Network::from_snapshot(&data);
        assert_eq!(off.scan(), Err(RequestError::WifiOff));
        assert_eq!(off.connect("/ap/1"), Err(RequestError::WifiOff));
        assert!(
            off.set_wifi(true).is_ok(),
            "turning it back on is the one thing allowed"
        );
    }

    #[test]
    fn wifi_scan_and_disconnect_requests() {
        let network = Network::from_snapshot(&snapshot());
        assert_eq!(
            network.set_wifi(false).unwrap(),
            Request {
                action: "enable",
                args: json!({"value": false})
            }
        );
        assert_eq!(network.set_wifi(true).unwrap().args["value"], true);
        assert_eq!(network.scan().unwrap().action, "scan");
        assert_eq!(network.disconnect().unwrap().action, "disconnect");
        let mut data = snapshot();
        data["connected"] = json!(false);
        assert_eq!(
            Network::from_snapshot(&data).disconnect(),
            Err(RequestError::NotConnected)
        );
    }

    #[test]
    fn only_a_saved_network_can_be_forgotten_and_it_is_sent_by_its_exact_name() {
        let mut data = snapshot();
        data["access-points"][3]["ssid"] = json!("Old Place\u{202e}");
        let network = Network::from_snapshot(&data);
        let request = network.forget("/ap/4").unwrap();
        assert_eq!(request.action, "forget");
        assert_eq!(
            request.args["ssid"], "Old Place\u{202e}",
            "the service gets the real name, not the display-safe one"
        );
        assert!(
            !network
                .view()
                .networks
                .iter()
                .any(|r| r.ssid.contains('\u{202e}')),
            "the display name is safe"
        );
        assert_eq!(network.forget("/ap/3"), Err(RequestError::NotSaved));
        assert_eq!(network.forget("/ap/999"), Err(RequestError::UnknownNetwork));
    }

    #[test]
    fn vpns_are_toggled_only_by_a_listed_path() {
        let network = Network::from_snapshot(&snapshot());
        assert_eq!(
            network.set_vpn("/vpn/1", true).unwrap(),
            Request {
                action: "vpn-connect",
                args: json!({"path": "/vpn/1"})
            }
        );
        assert_eq!(
            network.set_vpn("/vpn/1", false).unwrap().action,
            "vpn-disconnect"
        );
        assert_eq!(
            network.set_vpn("/vpn/2", true),
            Err(RequestError::UnknownVpn)
        );
        assert_eq!(
            network.set_vpn("/ap/1", true),
            Err(RequestError::UnknownVpn)
        );
    }

    #[test]
    fn odd_entries_are_dropped_or_tamed() {
        let data = json!({"available": true, "enabled": true, "access-points": [
            {"path": "/ap/ok", "ssid": "Fine", "strength": 500, "secured": false},
            {"path": "relative", "ssid": "No leading slash"},
            {"path": "/ap/empty", "ssid": ""},
            {"path": "/ap/noname"},
            {"ssid": "No path"},
            {"path": format!("/{}", "x".repeat(300)), "ssid": "Long path"},
            {"path": "/ap/ctl", "ssid": "\u{7}\u{200b}", "strength": 10},
            {"path": "/ap/neg", "ssid": "Negative", "strength": -5},
        ], "vpns": [{"name": "No path"}, {"path": "/vpn/x", "name": ""}, {"path": "/vpn/ok", "name": "Fine VPN"}]});
        let view = Network::from_snapshot(&data).view();
        let names: Vec<_> = view.networks.iter().map(|r| r.ssid.as_str()).collect();
        assert_eq!(names, ["Fine", "Negative"]);
        assert_eq!(view.networks[0].strength, 100, "strength is clamped");
        assert_eq!(view.vpns.len(), 1);
    }

    #[test]
    fn the_list_is_bounded() {
        let many: Vec<Value> = (0..40)
            .map(|i| {
                ap(
                    &format!("/ap/{i}"),
                    &format!("Net {i:02}"),
                    50,
                    false,
                    false,
                    false,
                )
            })
            .collect();
        let data = json!({"available": true, "enabled": true, "access-points": many});
        assert_eq!(
            Network::from_snapshot(&data).view().networks.len(),
            MAX_NETWORKS
        );
    }

    #[test]
    fn enterprise_networks_are_shown_as_such_and_cannot_be_clicked() {
        let mut data = snapshot();
        data["access-points"][0]["enterprise"] = json!(true);
        data["access-points"][0]["secured"] = json!(true);
        let network = Network::from_snapshot(&data);
        let row = network
            .view()
            .networks
            .into_iter()
            .find(|r| r.ssid == "Cafe")
            .unwrap();
        assert_eq!(row.action, "enterprise");
        assert_eq!(network.connect("/ap/1"), Err(RequestError::Enterprise));
        // Open but marked enterprise is still refused: the flag decides, not the lock.
        data["access-points"][0]["secured"] = json!(false);
        assert_eq!(
            Network::from_snapshot(&data).connect("/ap/1"),
            Err(RequestError::Enterprise)
        );
    }
}
