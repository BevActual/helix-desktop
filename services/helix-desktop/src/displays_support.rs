use zbus::Connection;

pub const OBJECT_PATH: &str = "/org/helix/Desktop";
const DISPLAYS_INTERFACE: &str = "org.helix.Desktop.Displays1";

pub async fn emit_displays_changed(conn: &Connection) {
    let _ = conn
        .emit_signal(
            None::<&str>,
            OBJECT_PATH,
            DISPLAYS_INTERFACE,
            "Changed",
            &(),
        )
        .await;
}
