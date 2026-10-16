//! BlueZ system-bus backend (blocking D-Bus; polled from the service loop).

use std::collections::HashMap;
use std::sync::Mutex;

use serde_json::{json, Value};
use zbus::blocking::Connection;
use zbus::zvariant::{ObjectPath, OwnedObjectPath, OwnedValue, Value as ZValue};

use super::{snapshot_from_parts, BluetoothFields, Device};

const BLUEZ_SERVICE: &str = "org.bluez";
const ROOT_PATH: &str = "/";
const OBJECT_MANAGER_IFACE: &str = "org.freedesktop.DBus.ObjectManager";
const PROPS_IFACE: &str = "org.freedesktop.DBus.Properties";
const ADAPTER_IFACE: &str = "org.bluez.Adapter1";
const DEVICE_IFACE: &str = "org.bluez.Device1";
const BATTERY_IFACE: &str = "org.bluez.Battery1";

type ManagedObjects = HashMap<OwnedObjectPath, HashMap<String, HashMap<String, OwnedValue>>>;

static SYSTEM_BUS: Mutex<Option<Connection>> = Mutex::new(None);

#[derive(Clone, Debug)]
pub struct BluezBackend {
    fields: BluetoothFields,
    devices: Vec<Device>,
    adapter_path: String,
    last_snapshot: Value,
}

pub fn probe() -> bool {
    std::process::Command::new("busctl")
        .args(["--system", "status", "org.bluez"])
        .output()
        .map(|output| output.status.success())
        .unwrap_or(false)
}

impl BluezBackend {
    pub fn new() -> Self {
        Self {
            fields: BluetoothFields {
                available: false,
                enabled: false,
                scanning: false,
                name: String::new(),
                discoverable: false,
                pairable: false,
                soft_blocked: false,
                hard_blocked: false,
            },
            devices: Vec::new(),
            adapter_path: String::new(),
            last_snapshot: json!({"available": false}),
        }
    }

    pub fn is_available(&self) -> bool {
        self.fields.available
    }

    pub fn snapshot(&self) -> Value {
        snapshot_from_parts(&self.fields, &self.devices)
    }

    pub fn refresh(&mut self) -> bool {
        match refresh_state() {
            Ok(next) => {
                let snap = snapshot_from_parts(&next.fields, &next.devices);
                let changed = self.last_snapshot != snap;
                self.fields = next.fields;
                self.devices = next.devices;
                self.adapter_path = next.adapter_path;
                self.last_snapshot = snap;
                changed
            }
            Err(error) => {
                tracing::debug!(%error, "bluetooth BlueZ refresh failed");
                let snap = json!({"available": false});
                let changed = self.last_snapshot != snap;
                self.fields.available = false;
                self.devices.clear();
                self.adapter_path.clear();
                self.last_snapshot = snap;
                changed
            }
        }
    }

    pub fn set_enabled(&mut self, enabled: bool) -> Result<(), String> {
        if enabled && self.fields.hard_blocked {
            return Err("Bluetooth is blocked by a hardware switch".into());
        }
        let adapter = self.adapter_path.clone();
        if adapter.is_empty() {
            return Err("Bluetooth adapter unavailable".into());
        }
        with_connection(|conn| {
            bz_call(
                conn,
                &adapter,
                PROPS_IFACE,
                "Set",
                &(ADAPTER_IFACE, "Powered", ZValue::Bool(enabled)),
            )?;
            Ok(())
        })?;
        self.refresh();
        Ok(())
    }

    pub fn set_scanning(&mut self, on: bool) -> Result<(), String> {
        if !self.fields.enabled {
            return Err("Bluetooth is off".into());
        }
        let adapter = self.adapter_path.clone();
        if adapter.is_empty() {
            return Err("Bluetooth adapter unavailable".into());
        }
        with_connection(|conn| {
            if on {
                bz_call(conn, &adapter, ADAPTER_IFACE, "StartDiscovery", &())?;
            } else {
                bz_call(conn, &adapter, ADAPTER_IFACE, "StopDiscovery", &())?;
            }
            Ok(())
        })?;
        self.refresh();
        Ok(())
    }

