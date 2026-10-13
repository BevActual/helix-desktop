use std::collections::HashMap;
use std::sync::Arc;

use tokio::sync::Mutex;
use zbus::object_server::SignalEmitter;
use zbus::zvariant::OwnedValue;
use zbus::interface;

use crate::dbus_value::json_to_owned;
use crate::state::State;

pub struct Displays {
    pub state: Arc<Mutex<State>>,
}

fn snapshot_map(value: &serde_json::Value) -> HashMap<String, OwnedValue> {
    let mut map = HashMap::new();
    if let Some(object) = value.as_object() {
        for (key, nested) in object {
            if let Some(owned) = json_to_owned(nested) {
                map.insert(key.clone(), owned);
            }
        }
    }
    map
}

/// Mock inventory matching C++ `DisplayInventory` shape (no niri query yet).
fn mock_snapshot(desktop_registered: bool) -> serde_json::Value {
    if !desktop_registered {
        return serde_json::json!({
            "available": false,
            "outputs": [],
            "capabilities": [],
            "reason": "Display inventory requires an active Helix desktop and its compositor connection.",
        });
    }
    serde_json::json!({
        "available": true,
        "outputs": [{
            "name": "eDP-1",
            "label": "Built-in Display",
            "enabled": true,
            "width": 1920,
            "height": 1080,
            "refresh-millihertz": 60000,
            "scale": 1.0,
            "x": 0,
            "y": 0,
            "logical-width": 1920,
            "logical-height": 1080,
            "transform": "Normal",
            "vrr-supported": false,
            "vrr-enabled": false,
        }],
        "capabilities": ["inventory"],
    })
}

#[interface(name = "org.helix.Desktop.Displays1")]
impl Displays {
    async fn get_snapshot(&self) -> HashMap<String, OwnedValue> {
        let state = self.state.lock().await;
        snapshot_map(&mock_snapshot(state.desktop.is_some()))
    }

    #[zbus(signal)]
    async fn changed(emitter: &SignalEmitter<'_>) -> zbus::Result<()>;
}
