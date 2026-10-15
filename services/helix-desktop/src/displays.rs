use std::collections::HashMap;
use std::sync::Arc;

use tokio::sync::Mutex;
use zbus::interface;
use zbus::object_server::SignalEmitter;
use zbus::zvariant::OwnedValue;

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

#[interface(name = "org.helix.Desktop.Displays1")]
impl Displays {
    async fn get_snapshot(&self) -> HashMap<String, OwnedValue> {
        let state = self.state.lock().await;
        let desktop = state.desktop.is_some();
        snapshot_map(&state.displays.snapshot(desktop))
    }

    #[zbus(signal)]
    async fn changed(emitter: &SignalEmitter<'_>) -> zbus::Result<()>;
}