    pub fn connect(&mut self, path: &str) -> Result<(), String> {
        if !self.fields.enabled {
            return Err("Bluetooth is off".into());
        }
        if !self.devices.iter().any(|device| device.path == path) {
            return Err("Device is no longer available".into());
        }
        let device = self
            .devices
            .iter()
            .find(|device| device.path == path)
            .ok_or_else(|| "Device is no longer available".to_string())?;
        if !device.paired {
            return Err("Device is not paired".into());
        }
        if device.connected {
            return Err("Already connected".into());
        }
        with_connection(|conn| {
            bz_call(conn, path, DEVICE_IFACE, "Connect", &())?;
            Ok(())
        })?;
        self.refresh();
        Ok(())
    }

    pub fn disconnect(&mut self, path: &str) -> Result<(), String> {
        if !self.fields.enabled {
            return Err("Bluetooth is off".into());
        }
        if !self
            .devices
            .iter()
            .any(|device| device.path == path && device.connected)
        {
            return Err("Not connected".into());
        }
        with_connection(|conn| {
            bz_call(conn, path, DEVICE_IFACE, "Disconnect", &())?;
            Ok(())
        })?;
        self.refresh();
        Ok(())
    }

    pub fn pair(&mut self, path: &str) -> Result<(), String> {
        // Like the C++ service, leave the adapter-power decision to BlueZ.
        if !self.devices.iter().any(|device| device.path == path) {
            return Err("Device is no longer available".into());
        }
        if self
            .devices
            .iter()
            .any(|device| device.path == path && device.paired)
        {
            return Err("Already paired".into());
        }
        with_connection(|conn| {
            bz_call(conn, path, DEVICE_IFACE, "Pair", &())?;
            Ok(())
        })?;
        self.refresh();
        Ok(())
    }

    pub fn set_trusted(&mut self, path: &str, trusted: bool) -> Result<(), String> {
        if !self.devices.iter().any(|device| device.path == path) {
            return Err("Device is no longer available".into());
        }
        with_connection(|conn| {
            bz_call(
                conn,
                path,
                PROPS_IFACE,
                "Set",
                &(DEVICE_IFACE, "Trusted", ZValue::Bool(trusted)),
            )?;
            Ok(())
        })?;
        self.refresh();
        Ok(())
    }

    pub fn cancel_pair(&mut self, path: &str) -> Result<(), String> {
        if !self.devices.iter().any(|device| device.path == path) {
            return Err("Device is no longer available".into());
        }
        with_connection(|conn| {
            let _ = bz_call(conn, path, DEVICE_IFACE, "CancelPairing", &());
            Ok(())
        })?;
        self.refresh();
        Ok(())
    }

    pub fn forget(&mut self, path: &str) -> Result<(), String> {
        let adapter = self.adapter_path.clone();
        if adapter.is_empty() {
            return Err("Bluetooth adapter unavailable".into());
        }
        if !self
            .devices
            .iter()
            .any(|device| device.path == path && device.paired)
        {
            return Err("Device is not paired".into());
        }
        with_connection(|conn| {
            bz_call(
                conn,
                &adapter,
                ADAPTER_IFACE,
                "RemoveDevice",
                &(ObjectPath::from_str_unchecked(path),),
            )?;
            Ok(())
        })?;
        self.refresh();
        Ok(())
    }
}

struct Refreshed {
    fields: BluetoothFields,
    devices: Vec<Device>,
    adapter_path: String,
}

