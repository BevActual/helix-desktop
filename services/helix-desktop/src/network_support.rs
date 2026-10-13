//! D-Bus helpers for Network1 background refresh.

use zbus::Connection;

pub const OBJECT_PATH: &str = "/org/helix/Desktop";
pub const NETWORK_INTERFACE: &str = "org.helix.Desktop.Network1";
const CONFIG_INTERFACE: &str = "org.helix.Desktop.Configuration1";

pub async fn emit_network_changed(conn: &Connection) {
    let _ = conn
        .emit_signal(
            None::<&str>,
            OBJECT_PATH,
            NETWORK_INTERFACE,
            "Changed",
            &(),
        )
        .await;
}

pub async fn emit_configuration_changed(conn: &Connection) {
    let _ = conn
        .emit_signal(
            None::<&str>,
            OBJECT_PATH,
            CONFIG_INTERFACE,
            "Changed",
            &(),
        )
        .await;
}
