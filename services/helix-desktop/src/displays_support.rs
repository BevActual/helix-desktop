use serde_json::Value;
use zbus::Connection;

pub const OBJECT_PATH: &str = "/org/helix/Desktop";
const DISPLAYS_INTERFACE: &str = "org.helix.Desktop.Displays1";

pub fn output_names(snapshot: &Value) -> Vec<String> {
    snapshot
        .get("outputs")
        .and_then(Value::as_array)
        .map(|outputs| {
            outputs
                .iter()
                .filter_map(|output| output.get("name").and_then(Value::as_str))
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default()
}

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
