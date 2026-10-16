//! The control center's Bluetooth model: the Bluetooth1 snapshot as what the page shows, and the only place that builds write requests.
//! Nothing here talks to D-Bus. The page names a device by the path it was listed with; this checks that path against the snapshot just read.
//!
//! Pairing a new device is a prompt flow (PINs, passkeys, confirmation): `Bluetooth::pair` starts it and `crate::pairing` follows it. The
//! control center does not offer it, so there its unpaired devices are listed with a note and cannot be clicked.

use std::collections::BTreeMap;

use serde::Serialize;
use serde_json::{json, Value};

use crate::display::sanitize_for_display;

const MAX_NAME_CHARS: usize = 40;
const MAX_PATH_CHARS: usize = 200;
const MAX_DEVICES: usize = 12;

#[derive(Clone, Debug, PartialEq, Eq)]
struct Device {
    path: String,
    name: String,
    paired: bool,
    connected: bool,
    connecting: bool,
    battery: Option<u8>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Bluetooth {
    available: bool,
    enabled: bool,
    scanning: bool,
    hard_blocked: bool,
    devices: Vec<Device>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RequestError {
    Unavailable,
    /// A hardware switch has Bluetooth off; software cannot turn it on.
    HardBlocked,
    Off,
    UnknownDevice,
    /// Not paired: pairing needs a prompt flow that is not built yet.
    NotPaired,
    AlreadyConnected,
    Busy,
    NotConnected,
    AlreadyPaired,
}

/// What to send to Bluetooth1.Action.
#[derive(Clone, Debug, PartialEq)]
pub struct Request {
    pub action: &'static str,
    pub args: Value,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
#[cfg_attr(feature = "ts", derive(ts_rs::TS), ts(export))]
pub struct DeviceRow {
    pub path: String,
    pub name: String,
    pub paired: bool,
    pub connected: bool,
    pub connecting: bool,
    pub battery: Option<u8>,
    /// `connect`, `disconnect`, `busy` (a connection is in progress) or `pair` (not paired: cannot be used from here yet).
    pub action: &'static str,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
#[cfg_attr(feature = "ts", derive(ts_rs::TS), ts(export))]
pub struct BluetoothView {
    pub available: bool,
    pub enabled: bool,
    pub blocked: bool,
    pub scanning: bool,
    pub status: String,
    pub devices: Vec<DeviceRow>,
}

fn flag(entry: &Value, key: &str) -> bool {
    entry.get(key).and_then(Value::as_bool).unwrap_or(false)
}

fn devices(snapshot: &Value) -> Vec<Device> {
    let Some(list) = snapshot.get("devices").and_then(Value::as_array) else {
        return Vec::new();
    };
    let mut by_path: BTreeMap<String, Device> = BTreeMap::new();
    for entry in list {
        let Some(path) = entry
            .get("path")
            .and_then(Value::as_str)
            .filter(|path| path.starts_with('/') && path.chars().count() <= MAX_PATH_CHARS)
        else {
            continue;
        };
        let name = ["name", "address"]
            .iter()
            .filter_map(|field| entry.get(*field).and_then(Value::as_str))
            .map(|text| sanitize_for_display(text, MAX_NAME_CHARS))
            .find(|text| !text.is_empty());
        let Some(name) = name else { continue };
        let battery = flag(entry, "has-battery")
            .then(|| entry.get("battery").and_then(Value::as_u64))
            .flatten()
            .map(|percent| u8::try_from(percent.min(100)).unwrap_or(100));
        by_path.insert(
            path.to_string(),
            Device {
                path: path.to_string(),
                name,
                paired: flag(entry, "paired"),
                connected: flag(entry, "connected"),
                connecting: flag(entry, "connecting"),
                battery,
            },
        );
    }
    let mut all: Vec<Device> = by_path.into_values().collect();
    all.sort_by(|a, b| {
        b.connected
            .cmp(&a.connected)
            .then(b.paired.cmp(&a.paired))
            .then_with(|| a.name.to_lowercase().cmp(&b.name.to_lowercase()))
    });
    all.truncate(MAX_DEVICES);
    all
}

impl Bluetooth {
    /// A missing, malformed or unavailable snapshot is "no Bluetooth".
    pub fn from_snapshot(snapshot: &Value) -> Self {
        if !flag(snapshot, "available") {
            return Self::default();
        }
        Self {
            available: true,
            enabled: flag(snapshot, "enabled"),
            scanning: flag(snapshot, "scanning"),
            hard_blocked: flag(snapshot, "hard-blocked"),
            devices: devices(snapshot),
        }
    }

    pub fn view(&self) -> BluetoothView {
        let connected: Vec<&Device> = self.devices.iter().filter(|d| d.connected).collect();
        let status = if !self.available {
            "Bluetooth is unavailable".to_string()
        } else if self.hard_blocked {
            "Bluetooth is blocked by a hardware switch".to_string()
        } else if !self.enabled {
            "Bluetooth is off".to_string()
        } else if connected.len() == 1 {
            format!("Connected to {}", connected[0].name)
        } else if connected.len() > 1 {
            format!("{} devices connected", connected.len())
        } else {
            "On".to_string()
        };
        let devices = if self.enabled {
            self.devices
                .iter()
                .map(|device| DeviceRow {
                    path: device.path.clone(),
                    name: device.name.clone(),
                    paired: device.paired,
                    connected: device.connected,
                    connecting: device.connecting,
                    battery: device.battery,
                    action: if !device.paired {
                        "pair"
                    } else if device.connecting {
                        "busy"
                    } else if device.connected {
                        "disconnect"
                    } else {
                        "connect"
                    },
                })
                .collect()
        } else {
            Vec::new()
        };
        BluetoothView {
            available: self.available,
            enabled: self.enabled,
            blocked: self.hard_blocked,
            scanning: self.scanning,
            status,
            devices,
        }
    }

    fn require_available(&self) -> Result<(), RequestError> {
        self.available
            .then_some(())
            .ok_or(RequestError::Unavailable)
    }

    fn require_on(&self) -> Result<(), RequestError> {
        self.require_available()?;
        self.enabled.then_some(()).ok_or(RequestError::Off)
    }

    fn device(&self, path: &str) -> Result<&Device, RequestError> {
        self.devices
            .iter()
            .find(|device| device.path == path)
            .ok_or(RequestError::UnknownDevice)
    }

    pub fn set_power(&self, enabled: bool) -> Result<Request, RequestError> {
        self.require_available()?;
        if enabled && self.hard_blocked {
            return Err(RequestError::HardBlocked);
        }
        Ok(Request {
            action: "enable",
            args: json!({"value": enabled}),
        })
    }

    /// Starts or stops looking for nearby devices.
    pub fn set_scanning(&self, on: bool) -> Result<Request, RequestError> {
        self.require_on()?;
        Ok(Request {
            action: if on { "scan" } else { "stop-scan" },
            args: json!({}),
        })
    }

    pub fn connect(&self, path: &str) -> Result<Request, RequestError> {
        self.require_on()?;
        let device = self.device(path)?;
        if !device.paired {
            return Err(RequestError::NotPaired);
        }
        if device.connected {
            return Err(RequestError::AlreadyConnected);
        }
        if device.connecting {
            return Err(RequestError::Busy);
        }
        Ok(Request {
            action: "connect",
            args: json!({"path": device.path}),
        })
    }

    pub fn is_paired(&self, path: &str) -> bool {
        self.devices.iter().any(|d| d.path == path && d.paired)
    }

    /// The name a device is shown under, for a prompt about it.
    pub fn name_of(&self, path: &str) -> Option<String> {
        self.devices.iter().find(|d| d.path == path).map(|d| d.name.clone())
    }

    /// Starts pairing with a device that has not been paired yet.
    pub fn pair(&self, path: &str) -> Result<Request, RequestError> {
        self.require_on()?;
        let device = self.device(path)?;
        if device.paired {
            return Err(RequestError::AlreadyPaired);
        }
        Ok(Request {
            action: "pair",
            args: json!({"path": device.path}),
        })
    }

    pub fn disconnect(&self, path: &str) -> Result<Request, RequestError> {
        self.require_on()?;
        let device = self.device(path)?;
        if !device.connected {
            return Err(RequestError::NotConnected);
        }
        Ok(Request {
            action: "disconnect",
            args: json!({"path": device.path}),
        })
    }

    /// Removes a paired device.
    pub fn forget(&self, path: &str) -> Result<Request, RequestError> {
        self.require_available()?;
        let device = self.device(path)?;
        if !device.paired {
            return Err(RequestError::NotPaired);
        }
        Ok(Request {
            action: "forget",
            args: json!({"path": device.path}),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dev(
        path: &str,
        name: &str,
        paired: bool,
        connected: bool,
        connecting: bool,
        battery: Option<u64>,
    ) -> Value {
        json!({"path": path, "address": "AA:BB:CC:DD:EE:FF", "name": name, "kind": 1, "paired": paired, "trusted": paired,
               "connected": connected, "connecting": connecting, "has-battery": battery.is_some(),
               "battery": battery.unwrap_or(0), "has-rssi": false, "rssi": 0})
    }
    fn snapshot() -> Value {
        json!({"available": true, "enabled": true, "scanning": false, "name": "laptop", "discoverable": false, "pairable": true,
               "soft-blocked": false, "hard-blocked": false, "devices": [
            dev("/d/phone", "Phone", true, false, false, None),
            dev("/d/buds", "Earbuds", true, true, false, Some(80)),
            dev("/d/kbd", "Keyboard", true, false, true, None),
            dev("/d/speaker", "Kitchen Speaker", false, false, false, None),
        ]})
    }
    fn actions(bt: &Bluetooth) -> Vec<(String, &'static str)> {
        bt.view()
            .devices
            .iter()
            .map(|d| (d.name.clone(), d.action))
            .collect()
    }

    #[test]
    fn devices_are_ordered_connected_then_paired_and_say_what_a_click_does() {
        let bt = Bluetooth::from_snapshot(&snapshot());
        assert_eq!(
            actions(&bt),
            [
                ("Earbuds".into(), "disconnect"),
                ("Keyboard".into(), "busy"),
                ("Phone".into(), "connect"),
                ("Kitchen Speaker".into(), "pair"),
            ]
        );
        assert_eq!(bt.view().devices[0].battery, Some(80));
        assert_eq!(bt.view().devices[2].battery, None);
    }

    #[test]
    fn status_text_covers_each_state() {
        let mut data = snapshot();
        assert_eq!(
            Bluetooth::from_snapshot(&data).view().status,
            "Connected to Earbuds"
        );
        data["devices"][0]["connected"] = json!(true);
        assert_eq!(
            Bluetooth::from_snapshot(&data).view().status,
            "2 devices connected"
        );
        data["devices"] = json!([]);
        assert_eq!(Bluetooth::from_snapshot(&data).view().status, "On");
        data["enabled"] = json!(false);
        let off = Bluetooth::from_snapshot(&data).view();
        assert_eq!(off.status, "Bluetooth is off");
        assert!(off.devices.is_empty());
        data["hard-blocked"] = json!(true);
        assert_eq!(
            Bluetooth::from_snapshot(&data).view().status,
            "Bluetooth is blocked by a hardware switch"
        );
        assert_eq!(
            Bluetooth::from_snapshot(&json!({})).view().status,
            "Bluetooth is unavailable"
        );
    }

    #[test]
    fn only_paired_idle_devices_can_be_connected() {
        let bt = Bluetooth::from_snapshot(&snapshot());
        assert_eq!(
            bt.connect("/d/phone").unwrap(),
            Request {
                action: "connect",
                args: json!({"path": "/d/phone"})
            }
        );
        assert_eq!(bt.connect("/d/buds"), Err(RequestError::AlreadyConnected));
        assert_eq!(bt.connect("/d/kbd"), Err(RequestError::Busy));
        assert_eq!(bt.connect("/d/speaker"), Err(RequestError::NotPaired));
        assert_eq!(bt.connect("/d/nope"), Err(RequestError::UnknownDevice));
        assert_eq!(bt.connect(""), Err(RequestError::UnknownDevice));
    }

    #[test]
    fn disconnect_and_forget_requests() {
        let bt = Bluetooth::from_snapshot(&snapshot());
        assert_eq!(bt.disconnect("/d/buds").unwrap().action, "disconnect");
        assert_eq!(bt.disconnect("/d/phone"), Err(RequestError::NotConnected));
        assert_eq!(bt.disconnect("/d/nope"), Err(RequestError::UnknownDevice));
        assert_eq!(
            bt.forget("/d/phone").unwrap(),
            Request {
                action: "forget",
                args: json!({"path": "/d/phone"})
            }
        );
        assert_eq!(
            bt.forget("/d/speaker"),
            Err(RequestError::NotPaired),
            "an unpaired device has nothing to forget"
        );
        assert_eq!(bt.forget("/d/nope"), Err(RequestError::UnknownDevice));
    }

    #[test]
    fn power_and_scan_requests() {
        let bt = Bluetooth::from_snapshot(&snapshot());
        assert_eq!(
            bt.set_power(false).unwrap(),
            Request {
                action: "enable",
                args: json!({"value": false})
            }
        );
        assert_eq!(bt.set_power(true).unwrap().args["value"], true);
        assert_eq!(bt.set_scanning(true).unwrap().action, "scan");
        assert_eq!(bt.set_scanning(false).unwrap().action, "stop-scan");
        let mut data = snapshot();
        data["enabled"] = json!(false);
        let off = Bluetooth::from_snapshot(&data);
        assert_eq!(off.set_scanning(true), Err(RequestError::Off));
        assert_eq!(off.connect("/d/phone"), Err(RequestError::Off));
        assert_eq!(off.disconnect("/d/buds"), Err(RequestError::Off));
        assert!(
            off.set_power(true).is_ok(),
            "turning it on is allowed while off"
        );
        data["hard-blocked"] = json!(true);
        let blocked = Bluetooth::from_snapshot(&data);
        assert_eq!(blocked.set_power(true), Err(RequestError::HardBlocked));
        assert!(blocked.set_power(false).is_ok());
    }

    #[test]
    fn nothing_can_be_requested_without_a_bluetooth_service() {
        for data in [
            json!({}),
            json!({"available": false}),
            json!("x"),
            json!(null),
        ] {
            let bt = Bluetooth::from_snapshot(&data);
            assert_eq!(bt.set_power(true), Err(RequestError::Unavailable));
            assert_eq!(bt.set_scanning(true), Err(RequestError::Unavailable));
            assert_eq!(bt.connect("/d/phone"), Err(RequestError::Unavailable));
            assert_eq!(bt.disconnect("/d/phone"), Err(RequestError::Unavailable));
            assert_eq!(bt.forget("/d/phone"), Err(RequestError::Unavailable));
        }
    }

    #[test]
    fn odd_entries_are_dropped_or_tamed() {
        let data = json!({"available": true, "enabled": true, "devices": [
            {"path": "/d/ok", "name": "Fine", "address": "AA", "paired": true, "has-battery": true, "battery": 500},
            {"path": "relative", "name": "No slash"},
            {"name": "No path"},
            {"path": "/d/empty", "name": "", "address": ""},
            {"path": "/d/addr", "name": "", "address": "11:22:33:44:55:66", "paired": true},
            {"path": "/d/evil", "name": "Evil\u{202e}Name\u{7}", "paired": true},
            {"path": format!("/{}", "x".repeat(300)), "name": "Long path"},
        ]});
        let view = Bluetooth::from_snapshot(&data).view();
        let names: Vec<&str> = view.devices.iter().map(|d| d.name.as_str()).collect();
        assert!(
            names.contains(&"Fine") && names.contains(&"11:22:33:44:55:66"),
            "{names:?}"
        );
        assert_eq!(view.devices.len(), 3, "{names:?}");
        assert!(view
            .devices
            .iter()
            .all(|d| !d.name.contains('\u{202e}') && !d.name.contains('\u{7}')));
        assert_eq!(
            view.devices
                .iter()
                .find(|d| d.name == "Fine")
                .unwrap()
                .battery,
            Some(100),
            "battery is clamped"
        );
    }

    #[test]
    fn the_list_is_bounded() {
        let many: Vec<Value> = (0..40)
            .map(|i| {
                dev(
                    &format!("/d/{i}"),
                    &format!("Device {i:02}"),
                    true,
                    false,
                    false,
                    None,
                )
            })
            .collect();
        let data = json!({"available": true, "enabled": true, "devices": many});
        assert_eq!(
            Bluetooth::from_snapshot(&data).view().devices.len(),
            MAX_DEVICES
        );
    }

    #[test]
    fn a_connected_device_comes_first_even_when_its_name_sorts_last() {
        let data = json!({"available": true, "enabled": true, "devices": [
            dev("/d/a", "Alpha", true, false, false, None),
            dev("/d/b", "Beta", false, false, false, None),
            dev("/d/z", "Zebra", true, true, false, None),
            dev("/d/m", "Mid", true, false, false, None),
        ]});
        let names: Vec<String> = Bluetooth::from_snapshot(&data)
            .view()
            .devices
            .into_iter()
            .map(|d| d.name)
            .collect();
        assert_eq!(
            names,
            ["Zebra", "Alpha", "Mid", "Beta"],
            "connected, then paired, then the rest, each by name"
        );
    }

    #[test]
    fn battery_levels_above_100_are_clamped() {
        let data = json!({"available": true, "enabled": true, "devices": [
            dev("/d/a", "A", true, false, false, Some(150)),
            dev("/d/b", "B", true, false, false, Some(100)),
            dev("/d/c", "C", true, false, false, Some(0)),
        ]});
        let view = Bluetooth::from_snapshot(&data).view();
        let batteries: Vec<Option<u8>> = view.devices.iter().map(|d| d.battery).collect();
        assert_eq!(batteries, [Some(100), Some(100), Some(0)]);
    }
}
