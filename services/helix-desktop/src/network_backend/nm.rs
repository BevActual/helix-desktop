//! NetworkManager system-bus backend (blocking D-Bus; polled from the service loop).

use std::collections::{HashMap, HashSet};
use std::sync::Mutex;

use serde_json::{json, Value};
use zbus::blocking::Connection;
use zbus::zvariant::{ObjectPath, OwnedObjectPath, OwnedValue, Value as ZValue};

use super::dns::{builtin_servers, DnsOverride};
use super::enterprise::{self, EnterpriseCredentials, Validation};
use super::{snapshot_from_parts, AccessPoint, NetworkFields, Vpn};

const NM_SERVICE: &str = "org.freedesktop.NetworkManager";
const NM_PATH: &str = "/org/freedesktop/NetworkManager";
const NM_IFACE: &str = "org.freedesktop.NetworkManager";
const PROPS_IFACE: &str = "org.freedesktop.DBus.Properties";
const DEVICE_IFACE: &str = "org.freedesktop.NetworkManager.Device";
const DEVICE_WIFI_IFACE: &str = "org.freedesktop.NetworkManager.Device.Wireless";
const AP_IFACE: &str = "org.freedesktop.NetworkManager.AccessPoint";
const ACTIVE_CONN_IFACE: &str = "org.freedesktop.NetworkManager.Connection.Active";
const SETTINGS_PATH: &str = "/org/freedesktop/NetworkManager/Settings";
const SETTINGS_IFACE: &str = "org.freedesktop.NetworkManager.Settings";
const SETTINGS_CONN_IFACE: &str = "org.freedesktop.NetworkManager.Settings.Connection";
const IP4_IFACE: &str = "org.freedesktop.NetworkManager.IP4Config";

const NM_DEVICE_TYPE_WIFI: u32 = 2;
const NM_DEVICE_TYPE_ETHERNET: u32 = 1;
const NM_DEVICE_STATE_ACTIVATED: u32 = 100;
const NM_DEVICE_STATE_PREPARE: u32 = 40;

const NM80211_AP_SEC_NONE: u32 = 0;
const NM80211_AP_SEC_KEY_MGMT_8021X: u32 = 0x00000200;
const NM80211_AP_SEC_KEY_MGMT_SAE: u32 = 0x00000400;
const NM80211_AP_SEC_KEY_MGMT_EAP_SUITE_B: u32 = 0x00002000;

static SYSTEM_BUS: Mutex<Option<Connection>> = Mutex::new(None);

#[derive(Clone, Debug)]
pub struct NmBackend {
    fields: NetworkFields,
    access_points: Vec<AccessPoint>,
    vpns: Vec<Vpn>,
    saved_ssids: HashSet<String>,
    saved_wired_paths: Vec<String>,
    saved_cellular_paths: Vec<String>,
    dns: Value,
    wifi_secret: String,
    last_snapshot: Value,
    scanning: bool,
}

pub fn probe() -> bool {
    std::process::Command::new("busctl")
        .args(["--system", "status", "org.freedesktop.NetworkManager"])
        .output()
        .map(|output| output.status.success())
        .unwrap_or(false)
}

impl NmBackend {
    pub fn new() -> Self {
        let backend = Self {
            fields: NetworkFields {
                available: false,
                enabled: false,
                connected: false,
                resolving: false,
                scanning: false,
                ssid: String::new(),
                signal: 0,
                address: String::new(),
                interface: String::new(),
                kind: 0,
                cellular_active: false,
                wired_available: false,
                cellular_available: false,
            },
            access_points: Vec::new(),
            vpns: Vec::new(),
            saved_ssids: HashSet::new(),
            saved_wired_paths: Vec::new(),
            saved_cellular_paths: Vec::new(),
            dns: json!({"profile": 0, "servers": [], "nextdns": ""}),
            wifi_secret: String::new(),
            last_snapshot: json!({"available": false}),
            scanning: false,
        };
        backend
    }

    pub fn is_available(&self) -> bool {
        self.fields.available
    }

    pub fn snapshot(&self) -> Value {
        snapshot_from_parts(&self.fields, &self.access_points, &self.vpns)
    }

    pub fn dns_snapshot(&self) -> Value {
        self.dns.clone()
    }

    pub fn wifi_secret(&self) -> &str {
        &self.wifi_secret
    }

    pub fn refresh(&mut self) -> bool {
        match refresh_state(self.scanning) {
            Ok(next) => {
                let snap = snapshot_from_parts(&next.fields, &next.access_points, &next.vpns);
                let changed = self.last_snapshot != snap;
                self.fields = next.fields;
                self.access_points = next.access_points;
                self.vpns = next.vpns;
                self.saved_ssids = next.saved_ssids;
                self.saved_wired_paths = next.wired_paths;
                self.saved_cellular_paths = next.cellular_paths;
                self.dns = next.dns;
                self.last_snapshot = snap;
                changed
            }
            Err(error) => {
                tracing::debug!(%error, "network NM refresh failed");
                let snap = json!({"available": false});
                let changed = self.last_snapshot != snap;
                self.fields.available = false;
                self.access_points.clear();
                self.vpns.clear();
                self.last_snapshot = snap;
                changed
            }
        }
    }

