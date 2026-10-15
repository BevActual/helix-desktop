//! Shared bus harness for `configuration_contract` integration tests.

use helix_desktop_api::{Desktop, Domain, BUS_NAME};
use serde_json::json;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::Duration;

/// Minimal nested contract seed (`theme.mode` only).
pub const HELIX_TEST_CONFIG_TOML: &str = r#"[theme]
mode = "dark"
"#;

pub fn contract_enabled() -> bool {
    std::env::var("HELIX_RUN_CONFIGURATION_CONTRACT")
        .ok()
        .is_some_and(|value| matches!(value.as_str(), "1" | "true" | "yes"))
}

pub fn helix_desktop_bin() -> PathBuf {
    if let Ok(path) = std::env::var("HELIX_SERVICE_BIN") {
        if !path.is_empty() {
            return PathBuf::from(path);
        }
    }
    if let Ok(path) = std::env::var("HELIX_DESKTOP_BIN") {
        if !path.is_empty() {
            return PathBuf::from(path);
        }
    }
    PathBuf::from("target/fast/helix-desktop")
}

pub fn string_list(row: &serde_json::Value) -> Vec<String> {
    row.get("value")
        .and_then(|value| value.as_array())
        .map(|items| {
            items
                .iter()
                .filter_map(|item| item.as_str().map(str::to_string))
                .collect()
        })
        .unwrap_or_default()
}

pub fn json_array_value(row: &serde_json::Value) -> serde_json::Value {
    row.get("value")
        .cloned()
        .unwrap_or_else(|| json!([]))
}

pub struct Session {
    pub root: PathBuf,
    child: Child,
    bin: PathBuf,
    config_home: PathBuf,
    empty_share: PathBuf,
    session_bus: String,
    marker: String,
}

pub struct FakeBluez {
    child: Child,
}

pub fn helix_fake_bluez_bin() -> Option<PathBuf> {
    if let Ok(path) = std::env::var("HELIX_FAKE_BLUEZ_BIN") {
        if !path.is_empty() {
            let path = PathBuf::from(path);
            if path.is_file() {
                return Some(path);
            }
        }
    }
    let fast = helix_desktop_bin()
        .parent()
        .map(|dir| dir.join("helix-fake-bluez"));
    fast.filter(|path| path.is_file())
}

impl FakeBluez {
    /// Spawns `helix-fake-bluez` on the private session bus (replaces C++ `--fake-bluez`).
    pub fn start() -> Option<Self> {
        let path = helix_fake_bluez_bin()?;
        let child = Command::new(&path)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .expect("spawn helix-fake-bluez");
        Some(Self { child })
    }
}

impl Drop for FakeBluez {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

impl Session {
    pub fn start() -> Self {
        let root = std::env::temp_dir().join(format!(
            "helix-configuration-contract-{}",
            std::process::id()
        ));
        let config_home = root.join("config");
        let helix_cfg = config_home.join("helix");
        std::fs::create_dir_all(&helix_cfg).expect("config dir");
        let empty_share = root.join("empty-share");
        std::fs::create_dir_all(&empty_share).expect("empty share");
        std::fs::write(helix_cfg.join("config.toml"), HELIX_TEST_CONFIG_TOML)
            .expect("write config");

        let bin = helix_desktop_bin();
        assert!(
            bin.is_file(),
            "helix-desktop binary missing: {}",
            bin.display()
        );

        let session_bus = std::env::var("DBUS_SESSION_BUS_ADDRESS")
            .expect("DBUS_SESSION_BUS_ADDRESS (use dbus-run-session)");

        let marker = root.to_string_lossy().to_string();
        let child = Command::new(&bin)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .env("HELIX_DESKTOP_CONTRACT_TEST", &marker)
            .env("HELIX_CONFIG_HOME", &config_home)
            .env_remove("HELIX_STATE_HOME")
            .env("XDG_STATE_HOME", root.join("state"))
            .env("XDG_CACHE_HOME", root.join("cache"))
            .env("XDG_DATA_HOME", root.join("data"))
            .env("XDG_DATA_DIRS", &empty_share)
            .env("GSETTINGS_BACKEND", "memory")
            .env("DBUS_SYSTEM_BUS_ADDRESS", &session_bus)
            .env("PIPEWIRE_REMOTE", "helix-test-no-audio")
            .env("HELIX_DESKTOP_MOCK_AUDIO", "1")
            .env("HELIX_DESKTOP_MOCK_DISPLAYS", "1")
            .env_remove("WAYLAND_DISPLAY")
            .env_remove("DISPLAY")
            .env_remove("NIRI_SOCKET")
            .spawn()
            .expect("spawn helix-desktop");

        Self {
            root,
            child,
            bin,
            config_home,
            empty_share,
            session_bus,
            marker,
        }
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    pub fn config_toml(&self) -> PathBuf {
        self.config_home.join("helix/config.toml")
    }

    pub async fn restart_service(&mut self, desktop: &Desktop) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        self.child = Command::new(&self.bin)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .env("HELIX_DESKTOP_CONTRACT_TEST", &self.marker)
            .env("HELIX_CONFIG_HOME", &self.config_home)
            .env_remove("HELIX_STATE_HOME")
            .env("XDG_STATE_HOME", self.root.join("state"))
            .env("XDG_CACHE_HOME", self.root.join("cache"))
            .env("XDG_DATA_HOME", self.root.join("data"))
            .env("XDG_DATA_DIRS", &self.empty_share)
            .env("GSETTINGS_BACKEND", "memory")
            .env("DBUS_SYSTEM_BUS_ADDRESS", &self.session_bus)
            .env("PIPEWIRE_REMOTE", "helix-test-no-audio")
            .env("HELIX_DESKTOP_MOCK_AUDIO", "1")
            .env("HELIX_DESKTOP_MOCK_DISPLAYS", "1")
            .env_remove("WAYLAND_DISPLAY")
            .env_remove("DISPLAY")
            .env_remove("NIRI_SOCKET")
            .spawn()
            .expect("respawn helix-desktop");
        wait_ready(desktop).await;
    }

