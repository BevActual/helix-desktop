use std::collections::HashMap;
use std::sync::Arc;

use serde_json::Value as JsonValue;
use tokio::sync::Mutex;
use zbus::fdo;
use zbus::message::Header;
use zbus::object_server::SignalEmitter;
use zbus::zvariant::{OwnedValue, Value};
use zbus::{interface, Connection};

use crate::dbus_value::{json_to_owned, owned_bool, owned_str, owned_to_json};
use crate::displays_support;
use crate::peer::resolve_peer;
use crate::state::State;

fn display_output_names(state: &State, include_desktop: bool) -> Vec<String> {
    if !include_desktop {
        return Vec::new();
    }
    displays_support::output_names(&state.displays.snapshot(true))
}

fn gamma_available(state: &State) -> bool {
    state
        .desktop_capabilities
        .get("gamma-control")
        .copied()
        .unwrap_or(false)
}

fn require_setting_access(state: &State, id: &str) -> fdo::Result<()> {
    if id.starts_with("ai.") {
        return Err(fdo::Error::InvalidArgs(
            helix_config::settings::MOBIUS_ROUTING_MESSAGE.into(),
        ));
    }
    if state.desktop.is_none() && !helix_config::settings::is_portable_setting(id) {
        return Err(fdo::Error::InvalidArgs(
            "This setting requires Helix desktop".into(),
        ));
    }
    if id.starts_with("nightlight.") && !gamma_available(state) {
        return Err(fdo::Error::InvalidArgs(
            "Night Light is unavailable on this desktop".into(),
        ));
    }
    Ok(())
}

pub struct Configuration {
    pub state: Arc<Mutex<State>>,
}

fn value_from_zvariant(value: &Value<'_>) -> Option<serde_json::Value> {
    match value {
        Value::Bool(flag) => Some(serde_json::Value::Bool(*flag)),
        Value::F64(number) => Some(serde_json::json!(*number)),
        Value::I64(number) => Some(serde_json::json!(*number as f64)),
        Value::U64(number) => Some(serde_json::json!(*number as f64)),
        Value::I32(number) => Some(serde_json::json!(f64::from(*number))),
        Value::U32(number) => Some(serde_json::json!(f64::from(*number))),
        Value::Str(text) => Some(serde_json::Value::String(text.to_string())),
        Value::Value(inner) => value_from_zvariant(inner),
        _ => None,
    }
}

fn require_desktop(state: &State, sender: &str) -> fdo::Result<()> {
    match &state.desktop {
        Some(owner) if owner == sender => Ok(()),
        Some(_) => Err(fdo::Error::AccessDenied(
            "Only the registered shell may perform this action".into(),
        )),
        None => Err(fdo::Error::Failed("No Helix desktop is registered".into())),
    }
}

fn json_array_from_owned(items: &[OwnedValue]) -> fdo::Result<JsonValue> {
    let mut out = Vec::with_capacity(items.len());
    for item in items {
        out.push(owned_to_json(item).ok_or_else(|| {
            fdo::Error::InvalidArgs("Ordered list values must be JSON-compatible".into())
        })?);
    }
    Ok(JsonValue::Array(out))
}

/// Structural equality where `1234` and `1234.0` match: D-Bus integers arrive as JSON floats
/// while stored lists hold integers.
fn json_equivalent(a: &JsonValue, b: &JsonValue) -> bool {
    match (a, b) {
        (JsonValue::Number(x), JsonValue::Number(y)) => match (x.as_f64(), y.as_f64()) {
            (Some(x), Some(y)) => x == y,
            _ => x == y,
        },
        (JsonValue::Array(x), JsonValue::Array(y)) => {
            x.len() == y.len() && x.iter().zip(y).all(|(x, y)| json_equivalent(x, y))
        }
        (JsonValue::Object(x), JsonValue::Object(y)) => {
            x.len() == y.len()
                && x.iter()
                    .all(|(k, v)| y.get(k).is_some_and(|w| json_equivalent(v, w)))
        }
        _ => a == b,
    }
}