    pub fn set_enabled(&mut self, enabled: bool) {
        if let Err(error) = set_wireless_enabled(enabled) {
            tracing::warn!(%error, "set WirelessEnabled failed");
        }
        self.refresh();
    }

    pub fn scan(&mut self) {
        self.scanning = true;
        if let Err(error) = request_wifi_scan() {
            tracing::debug!(%error, "wifi scan request failed");
            self.scanning = false;
        }
        self.refresh();
        self.scanning = false;
    }

    pub fn connect(&mut self, path: &str, secret: Option<&str>) -> Result<(), String> {
        let ap = self
            .access_points
            .iter()
            .find(|ap| ap.path == path)
            .cloned()
            .ok_or_else(|| "Network is no longer available".to_string())?;
        if ap.enterprise {
            return Err("Enterprise networks need the enterprise action".into());
        }
        if ap.active {
            return Ok(());
        }
        if ap.secured && !ap.saved {
            let password = secret.filter(|text| !text.is_empty()).ok_or_else(|| "Password required".to_string())?;
            activate_access_point(&ap, Some(password))?;
            self.wifi_secret = password.to_string();
        } else if ap.saved {
            activate_saved_access_point(&ap)?;
        } else if ap.secured {
            return Err("Password required".into());
        } else {
            activate_access_point(&ap, None)?;
        }
        self.refresh();
        Ok(())
    }

    pub fn disconnect(&mut self) -> Result<(), String> {
        deactivate_primary()?;
        self.refresh();
        Ok(())
    }

    pub fn forget(&mut self, ssid: &str) -> Result<(), String> {
        delete_saved_ssid(ssid)?;
        self.refresh();
        Ok(())
    }

    pub fn set_vpn(&mut self, path: &str, active: bool) -> Result<(), String> {
        if !self.vpns.iter().any(|vpn| vpn.path == path) {
            return Err("VPN is no longer available".into());
        }
        if active {
            activate_vpn(path)?;
        } else {
            deactivate_vpn(path)?;
        }
        self.refresh();
        Ok(())
    }

    pub fn connect_enterprise(
        &mut self,
        path: &str,
        credentials: &EnterpriseCredentials,
    ) -> Result<(), String> {
        let ap = self
            .access_points
            .iter()
            .find(|ap| ap.path == path)
            .cloned()
            .ok_or_else(|| "Network is no longer available".to_string())?;
        if !ap.enterprise {
            return Err("Not an enterprise network".into());
        }
        if ap.key_management == 3 {
            return Err("WPA3-Enterprise 192-bit requires certificate-based EAP".into());
        }
        if !enterprise::password_auth_usable(ap.key_management) {
            return Err("Enterprise authentication is not supported for this network".into());
        }
        match enterprise::validate(credentials) {
            Validation::Ok => {}
            Validation::MissingIdentity => return Err("Identity is required".into()),
            Validation::MissingPassword => return Err("Password is required".into()),
            Validation::UnsupportedPhase2 => {
                return Err("Unsupported enterprise authentication method".into());
            }
            Validation::CaCertNotAbsolute => {
                return Err("CA certificate path must be absolute".into());
            }
        }
        if ap.active {
            return Ok(());
        }
        activate_enterprise_access_point(&ap, credentials)?;
        self.refresh();
        Ok(())
    }

    pub fn set_dns(&mut self, dns_override: DnsOverride) -> Result<(), String> {
        if !self.fields.connected {
            return Err("Connect to a network before changing DNS".into());
        }
        apply_dns_override(&dns_override)?;
        self.refresh();
        Ok(())
    }

    pub fn activate_wired(&mut self) -> Result<(), String> {
        if self.fields.kind == 2 && self.fields.connected {
            return Ok(());
        }
        activate_saved_profiles(&self.saved_wired_paths)?;
        self.refresh();
        Ok(())
    }

    pub fn activate_cellular(&mut self) -> Result<(), String> {
        if self.fields.cellular_active {
            return Ok(());
        }
        if self.saved_cellular_paths.is_empty() {
            return Err("Connection rejected".into());
        }
        activate_saved_profiles(&self.saved_cellular_paths)?;
        self.refresh();
        Ok(())
    }

    pub fn deactivate_cellular(&mut self) -> Result<(), String> {
        if self.saved_cellular_paths.is_empty() {
            return Err("Connection rejected".into());
        }
        deactivate_profiles(&self.saved_cellular_paths)?;
        self.refresh();
        Ok(())
    }
}

struct Refreshed {
    fields: NetworkFields,
    access_points: Vec<AccessPoint>,
    vpns: Vec<Vpn>,
    saved_ssids: HashSet<String>,
    wired_paths: Vec<String>,
    cellular_paths: Vec<String>,
    dns: Value,
}

fn refresh_state(scanning: bool) -> Result<Refreshed, String> {
    with_connection(|conn| refresh_state_on_conn(conn, scanning))
}