    pub fn settings_toml(&self) -> PathBuf {
        self.root.join("state/helix/settings.toml")
    }

    pub async fn wait_for_settings_file(&self) {
        for _ in 0..80 {
            if self.settings_toml().is_file() {
                return;
            }
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        panic!("settings.toml was not created");
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        let _ = std::fs::remove_dir_all(&self.root);
    }
}

async fn wait_for_bus_name_absent() {
    let connection = zbus::Connection::session().await.expect("session bus");
    let dbus = zbus::fdo::DBusProxy::new(&connection)
        .await
        .expect("DBus proxy");
    for _ in 0..200 {
        let name = zbus::names::BusName::try_from(BUS_NAME).expect("bus name");
        if !dbus.name_has_owner(name).await.unwrap_or(false) {
            return;
        }
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
    panic!(
        "{BUS_NAME} already owned on this session bus (run via tools/nested/helix-desktop-configuration-contract.sh; unset HELIX_STATE_HOME if set)"
    );
}

async fn wait_for_bus_name() {
    let connection = zbus::Connection::session().await.expect("session bus");
    let dbus = zbus::fdo::DBusProxy::new(&connection)
        .await
        .expect("DBus proxy");
    for _ in 0..200 {
        let name = zbus::names::BusName::try_from(BUS_NAME).expect("bus name");
        if dbus.name_has_owner(name).await.unwrap_or(false) {
            return;
        }
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
    panic!("{BUS_NAME} never appeared on the session bus");
}

async fn wait_ready(desktop: &Desktop) {
    for _ in 0..200 {
        if desktop
            .snapshot(Domain::Configuration)
            .await
            .is_ok_and(|snap| snap.get("available").and_then(|v| v.as_bool()) == Some(true))
        {
            return;
        }
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
    panic!("helix-desktop did not become ready");
}

pub async fn boot() -> (Session, Desktop) {
    // Match nested runner: no host .service files on this private session bus.
    std::env::set_var("XDG_DATA_DIRS", "/var/empty");
    std::env::remove_var("HELIX_STATE_HOME");
    wait_for_bus_name_absent().await;
    let session = Session::start();
    wait_for_bus_name().await;
    let desktop = Desktop::session().await.expect("connect");
    wait_ready(&desktop).await;
    let snap = desktop.snapshot(Domain::Configuration).await.expect("snapshot");
    assert_eq!(
        snap.get("implementation").and_then(|v| v.as_str()),
        Some("rust"),
        "session bus must talk to the test helix-desktop binary, not a dbus-activated install"
    );
    let want_marker = session.root().to_string_lossy();
    assert_eq!(
        snap.get("contract-marker").and_then(|v| v.as_str()),
        Some(want_marker.as_ref()),
        "Configuration1 must be served by this test's helix-desktop child"
    );
    let want_settings = session.settings_toml().to_string_lossy().into_owned();
    assert_eq!(
        snap.get("settings-toml-path").and_then(|v| v.as_str()),
        Some(want_settings.as_str()),
        "helix-desktop must persist overrides where the harness expects settings.toml"
    );
    (session, desktop)
}

pub fn sample_notification_rule() -> serde_json::Value {
    json!({
        "name": "test-first",
        "enabled": true,
        "match": "test-app",
        "match_content": "urgent",
        "show_toast": false,
        "save_history": true,
        "play_sound": true,
        "bypass_dnd": false,
        "allow_permanent": true,
        "allowed_urgencies": ["critical"],
        "override_duration": 1234
    })
}