fn refresh_state() -> Result<Refreshed, String> {
    with_connection(|conn| {
        let objects = managed_objects(conn)?;
        let (hard_blocked, soft_blocked) = rfkill_blocks();
        let mut adapter_path = String::new();
        let mut fields = BluetoothFields {
            available: false,
            enabled: false,
            scanning: false,
            name: String::new(),
            discoverable: false,
            pairable: false,
            soft_blocked,
            hard_blocked,
        };
        let mut devices = Vec::new();
        let mut batteries: HashMap<String, u8> = HashMap::new();

        for (path, interfaces) in &objects {
            let path_str = path.to_string();
            if let Some(props) = interfaces.get(BATTERY_IFACE) {
                if let Some(percent) = prop_u8(props, "Percentage") {
                    batteries.insert(path_str, percent);
                }
            }
        }

        for (path, interfaces) in objects {
            let path_str = path.to_string();
            if let Some(props) = interfaces.get(ADAPTER_IFACE) {
                if adapter_path.is_empty() {
                    adapter_path = path_str.clone();
                    fields.available = true;
                    fields.enabled = prop_bool(props, "Powered").unwrap_or(false);
                    fields.discoverable = prop_bool(props, "Discoverable").unwrap_or(false);
                    fields.pairable = prop_bool(props, "Pairable").unwrap_or(false);
                    fields.scanning = prop_bool(props, "Discovering").unwrap_or(false);
                    fields.name = prop_str(props, "Alias")
                        .or_else(|| prop_str(props, "Name"))
                        .unwrap_or_default();
                    if prop_str(props, "PowerState").as_deref() == Some("off-blocked") {
                        fields.soft_blocked = true;
                    }
                }
            }
            if let Some(props) = interfaces.get(DEVICE_IFACE) {
                let address = prop_str(props, "Address").unwrap_or_default();
                let name = prop_str(props, "Alias")
                    .or_else(|| prop_str(props, "Name"))
                    .unwrap_or_else(|| {
                        if address.is_empty() {
                            "Unknown device".into()
                        } else {
                            address.clone()
                        }
                    });
                let connected = prop_bool(props, "Connected").unwrap_or(false);
                let (has_battery, battery) = if connected {
                    battery_for_device(&path_str, &batteries)
                } else {
                    (false, 0)
                };
                let has_rssi = props.contains_key("RSSI");
                let rssi = prop_i16(props, "RSSI").unwrap_or(0);
                devices.push(Device {
                    path: path_str,
                    address,
                    name,
                    kind: device_kind(props),
                    paired: prop_bool(props, "Paired").unwrap_or(false),
                    trusted: prop_bool(props, "Trusted").unwrap_or(false),
                    connected,
                    connecting: false,
                    has_battery,
                    battery,
                    has_rssi,
                    rssi,
                });
            }
        }

        devices.sort_by(|a, b| a.name.cmp(&b.name));

        Ok(Refreshed {
            fields,
            devices,
            adapter_path,
        })
    })
}

/// Unique bus name currently owning the daemon, `None` when it is not on the bus.
pub fn bus_owner() -> Option<String> {
    with_connection(|conn| {
        let reply = conn
            .call_method(
                Some("org.freedesktop.DBus"),
                "/org/freedesktop/DBus",
                Some("org.freedesktop.DBus"),
                "GetNameOwner",
                &(BLUEZ_SERVICE,),
            )
            .map_err(|error| error.to_string())?;
        reply
            .body()
            .deserialize::<String>()
            .map_err(|error| error.to_string())
    })
    .ok()
}

fn with_connection<R>(
    operation: impl FnOnce(&Connection) -> Result<R, String>,
) -> Result<R, String> {
    crate::blocking::run(|| {
        let mut guard = SYSTEM_BUS
            .lock()
            .map_err(|_| "system bus mutex poisoned".to_string())?;
        if guard.is_none() {
            *guard = Some(Connection::system().map_err(|error| error.to_string())?);
        }
        let conn = guard
            .as_ref()
            .ok_or_else(|| "system bus unavailable".to_string())?;
        operation(conn)
    })
}

fn bz_call<B>(
    conn: &Connection,
    path: &str,
    iface: &str,
    method: &str,
    body: &B,
) -> Result<zbus::Message, String>
where
    B: serde::Serialize + zbus::zvariant::DynamicType,
{
    conn.call_method(Some(BLUEZ_SERVICE), path, Some(iface), method, body)
        .map_err(|error| error.to_string())
}