fn refresh_state_on_conn(conn: &Connection, scanning: bool) -> Result<Refreshed, String> {
    let nm_props = get_all(&conn, NM_PATH, NM_IFACE)?;
    let wireless_enabled = prop_bool(&nm_props, "WirelessEnabled").unwrap_or(false);

    let (saved_ssids, wired_paths, cellular_paths) = load_saved_connections(&conn)?;
    let wired_available = !wired_paths.is_empty();
    let cellular_available = !cellular_paths.is_empty();
    let cellular_active = cellular_is_active(&conn, &cellular_paths)?;
    let vpns = load_vpns(&conn)?;

    let device_paths = call_devices(&conn)?;
    let mut access_points = Vec::new();
    let mut primary_wifi: Option<WifiDeviceState> = None;
    let mut primary_ethernet: Option<EthernetDeviceState> = None;

    for path in device_paths {
        let device_props = get_all(&conn, &path, DEVICE_IFACE)?;
        let device_type = prop_u32(&device_props, "DeviceType").unwrap_or(0);
        let interface = prop_str(&device_props, "Interface").unwrap_or_default();
        let state = prop_u32(&device_props, "State").unwrap_or(0);
        let ip4_path = prop_object_path(&device_props, "Ip4Config").unwrap_or_default();

        if device_type == NM_DEVICE_TYPE_WIFI {
            if let Ok(wifi_props) = get_all(&conn, &path, DEVICE_WIFI_IFACE) {
                let active_ap = prop_object_path(&wifi_props, "ActiveAccessPoint").unwrap_or_default();
                let aps = load_access_points(conn, &path, &active_ap, &saved_ssids)?;
                access_points.extend(aps);
                let signal = active_ap_strength(&conn, &active_ap);
                let ssid = active_ap_ssid(&conn, &active_ap);
                let connected = state == NM_DEVICE_STATE_ACTIVATED;
                let resolving = state >= NM_DEVICE_STATE_PREPARE && state < NM_DEVICE_STATE_ACTIVATED;
                let candidate = WifiDeviceState {
                    interface,
                    connected,
                    resolving,
                    ssid,
                    signal,
                    ip4_path,
                };
                if primary_wifi.as_ref().map(|w| w.connected) != Some(true) || candidate.connected {
                    primary_wifi = Some(candidate);
                }
            }
        } else if device_type == NM_DEVICE_TYPE_ETHERNET {
            let connected = state == NM_DEVICE_STATE_ACTIVATED;
            let resolving = state >= NM_DEVICE_STATE_PREPARE && state < NM_DEVICE_STATE_ACTIVATED;
            let candidate = EthernetDeviceState {
                interface,
                connected,
                resolving,
                ip4_path,
            };
            if primary_ethernet.as_ref().map(|w| w.connected) != Some(true) || candidate.connected {
                primary_ethernet = Some(candidate);
            }
        }
    }

    access_points = dedupe_access_points(access_points);

    let wifi_interface = primary_wifi.as_ref().map(|w| w.interface.clone()).unwrap_or_default();
    let (fields, dns) = if let Some(wifi) = primary_wifi.filter(|w| w.connected || wireless_enabled) {
        if wifi.connected {
            (
                NetworkFields {
                    available: true,
                    enabled: wireless_enabled,
                    connected: true,
                    resolving: wifi.resolving,
                    scanning,
                    ssid: wifi.ssid,
                    signal: wifi.signal,
                    address: ipv4_address(&conn, &wifi.ip4_path),
                    interface: wifi.interface,
                    kind: 3,
                    cellular_active,
                    wired_available,
                    cellular_available,
                },
                read_dns(&conn),
            )
        } else {
            (
                NetworkFields {
                    available: true,
                    enabled: wireless_enabled,
                    connected: false,
                    resolving: false,
                    scanning,
                    ssid: String::new(),
                    signal: 0,
                    address: String::new(),
                    interface: wifi.interface,
                    kind: 3,
                    cellular_active,
                    wired_available,
                    cellular_available,
                },
                json!({"profile": 0, "servers": [], "nextdns": ""}),
            )
        }
    } else if let Some(eth) = primary_ethernet.filter(|e| e.connected) {
        (
            NetworkFields {
                available: true,
                enabled: wireless_enabled,
                connected: true,
                resolving: eth.resolving,
                scanning: false,
                ssid: String::new(),
                signal: 0,
                address: ipv4_address(&conn, &eth.ip4_path),
                interface: eth.interface,
                kind: 2,
                cellular_active,
                wired_available,
                cellular_available,
            },
            read_dns(&conn),
        )
    } else {
        (
            NetworkFields {
                available: true,
                enabled: wireless_enabled,
                connected: false,
                resolving: false,
                scanning,
                ssid: String::new(),
                signal: 0,
                address: String::new(),
                interface: wifi_interface,
                kind: if wireless_enabled { 3 } else { 0 },
                cellular_active,
                wired_available,
                cellular_available,
            },
            json!({"profile": 0, "servers": [], "nextdns": ""}),
        )
    };

    Ok(Refreshed {
        fields,
        access_points,
        vpns,
        saved_ssids,
        wired_paths,
        cellular_paths,
        dns,
    })
}

#[derive(Clone)]
struct WifiDeviceState {
    interface: String,
    connected: bool,
    resolving: bool,
    ssid: String,
    signal: u8,
    ip4_path: String,
}