/// Any client of the person's session may edit an ordered list, as any may change a scalar setting with `Update`: Settings is its own
/// application. A Helix desktop (the shell) must be registered, since these lists configure it.
fn apply_ordered_list_update(
    state: &mut State,
    id: &str,
    expected: &[OwnedValue],
    value: &[OwnedValue],
) -> fdo::Result<()> {
    use helix_config::ordered_lists;
    if state.desktop.is_none() {
        return Err(fdo::Error::InvalidArgs(
            "Ordered desktop lists require Helix desktop".into(),
        ));
    }
    let display_names = display_output_names(state, true);
    let catalog = state.config.full_catalog(true, &display_names);
    let entry = ordered_lists::catalog_entry(&catalog, id)
        .ok_or_else(|| fdo::Error::InvalidArgs("Unknown ordered desktop list".into()))?;
    let type_name = entry
        .get("type")
        .and_then(|value| value.as_str())
        .ok_or_else(|| fdo::Error::InvalidArgs("Unknown ordered desktop list".into()))?;
    if !ordered_lists::supports_ordered_type(type_name) {
        return Err(fdo::Error::InvalidArgs(
            "Unknown ordered desktop list".into(),
        ));
    }
    let expected_json = json_array_from_owned(expected)?;
    let value_json = json_array_from_owned(value)?;
    let current =
        ordered_lists::current_value(&state.config, &catalog, id).unwrap_or(JsonValue::Null);
    if !json_equivalent(&current, &expected_json) {
        return Err(fdo::Error::InvalidArgs(
            "The list changed. Refresh and try again".into(),
        ));
    }
    let validated = ordered_lists::validate_update(&catalog, id, &value_json)
        .map_err(fdo::Error::InvalidArgs)?;
    if type_name == "application-list" {
        state
            .config
            .set(id, validated)
            .map_err(fdo::Error::InvalidArgs)?;
    } else {
        state
            .config
            .apply_ordered_list(id, validated)
            .map_err(fdo::Error::InvalidArgs)?;
    }
    Ok(())
}

/// `os` (os-release PRETTY_NAME), `kernel` and `architecture` (uname), as the C++ service reports.
fn system_info() -> &'static serde_json::Value {
    static INFO: std::sync::OnceLock<serde_json::Value> = std::sync::OnceLock::new();
    INFO.get_or_init(|| {
        let pretty = ["/etc/os-release", "/usr/lib/os-release"]
            .iter()
            .find_map(|path| {
                std::fs::read_to_string(path)
                    .ok()?
                    .lines()
                    .find_map(|line| {
                        let value = line.strip_prefix("PRETTY_NAME=")?;
                        Some(
                            value
                                .trim()
                                .trim_matches(|c| c == '"' || c == '\'')
                                .to_string(),
                        )
                    })
            });
        let mut details = serde_json::Map::new();
        details.insert(
            "os".into(),
            serde_json::Value::String(pretty.unwrap_or_else(|| "Linux".into())),
        );
        // SAFETY: `uname` fills a plain-data struct; fields are NUL-terminated C strings.
        unsafe {
            let mut name: libc::utsname = std::mem::zeroed();
            if libc::uname(&mut name) == 0 {
                let text = |field: &[libc::c_char]| {
                    std::ffi::CStr::from_ptr(field.as_ptr())
                        .to_string_lossy()
                        .into_owned()
                };
                details.insert(
                    "kernel".into(),
                    serde_json::Value::String(text(&name.release)),
                );
                details.insert(
                    "architecture".into(),
                    serde_json::Value::String(text(&name.machine)),
                );
            }
        }
        serde_json::Value::Object(details)
    })
}

