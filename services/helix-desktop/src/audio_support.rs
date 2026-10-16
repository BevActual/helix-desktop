//! D-Bus helpers for Audio1 background refresh.

use zbus::Connection;

pub const OBJECT_PATH: &str = "/org/helix/Desktop";
const AUDIO_INTERFACE: &str = "org.helix.Desktop.Audio1";
const CONFIG_INTERFACE: &str = "org.helix.Desktop.Configuration1";

pub async fn emit_audio_changed(conn: &Connection) {
    let _ = conn
        .emit_signal(None::<&str>, OBJECT_PATH, AUDIO_INTERFACE, "Changed", &())
        .await;
}

pub async fn emit_input_level(
    conn: &Connection,
    destination: &str,
    source: u32,
    peak: f64,
    available: bool,
) {
    let _ = conn
        .emit_signal(
            Some(destination),
            OBJECT_PATH,
            AUDIO_INTERFACE,
            "InputLevel",
            &(source, peak, available),
        )
        .await;
}

pub async fn emit_configuration_changed(conn: &Connection) {
    let _ = conn
        .emit_signal(None::<&str>, OBJECT_PATH, CONFIG_INTERFACE, "Changed", &())
        .await;
}