struct EthernetDeviceState {
    interface: String,
    connected: bool,
    resolving: bool,
    ip4_path: String,
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
                &(NM_SERVICE,),
            )
            .map_err(|error| error.to_string())?;
        reply.body().deserialize::<String>().map_err(|error| error.to_string())
    })
    .ok()
}

fn with_connection<R>(operation: impl FnOnce(&Connection) -> Result<R, String>) -> Result<R, String> {
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

fn nm_call<B>(
    conn: &Connection,
    path: &str,
    iface: &str,
    method: &str,
    body: &B,
) -> Result<zbus::Message, String>
where
    B: serde::Serialize + zbus::zvariant::DynamicType,
{
    conn.call_method(Some(NM_SERVICE), path, Some(iface), method, body)
        .map_err(|error| error.to_string())
}

fn get_all(conn: &Connection, path: &str, iface: &str) -> Result<HashMap<String, OwnedValue>, String> {
    let reply = nm_call(conn, path, PROPS_IFACE, "GetAll", &(iface,))?;
    reply
        .body()
        .deserialize::<HashMap<String, OwnedValue>>()
        .map_err(|error| error.to_string())
}

fn call_devices(conn: &Connection) -> Result<Vec<String>, String> {
    let reply = nm_call(conn, NM_PATH, NM_IFACE, "GetDevices", &())?;
    let paths = reply
        .body()
        .deserialize::<Vec<OwnedObjectPath>>()
        .map_err(|error| error.to_string())?;
    Ok(paths.into_iter().map(|path| path.to_string()).collect())
}

fn load_access_points(
    conn: &Connection,
    device_path: &str,
    active_ap_path: &str,
    saved_ssids: &HashSet<String>,
) -> Result<Vec<AccessPoint>, String> {
    let reply = nm_call(conn, device_path, DEVICE_WIFI_IFACE, "GetAccessPoints", &())?;
    let paths = reply
        .body()
        .deserialize::<Vec<OwnedObjectPath>>()
        .map_err(|error| error.to_string())?;
    let mut out = Vec::new();
    for path in paths {
        let path_str = path.to_string();
        let props = get_all(conn, &path_str, AP_IFACE)?;
        let ssid = prop_bytes(&props, "Ssid")
            .map(|bytes| String::from_utf8_lossy(&bytes).to_string())
            .unwrap_or_default();
        if ssid.is_empty() {
            continue;
        }
        let strength = prop_u8(&props, "Strength").unwrap_or(0);
        let wpa = prop_u32(&props, "WpaFlags").unwrap_or(0);
        let rsn = prop_u32(&props, "RsnFlags").unwrap_or(0);
        let secured = wpa != NM80211_AP_SEC_NONE || rsn != NM80211_AP_SEC_NONE;
        let key_management = key_management_for(rsn);
        let enterprise = is_enterprise(key_management);
        let frequency = prop_u32(&props, "Frequency").unwrap_or(0);
        let active = !active_ap_path.is_empty() && path_str == active_ap_path;
        let saved = saved_ssids.contains(&ssid);
        out.push(AccessPoint {
            path: path_str,
            device_path: device_path.to_string(),
            ssid,
            strength,
            secured,
            enterprise,
            key_management,
            active,
            frequency,
            saved,
        });
    }
    Ok(out)
}

fn dedupe_access_points(aps: Vec<AccessPoint>) -> Vec<AccessPoint> {
    let mut deduped: Vec<AccessPoint> = Vec::new();
    for ap in aps {
        if let Some(existing) = deduped.iter_mut().find(|entry| entry.ssid == ap.ssid) {
            if ap.active && (!existing.active || ap.strength > existing.strength) {
                *existing = ap;
            } else if ap.active {
                existing.active = true;
            } else if !existing.active && ap.strength > existing.strength {
                existing.strength = ap.strength;
                existing.path = ap.path.clone();
                existing.device_path = ap.device_path.clone();
                existing.secured = ap.secured;
                existing.enterprise = ap.enterprise;
                existing.key_management = ap.key_management;
                existing.frequency = ap.frequency;
                existing.saved = ap.saved;
            }
            continue;
        }
        deduped.push(ap);
    }
    deduped.sort_by(|a, b| {
        b.active
            .cmp(&a.active)
            .then_with(|| b.strength.cmp(&a.strength))
    });
    deduped
}

fn load_saved_connections(
    conn: &Connection,
) -> Result<(HashSet<String>, Vec<String>, Vec<String>), String> {
    let reply = nm_call(conn, SETTINGS_PATH, SETTINGS_IFACE, "ListConnections", &())?;
    let paths = reply
        .body()
        .deserialize::<Vec<OwnedObjectPath>>()
        .map_err(|error| error.to_string())?;
    let mut ssids = HashSet::new();
    let mut wired_paths = Vec::new();
    let mut cellular_paths = Vec::new();
    for path in paths {
        let path_str = path.to_string();
        let settings = connection_settings(conn, &path_str)?;
        let conn_type = settings
            .get("connection")
            .and_then(|section| section.get("type"))
            .and_then(prop_value_str)
            .unwrap_or_default();
        match conn_type.as_str() {
            "802-3-ethernet" => wired_paths.push(path_str),
            "gsm" => cellular_paths.push(path_str),
            _ => {}
        }
        if let Some(bytes) = settings
            .get("802-11-wireless")
            .and_then(|section| section.get("ssid"))
            .and_then(prop_value_bytes)
        {
            let ssid = String::from_utf8_lossy(&bytes).to_string();
            if !ssid.is_empty() {
                ssids.insert(ssid);
            }
        }
    }
    wired_paths.sort();
    cellular_paths.sort();
    Ok((ssids, wired_paths, cellular_paths))
}

fn cellular_is_active(conn: &Connection, cellular_profiles: &[String]) -> Result<bool, String> {
    if cellular_profiles.is_empty() {
        return Ok(false);
    }
    let profiles: HashSet<String> = cellular_profiles.iter().cloned().collect();
    for active_path in active_connection_paths(conn)? {
        let props = get_all(conn, &active_path, ACTIVE_CONN_IFACE)?;
        let profile = prop_object_path(&props, "Connection").unwrap_or_default();
        if profiles.contains(&profile) {
            return Ok(true);
        }
    }
    Ok(false)
}

fn activate_saved_profiles(paths: &[String]) -> Result<(), String> {
    if paths.is_empty() {
        return Err("Connection rejected".into());
    }
    with_connection(|conn| {
        for path in paths {
            if nm_call(
                conn,
                NM_PATH,
                NM_IFACE,
                "ActivateConnection",
                &(
                    ObjectPath::from_str_unchecked(path),
                    ObjectPath::from_str_unchecked("/"),
                    ObjectPath::from_str_unchecked("/"),
                ),
            )
            .is_ok()
            {
                return Ok(());
            }
        }
        Err("Connection rejected".into())
    })
}

fn deactivate_profiles(paths: &[String]) -> Result<(), String> {
    if paths.is_empty() {
        return Err("Connection rejected".into());
    }
    let profiles: HashSet<String> = paths.iter().cloned().collect();
    with_connection(|conn| {
        for active_path in active_connection_paths(conn)? {
            let props = get_all(conn, &active_path, ACTIVE_CONN_IFACE)?;
            let profile = prop_object_path(&props, "Connection").unwrap_or_default();
            if profiles.contains(&profile) {
                nm_call(
                    conn,
                    NM_PATH,
                    NM_IFACE,
                    "DeactivateConnection",
                    &(ObjectPath::from_str_unchecked(&active_path),),
                )?;
            }
        }
        Ok(())
    })
}

fn load_vpns(conn: &Connection) -> Result<Vec<Vpn>, String> {
    let reply = nm_call(conn, SETTINGS_PATH, SETTINGS_IFACE, "ListConnections", &())?;
    let paths = reply
        .body()
        .deserialize::<Vec<OwnedObjectPath>>()
        .map_err(|error| error.to_string())?;
    let active = active_connection_paths(conn)?;
    let mut vpns = Vec::new();
    for path in paths {
        let path_str = path.to_string();
        let settings = connection_settings(conn, &path_str)?;
        let conn_type = settings
            .get("connection")
            .and_then(|section| section.get("type"))
            .and_then(prop_value_str)
            .unwrap_or_default();
        if conn_type != "vpn" && conn_type != "wireguard" {
            continue;
        }
        let name = settings
            .get("connection")
            .and_then(|section| section.get("id"))
            .and_then(prop_value_str)
            .unwrap_or_else(|| "VPN".to_string());
        let is_active = active.contains(&path_str);
        vpns.push(Vpn {
            path: path_str,
            name,
            active: is_active,
        });
    }
    Ok(vpns)
}

fn connection_settings(
    conn: &Connection,
    path: &str,
) -> Result<HashMap<String, HashMap<String, OwnedValue>>, String> {
    let reply = nm_call(conn, path, SETTINGS_CONN_IFACE, "GetSettings", &())?;
    reply
        .body()
        .deserialize()
        .map_err(|error| error.to_string())
}

fn active_connection_paths(conn: &Connection) -> Result<HashSet<String>, String> {
    let nm_props = get_all(conn, NM_PATH, NM_IFACE)?;
    let active = prop_object_path_array(&nm_props, "ActiveConnections");
    let mut paths = HashSet::new();
    for active_path in active {
        if let Ok(props) = get_all(conn, &active_path, ACTIVE_CONN_IFACE) {
            if let Some(conn_path) = prop_object_path(&props, "Connection") {
                if !conn_path.is_empty() && conn_path != "/" {
                    paths.insert(conn_path);
                }
            }
        }
    }
    Ok(paths)
}

fn read_dns(conn: &Connection) -> Value {
    let servers = match primary_active_connection(conn) {
        Ok(path) => dns_servers_for_connection(conn, &path),
        Err(_) => Vec::new(),
    };
    json!({
        "profile": if servers.is_empty() { 0 } else { 4 },
        "servers": servers,
        "nextdns": "",
    })
}

fn primary_active_connection(conn: &Connection) -> Result<String, String> {
    let nm_props = get_all(conn, NM_PATH, NM_IFACE)?;
    let primary = prop_object_path(&nm_props, "PrimaryConnection").unwrap_or_default();
    if !primary.is_empty() && primary != "/" {
        return Ok(primary);
    }
    let devices = call_devices(conn)?;
    for device in devices {
        let props = get_all(conn, &device, DEVICE_IFACE)?;
        let state = prop_u32(&props, "State").unwrap_or(0);
        if state == NM_DEVICE_STATE_ACTIVATED {
            if let Some(active) = prop_object_path(&props, "ActiveConnection") {
                if !active.is_empty() && active != "/" {
                    return Ok(active);
                }
            }
        }
    }
    Err("no active connection".into())
}

fn dns_servers_for_connection(conn: &Connection, active_path: &str) -> Vec<String> {
    let props = match get_all(conn, active_path, ACTIVE_CONN_IFACE) {
        Ok(value) => value,
        Err(_) => return Vec::new(),
    };
    let ip4_path = prop_object_path(&props, "Ip4Config").unwrap_or_default();
    if ip4_path.is_empty() || ip4_path == "/" {
        return Vec::new();
    }
    let ip4 = match get_all(conn, &ip4_path, IP4_IFACE) {
        Ok(value) => value,
        Err(_) => return Vec::new(),
    };
    prop_value_str_array(ip4.get("Nameservers")).unwrap_or_default()
}

fn ipv4_address(conn: &Connection, ip4_path: &str) -> String {
    if ip4_path.is_empty() || ip4_path == "/" {
        return String::new();
    }
    let props = get_all(conn, ip4_path, IP4_IFACE).ok();
    let props = match props {
        Some(value) => value,
        None => return String::new(),
    };
    if let Some(addresses) = props.get("AddressData") {
        if let Some(first) = prop_address_entry(addresses) {
            return first;
        }
    }
    String::new()
}

fn set_wireless_enabled(enabled: bool) -> Result<(), String> {
    with_connection(|conn| {
        nm_call(
            conn,
            NM_PATH,
            PROPS_IFACE,
            "Set",
            &(NM_IFACE, "WirelessEnabled", ZValue::Bool(enabled)),
        )?;
        Ok(())
    })
}

fn request_wifi_scan() -> Result<(), String> {
    with_connection(|conn| {
        for device in call_devices(conn)? {
            let props = get_all(conn, &device, DEVICE_IFACE)?;
        if prop_u32(&props, "DeviceType").unwrap_or(0) != NM_DEVICE_TYPE_WIFI {
            continue;
        }
            let _ = nm_call(
                conn,
                device.as_str(),
                DEVICE_WIFI_IFACE,
                "RequestScan",
                &(HashMap::<&str, &str>::new(),),
            );
        }
        Ok(())
    })
}

fn activate_saved_access_point(ap: &AccessPoint) -> Result<(), String> {
    with_connection(|conn| {
        nm_call(
            conn,
            NM_PATH,
            NM_IFACE,
            "ActivateConnection",
            &(
                ObjectPath::from_str_unchecked("/"),
                ObjectPath::from_str_unchecked(&ap.device_path),
                ObjectPath::from_str_unchecked(&ap.path),
            ),
        )?;
        Ok(())
    })
}

fn activate_enterprise_access_point(
    ap: &AccessPoint,
    credentials: &EnterpriseCredentials,
) -> Result<(), String> {
    with_connection(|conn| {
        let mut settings: HashMap<&str, HashMap<&str, ZValue<'_>>> = HashMap::new();
        let mut security = HashMap::new();
        security.insert("key-mgmt", ZValue::from(key_management_name(ap.key_management)));
        settings.insert("802-11-wireless-security", security);
        settings.insert("802-1x", enterprise::eap_settings(credentials));
        let options: HashMap<&str, ZValue<'_>> =
            HashMap::from([("persist", ZValue::from("memory"))]);
        let _ = nm_call(
            conn,
            NM_PATH,
            NM_IFACE,
            "AddAndActivateConnection2",
            &(
                settings.clone(),
                ObjectPath::from_str_unchecked(&ap.device_path),
                ObjectPath::from_str_unchecked(&ap.path),
                options,
            ),
        );
        nm_call(
            conn,
            NM_PATH,
            NM_IFACE,
            "AddAndActivateConnection",
            &(
                settings.clone(),
                ObjectPath::from_str_unchecked(&ap.device_path),
                ObjectPath::from_str_unchecked(&ap.path),
            ),
        )?;
        Ok(())
    })
}

fn apply_dns_override(dns_override: &DnsOverride) -> Result<(), String> {
    with_connection(|conn| {
        let active_path = primary_active_connection(conn)?;
        let active_props = get_all(conn, &active_path, ACTIVE_CONN_IFACE)?;
        let profile_path = prop_object_path(&active_props, "Connection")
            .filter(|path| !path.is_empty() && path != "/")
            .ok_or_else(|| "Active connection has no profile".to_string())?;
        let devices = prop_object_path_array(&active_props, "Devices");
        let device_path = devices
            .first()
            .cloned()
            .ok_or_else(|| "Active connection has no device".to_string())?;

        let automatic = dns_override.profile == 0;
        let servers = if dns_override.profile == 4 {
            dns_override.servers.clone()
        } else if automatic {
            Vec::new()
        } else {
            builtin_servers(dns_override.profile, &dns_override.nextdns_id)
        };

        let mut ipv4 = HashMap::new();
        let mut ipv6 = HashMap::new();
        ipv4.insert("ignore-auto-dns", ZValue::Bool(!automatic));
        ipv6.insert("ignore-auto-dns", ZValue::Bool(!automatic));
        if !automatic {
            let mut v4_dns: Vec<u32> = Vec::new();
            let mut v6_dns: Vec<Vec<u8>> = Vec::new();
            for server in &servers {
                let host = server.split('#').next().unwrap_or(server).trim();
                if let Ok(addr) = host.parse::<std::net::Ipv4Addr>() {
                    v4_dns.push(u32::from_be_bytes(addr.octets()));
                } else if let Ok(addr) = host.parse::<std::net::Ipv6Addr>() {
                    v6_dns.push(addr.octets().to_vec());
                }
            }
            ipv4.insert("dns", ZValue::from(v4_dns));
            ipv6.insert("dns", ZValue::from(v6_dns));
        }
        let settings: HashMap<&str, HashMap<&str, ZValue<'_>>> =
            HashMap::from([("ipv4", ipv4), ("ipv6", ipv6)]);
        let flags: u32 = 1;
        let args: HashMap<&str, ZValue<'_>> = HashMap::new();
        nm_call(
            conn,
            &profile_path,
            SETTINGS_CONN_IFACE,
            "Update2",
            &(settings, flags, args),
        )?;
        let empty: HashMap<&str, HashMap<&str, ZValue<'_>>> = HashMap::new();
        nm_call(
            conn,
            &device_path,
            DEVICE_IFACE,
            "Reapply",
            &(empty, 0u64, 0u32),
        )?;
        Ok(())
    })
}

fn activate_access_point(ap: &AccessPoint, psk: Option<&str>) -> Result<(), String> {
    with_connection(|conn| {
    let mut settings: HashMap<&str, HashMap<&str, ZValue<'_>>> = HashMap::new();
    if ap.secured {
        let mut security = HashMap::new();
        security.insert("key-mgmt", ZValue::from(key_management_name(ap.key_management)));
        if let Some(password) = psk {
            security.insert("psk", ZValue::from(password));
        }
        settings.insert("802-11-wireless-security", security);
    }
    let options: HashMap<&str, ZValue<'_>> = HashMap::from([("persist", ZValue::from("memory"))]);
    let result = nm_call(
        conn,
        NM_PATH,
        NM_IFACE,
        "AddAndActivateConnection2",
        &(
            settings.clone(),
            ObjectPath::from_str_unchecked(&ap.device_path),
            ObjectPath::from_str_unchecked(&ap.path),
            options,
        ),
    );
    if result.is_err() {
        nm_call(
            conn,
            NM_PATH,
            NM_IFACE,
            "AddAndActivateConnection",
            &(
                settings.clone(),
                ObjectPath::from_str_unchecked(&ap.device_path),
                ObjectPath::from_str_unchecked(&ap.path),
            ),
        )?;
    }
        Ok(())
    })
}

fn deactivate_primary() -> Result<(), String> {
    with_connection(|conn| {
        let active = primary_active_connection(conn)?;
        nm_call(
            conn,
            NM_PATH,
            NM_IFACE,
            "DeactivateConnection",
            &(ObjectPath::from_str_unchecked(&active),),
        )?;
        Ok(())
    })
}

fn delete_saved_ssid(ssid: &str) -> Result<(), String> {
    with_connection(|conn| {
    let reply = nm_call(conn, SETTINGS_PATH, SETTINGS_IFACE, "ListConnections", &())?;
    let paths = reply
        .body()
        .deserialize::<Vec<OwnedObjectPath>>()
        .map_err(|error| error.to_string())?;
    for path in paths {
        let path_str = path.to_string();
        let settings = connection_settings(conn, &path_str)?;
        let candidate = settings
            .get("802-11-wireless")
            .and_then(|section| section.get("ssid"))
            .and_then(prop_value_bytes)
            .map(|bytes| String::from_utf8_lossy(&bytes).to_string())
            .unwrap_or_default();
        if candidate == ssid {
            nm_call(conn, &path_str, SETTINGS_CONN_IFACE, "Delete", &())?;
            }
        }
        Ok(())
    })
}

fn activate_vpn(path: &str) -> Result<(), String> {
    with_connection(|conn| {
        let device = call_devices(conn)?
        .into_iter()
            .find(|device| {
                get_all(conn, device, DEVICE_IFACE)
                    .ok()
                    .and_then(|props| prop_u32(&props, "DeviceType"))
                    == Some(NM_DEVICE_TYPE_ETHERNET)
            })
            .or_else(|| call_devices(conn).ok()?.into_iter().next())
            .ok_or_else(|| "No network device for VPN".to_string())?;
        nm_call(
            conn,
            NM_PATH,
            NM_IFACE,
            "ActivateConnection",
            &(
                ObjectPath::from_str_unchecked(path),
                ObjectPath::from_str_unchecked(device.as_str()),
                ObjectPath::from_str_unchecked("/"),
            ),
        )?;
        Ok(())
    })
}

fn deactivate_vpn(path: &str) -> Result<(), String> {
    with_connection(|conn| {
        let nm_props = get_all(conn, NM_PATH, NM_IFACE)?;
        for active_path in prop_object_path_array(&nm_props, "ActiveConnections") {
            let props = get_all(conn, &active_path, ACTIVE_CONN_IFACE)?;
            if prop_object_path(&props, "Connection").as_deref() == Some(path) {
                nm_call(
                    conn,
                    NM_PATH,
                    NM_IFACE,
                    "DeactivateConnection",
                    &(ObjectPath::from_str_unchecked(&active_path),),
                )?;
            }
        }
        Ok(())
    })
}

fn active_ap_strength(conn: &Connection, ap_path: &str) -> u8 {
    if ap_path.is_empty() || ap_path == "/" {
        return 0;
    }
    get_all(conn, ap_path, AP_IFACE)
        .ok()
        .and_then(|props| prop_u8(&props, "Strength"))
        .unwrap_or(0)
}

fn active_ap_ssid(conn: &Connection, ap_path: &str) -> String {
    if ap_path.is_empty() || ap_path == "/" {
        return String::new();
    }
    get_all(conn, ap_path, AP_IFACE)
        .ok()
        .and_then(|props| prop_bytes(&props, "Ssid"))
        .map(|bytes| String::from_utf8_lossy(&bytes).to_string())
        .unwrap_or_default()
}

fn key_management_for(rsn_flags: u32) -> u32 {
    if (rsn_flags & NM80211_AP_SEC_KEY_MGMT_EAP_SUITE_B) != 0 {
        3
    } else if (rsn_flags & NM80211_AP_SEC_KEY_MGMT_8021X) != 0 {
        2
    } else if (rsn_flags & NM80211_AP_SEC_KEY_MGMT_SAE) != 0 {
        1
    } else {
        0
    }
}

fn is_enterprise(key_management: u32) -> bool {
    key_management == 2 || key_management == 3
}

fn key_management_name(key_management: u32) -> &'static str {
    match key_management {
        1 => "sae",
        2 => "wpa-eap",
        3 => "wpa-eap-suite-b-192",
        _ => "wpa-psk",
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
        ZValue::I32(number) if *number >= 0 => Some(*number as u32),
        _ => None,
    })
}