#[interface(name = "org.helix.Desktop.Configuration1")]
impl Configuration {
    async fn get_snapshot(&self) -> HashMap<String, OwnedValue> {
        let state = self.state.lock().await;
        let mut map = HashMap::new();
        map.insert("available".into(), owned_bool(true));
        map.insert("helix-desktop".into(), owned_bool(state.desktop.is_some()));
        map.insert("audio".into(), owned_bool(state.audio.is_available()));
        map.insert("network".into(), owned_bool(state.network.is_available()));
        map.insert(
            "bluetooth".into(),
            owned_bool(state.bluetooth.is_available()),
        );
        map.insert("implementation".into(), owned_str("rust"));
        if let Ok(marker) = std::env::var("HELIX_DESKTOP_CONTRACT_TEST") {
            map.insert("contract-marker".into(), owned_str(&marker));
            let settings_path = state
                .config
                .paths()
                .settings_toml
                .to_string_lossy()
                .into_owned();
            map.insert("settings-toml-path".into(), owned_str(&settings_path));
        }
        map.insert(
            "settings".into(),
            owned_bool(!state.config.catalog().is_empty()),
        );
        let caps: serde_json::Map<String, serde_json::Value> = state
            .desktop_capabilities
            .iter()
            .map(|(key, value)| (key.clone(), serde_json::Value::Bool(*value)))
            .collect();
        if let Some(owned) = json_to_owned(&serde_json::Value::Object(caps)) {
            map.insert("desktop-capabilities".into(), owned);
        }
        if let Some(owned) = json_to_owned(system_info()) {
            map.insert("system".into(), owned);
        }
        map
    }

    async fn get_settings(&self) -> Vec<OwnedValue> {
        let state = self.state.lock().await;
        let include_desktop = state.desktop.is_some();
        let display_names = display_output_names(&state, include_desktop);
        let gamma = gamma_available(&state);
        state
            .config
            .full_catalog(include_desktop, &display_names)
            .iter()
            .filter(|row| {
                // Night Light rows exist only when the compositor offers gamma control.
                gamma
                    || !row
                        .get("id")
                        .and_then(|id| id.as_str())
                        .is_some_and(|id| id.starts_with("nightlight."))
            })
            .filter_map(json_to_owned)
            .collect()
    }