fn managed_objects(conn: &Connection) -> Result<ManagedObjects, String> {
    let reply = bz_call(
        conn,
        ROOT_PATH,
        OBJECT_MANAGER_IFACE,
        "GetManagedObjects",
        &(),
    )?;
    reply
        .body()
        .deserialize()
        .map_err(|error| error.to_string())
}

fn rfkill_blocks() -> (bool, bool) {
    let output = std::process::Command::new("rfkill")
        .arg("list")
        .arg("bluetooth")
        .output();
    let Ok(output) = output else {
        return (false, false);
    };
    if !output.status.success() {
        return (false, false);
    }
    let text = String::from_utf8_lossy(&output.stdout);
    let soft = text.contains("Soft blocked: yes");
    let hard = text.contains("Hard blocked: yes");
    (hard, soft)
}

fn device_kind(props: &HashMap<String, OwnedValue>) -> u32 {
    if let Some(icon) = prop_str(props, "Icon") {
        let kind = classify_icon(&icon);
        if kind != 0 {
            return kind;
        }
    }
    if let Some(class) = prop_u32(props, "Class") {
        return classify_class(class);
    }
    0
}

fn battery_for_device(device_path: &str, batteries: &HashMap<String, u8>) -> (bool, u8) {
    if let Some(percent) = batteries.get(device_path) {
        return (true, *percent);
    }
    let prefix = format!("{}/", device_path);
    for (path, percent) in batteries {
        if path.starts_with(&prefix) {
            return (true, *percent);
        }
    }
    (false, 0)
}

fn classify_icon(icon: &str) -> u32 {
    match icon {
        "audio-headset" => 1,
        "audio-headphones" => 2,
        "audio-card" | "audio-speakers" => 4,
        "input-mouse" => 6,
        "input-keyboard" => 7,
        "input-gaming" => 10,
        "phone" => 8,
        "computer" => 9,
        "video-display" => 12,
        _ => 0,
    }
}

fn classify_class(cod: u32) -> u32 {
    let major = (cod >> 8) & 0x1f;
    let minor = (cod >> 2) & 0x3f;
    match major {
        0x01 => 9,
        0x02 => 8,
        0x04 => match minor {
            0x01 | 0x02 => 1,
            0x06 => 2,
            0x05 | 0x07 => 4,
            0x04 => 5,
            0x0a | 0x0b => 12,
            _ => 2,
        },
        0x05 => match minor & 0x0f {
            0x01 => 7,
            0x02 => 6,
            _ => 0,
        },
        0x07 => 11,
        0x08 => 10,
        _ => 0,
    }
}

fn prop_bool(map: &HashMap<String, OwnedValue>, key: &str) -> Option<bool> {
    map.get(key).and_then(|value| match &**value {
        ZValue::Bool(flag) => Some(*flag),
        _ => None,
    })
}

fn prop_u32(map: &HashMap<String, OwnedValue>, key: &str) -> Option<u32> {
    map.get(key).and_then(|value| match &**value {
        ZValue::U32(number) => Some(*number),
        ZValue::U64(number) => u32::try_from(*number).ok(),
        _ => None,
    })
}

fn prop_u8(map: &HashMap<String, OwnedValue>, key: &str) -> Option<u8> {
    prop_u32(map, key).and_then(|number| u8::try_from(number).ok())
}

fn prop_i16(map: &HashMap<String, OwnedValue>, key: &str) -> Option<i16> {
    map.get(key).and_then(|value| match &**value {
        ZValue::I16(number) => Some(*number),
        ZValue::I32(number) => i16::try_from(*number).ok(),
        _ => None,
    })
}

fn prop_str(map: &HashMap<String, OwnedValue>, key: &str) -> Option<String> {
    map.get(key).and_then(|value| match &**value {
        ZValue::Str(text) => Some(text.to_string()),
        _ => None,
    })
}