fn prop_u8(map: &HashMap<String, OwnedValue>, key: &str) -> Option<u8> {
    prop_u32(map, key).and_then(|number| u8::try_from(number).ok())
}

fn prop_str(map: &HashMap<String, OwnedValue>, key: &str) -> Option<String> {
    map.get(key).and_then(|value| match &**value {
        ZValue::Str(text) => Some(text.to_string()),
        _ => None,
    })
}

fn prop_bytes(map: &HashMap<String, OwnedValue>, key: &str) -> Option<Vec<u8>> {
    map.get(key).and_then(|value| match &**value {
        ZValue::Array(bytes) => {
            let mut out = Vec::new();
            for entry in bytes.iter() {
                if let ZValue::U8(byte) = entry {
                    out.push(*byte);
                }
            }
            Some(out)
        }
        _ => None,
    })
}

fn prop_object_path(map: &HashMap<String, OwnedValue>, key: &str) -> Option<String> {
    map.get(key).and_then(|value| match &**value {
        ZValue::ObjectPath(path) => Some(path.to_string()),
        _ => None,
    })
}

fn prop_object_path_array(map: &HashMap<String, OwnedValue>, key: &str) -> Vec<String> {
    map.get(key)
        .and_then(|value| match &**value {
            ZValue::Array(entries) => {
                let mut out = Vec::new();
                for entry in entries.iter() {
                    if let ZValue::ObjectPath(path) = entry {
                        out.push(path.to_string());
                    }
                }
                Some(out)
            }
            _ => None,
        })
        .unwrap_or_default()
}