    async fn update(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        id: &str,
        value: Value<'_>,
    ) -> fdo::Result<()> {
        let json = value_from_zvariant(&value)
            .ok_or_else(|| fdo::Error::InvalidArgs("unsupported value type".into()))?;
        if helix_config::ordered_lists::requires_update_ordered_list(id) {
            let message = helix_config::ordered_lists::update_ordered_list_message(id);
            return Err(fdo::Error::InvalidArgs(message.into()));
        }
        let mut state = self.state.lock().await;
        require_setting_access(&state, id)?;
        state.config.force_reload();
        state
            .config
            .set(id, json.clone())
            .map_err(fdo::Error::InvalidArgs)?;
        if id == "audio.sound_volume" {
            if let Some(volume) = json.as_f64() {
                if let Some(sink) = state.audio.sinks().iter().find(|node| node.is_default) {
                    let sink_id = sink.id;
                    let _ = state.audio.set_volume(sink_id, volume);
                }
            }
        }
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn update_ordered_list(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        id: &str,
        expected: Vec<OwnedValue>,
        value: Vec<OwnedValue>,
    ) -> fdo::Result<()> {
        let mut state = self.state.lock().await;
        if state.desktop.is_none() {
            return Err(fdo::Error::InvalidArgs(
                "Ordered desktop lists require Helix desktop".into(),
            ));
        }
        state.config.force_reload();
        apply_ordered_list_update(&mut state, id, &expected, &value)?;
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn update_application_list(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        id: &str,
        expected: Vec<OwnedValue>,
        value: Vec<OwnedValue>,
    ) -> fdo::Result<()> {
        let mut state = self.state.lock().await;
        state.config.force_reload();
        apply_ordered_list_update(&mut state, id, &expected, &value)?;
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn reset(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        id: &str,
    ) -> fdo::Result<()> {
        let mut state = self.state.lock().await;
        require_setting_access(&state, id)?;
        state.config.force_reload();
        state.config.reset(id).map_err(fdo::Error::InvalidArgs)?;
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn commit_overrides(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        baseline: &str,
        candidate: &str,
    ) -> fdo::Result<()> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let mut state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        state
            .config
            .commit_remote_overrides(baseline, candidate)
            .map_err(fdo::Error::Failed)?;
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn register_desktop(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] connection: &Connection,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
    ) -> fdo::Result<()> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let peer = resolve_peer(connection, &sender).await?;
        let mut state = self.state.lock().await;
        if let Some(owner) = &state.desktop {
            if owner != &peer.unique_name {
                return Err(fdo::Error::Failed(
                    "A Helix desktop is already registered".into(),
                ));
            }
        }
        state.desktop = Some(peer.unique_name);
        tracing::info!(client = %peer.client, "RegisterDesktop");
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn register_capabilities(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        capabilities: HashMap<String, OwnedValue>,
    ) -> fdo::Result<()> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let mut state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        let mut parsed = HashMap::new();
        for (key, value) in capabilities {
            if key != "gamma-control" {
                return Err(fdo::Error::InvalidArgs(format!(
                    "Unknown desktop capability {key}"
                )));
            }
            match &*value {
                Value::Bool(flag) => {
                    parsed.insert(key, *flag);
                }
                _ => {
                    return Err(fdo::Error::InvalidArgs(
                        "Capability values must be boolean".into(),
                    ));
                }
            }
        }
        let changed = state.desktop_capabilities != parsed;
        state.desktop_capabilities = parsed;
        drop(state);
        if changed {
            Self::changed(&emitter).await.ok();
        }
        Ok(())
    }

    async fn get_state(
        &self,
        #[zbus(header)] header: Header<'_>,
        owner: &str,
        key: &str,
        kind: &str,
    ) -> fdo::Result<HashMap<String, OwnedValue>> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        let json = state
            .runtime
            .get(owner, key, kind)
            .map_err(fdo::Error::InvalidArgs)?;
        let mut map = HashMap::new();
        if let Some(object) = json.as_object() {
            for (entry_key, value) in object {
                if let Some(owned) = json_to_owned(value) {
                    map.insert(entry_key.clone(), owned);
                }
            }
        }
        Ok(map)
    }

    async fn set_state(
        &self,
        #[zbus(header)] header: Header<'_>,
        owner: &str,
        key: &str,
        value: Value<'_>,
    ) -> fdo::Result<()> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let mut state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        match &value {
            Value::Bool(flag) => state
                .runtime
                .set_bool(owner, key, *flag)
                .map_err(fdo::Error::Failed)?,
            Value::Str(text) => state
                .runtime
                .set_string(owner, key, text)
                .map_err(fdo::Error::Failed)?,
            Value::Value(inner) => match &**inner {
                Value::Bool(flag) => state
                    .runtime
                    .set_bool(owner, key, *flag)
                    .map_err(fdo::Error::Failed)?,
                Value::Str(text) => state
                    .runtime
                    .set_string(owner, key, text)
                    .map_err(fdo::Error::Failed)?,
                _ => {
                    return Err(fdo::Error::InvalidArgs(
                        "Runtime state must be boolean or string".into(),
                    ));
                }
            },
            _ => {
                return Err(fdo::Error::InvalidArgs(
                    "Runtime state must be boolean or string".into(),
                ));
            }
        }
        Ok(())
    }

    async fn clear_state(
        &self,
        #[zbus(header)] header: Header<'_>,
        owner: &str,
    ) -> fdo::Result<()> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let mut state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        state
            .runtime
            .clear_owner(owner)
            .map_err(fdo::Error::Failed)?;
        Ok(())
    }

    #[zbus(signal)]
    async fn changed(emitter: &SignalEmitter<'_>) -> zbus::Result<()>;
}