fn prop_value_bytes(value: &OwnedValue) -> Option<Vec<u8>> {
    match &**value {
        ZValue::Array(bytes) => {
            let mut out = Vec::new();
            for entry in bytes.iter() {
                if let ZValue::U8(byte) = entry {
                    out.push(*byte);
                }
            }
            Some(out)
        }
        _ => None,
    }
}

fn prop_value_str(value: &OwnedValue) -> Option<String> {
    match &**value {
        ZValue::Str(text) => Some(text.to_string()),
        _ => None,
    }
}

fn prop_value_str_array(value: Option<&OwnedValue>) -> Option<Vec<String>> {
    value.and_then(|owned| match &**owned {
        ZValue::Array(entries) => {
            let mut out = Vec::new();
            for entry in entries.iter() {
                if let ZValue::Str(text) = entry {
                    out.push(text.to_string());
                }
            }
            Some(out)
        }
        _ => None,
    })
}

fn prop_address_entry(value: &OwnedValue) -> Option<String> {
    match &**value {
        ZValue::Array(entries) => entries.first().and_then(|entry| match entry {
            ZValue::Dict(dict) => {
                for (key, value) in dict.iter() {
                    if matches!(key, ZValue::Str(text) if text == "address") {
                        return match value {
                            ZValue::Str(text) => Some(text.to_string()),
                            _ => None,
                        };
                    }
                }
                None
            }
            _ => None,
        }),
        _ => None,
    }
}
