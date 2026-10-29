//! Desktop D-Bus client. Configuration, Audio, Network, Bluetooth snapshots and actions, and the
//! Agent Surface **confirmer** role only (`RegisterConfirmer`, `Confirm`, the two confirmation signals).
//! The shell must never call Hello, RequestAccess, Action on Agent Surface, or the manager methods.

mod activity;
mod agent;
mod contract;
mod json;
mod manager;

pub use activity::{ActivityClient, ACTIVITY_METHODS, ACTIVITY_SIGNALS};
pub use agent::{details_from_owned, ConfirmerClient, CONFIRMER_METHODS, CONFIRMER_SIGNALS};
pub use contract::{parse_interfaces, protocol_xml, Interface, BUS_NAME, OBJECT_PATH};
/// The wall clock the theme schedule is evaluated against.
pub use helix_theme::schedule::LocalTime;
pub use json::{args_from_json, owned_to_json, to_json};
pub use manager::{ManagerClient, MANAGER_METHODS};

use futures_util::StreamExt;
use zbus::proxy;
use zbus::zvariant::{Dict, OwnedValue, Value};

#[proxy(
    interface = "org.helix.Desktop.Configuration1",
    default_service = "org.helix.Desktop",
    default_path = "/org/helix/Desktop"
)]
trait Configuration {
    fn get_settings(&self) -> zbus::Result<Vec<OwnedValue>>;

    fn update(&self, id: &str, value: Value<'_>) -> zbus::Result<()>;

    fn reset(&self, id: &str) -> zbus::Result<()>;

    #[zbus(signal)]
    fn changed(&self) -> zbus::Result<()>;
}

/// Interface-sound volume from a GetSettings catalog, as a fraction in 0..=1.
pub fn sound_volume(entries: &[OwnedValue]) -> Option<f64> {
    for entry in entries {
        let Some(dict) = as_dict(entry) else { continue };
        let Some(id) = dict_string(&dict, "id") else {
            continue;
        };
        if id != "audio.sound_volume" {
            continue;
        }
        return dict_number(&dict, "value").map(|volume| volume.clamp(0.0, 1.0));
    }
    None
}

/// The smallest and largest interface scale an application applies. The catalog row has the same bounds.
pub const UI_SCALE_MIN: f64 = 0.8;
pub const UI_SCALE_MAX: f64 = 1.5;

/// `accessibility.ui_scale` from a GetSettings catalog, kept within bounds; 1.0 when the row is missing.
pub fn ui_scale(entries: &[OwnedValue]) -> f64 {
    for entry in entries {
        let Some(dict) = as_dict(entry) else { continue };
        if dict_string(&dict, "id").as_deref() != Some("accessibility.ui_scale") {
            continue;
        }
        if let Some(scale) = dict_number(&dict, "value").filter(|scale| scale.is_finite()) {
            return scale.clamp(UI_SCALE_MIN, UI_SCALE_MAX);
        }
    }
    1.0
}

fn dict_bool(dict: &Dict<'_, '_>, key: &str) -> Option<bool> {
    let key = Value::from(key);
    let mut value = dict.iter().find(|(candidate, _)| *candidate == &key)?.1;
    while let Value::Value(inner) = value {
        value = inner;
    }
    match value {
        Value::Bool(flag) => Some(*flag),
        _ => None,
    }
}

fn entry_of<'a>(entries: &'a [OwnedValue], id: &str) -> Option<Dict<'a, 'a>> {
    entries.iter().find_map(|entry| {
        let dict = as_dict(entry)?;
        (dict_string(&dict, "id").as_deref() == Some(id)).then_some(dict)
    })
}

/// Dark or light for a mode (`dark`, `light`, `auto`); `auto` follows the day and night schedule the way the desktop service
/// resolves it. Dark when the mode is missing or odd.
fn dark_for(
    mode: Option<&str>,
    location: impl FnOnce() -> helix_theme::schedule::LocationConfig,
    now: helix_theme::schedule::LocalTime,
) -> bool {
    match mode {
        Some("light") => false,
        Some("auto") => helix_theme::schedule::evaluate(&location(), now).night,
        _ => true,
    }
}

/// How Helix applications look: light or dark, and the accent colour (a name from `helix_config::settings::ACCENTS`; the stylesheet turns it into colours).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Look {
    pub dark: bool,
    pub accent: String,
}

/// The script that puts a look on a page: `data-theme` and `data-accent` on the root element. The accent is checked here, so nothing but a plain
/// name is ever put in a script.
pub fn theme_script(look: &Look) -> String {
    let accent: String = look
        .accent
        .chars()
        .filter(|c| c.is_ascii_lowercase())
        .take(16)
        .collect();
    format!(
        "document.documentElement.dataset.theme = '{}'; document.documentElement.dataset.accent = '{}'",
        if look.dark { "dark" } else { "light" },
        if accent.is_empty() { "blue" } else { &accent },
    )
}

fn accent_of(entries: &[OwnedValue]) -> String {
    entry_of(entries, "theme.accent")
        .and_then(|dict| dict_string(&dict, "value"))
        .filter(|name| name.chars().all(|c| c.is_ascii_lowercase()) && !name.is_empty())
        .unwrap_or_else(|| "blue".into())
}

/// Whether applications should draw dark now, from a GetSettings catalog (`theme.mode` and the `location.*` rows).
pub fn theme_is_dark(entries: &[OwnedValue], now: helix_theme::schedule::LocalTime) -> bool {
    let mode = entry_of(entries, "theme.mode").and_then(|dict| dict_string(&dict, "value"));
    dark_for(
        mode.as_deref(),
        || {
            let text = |id: &str| {
                entry_of(entries, id)
                    .and_then(|dict| dict_string(&dict, "value"))
                    .unwrap_or_default()
            };
            let number =
                |id: &str| entry_of(entries, id).and_then(|dict| dict_number(&dict, "value"));
            helix_theme::schedule::LocationConfig {
                custom_schedule: entry_of(entries, "location.custom_schedule")
                    .and_then(|dict| dict_bool(&dict, "value"))
                    .unwrap_or(false),
                sunset: text("location.sunset"),
                sunrise: text("location.sunrise"),
                latitude: number("location.latitude"),
                longitude: number("location.longitude"),
            }
        },
        now,
    )
}

/// Where the person is, from the settings list as JSON (what [`Desktop::settings`] returns): the `location.*` rows.
pub fn location_from_json(settings: &serde_json::Value) -> helix_theme::schedule::LocationConfig {
    let entry = |id: &str| {
        settings
            .as_array()
            .and_then(|entries| {
                entries
                    .iter()
                    .find(|e| e.get("id").and_then(|v| v.as_str()) == Some(id))
            })
            .and_then(|e| e.get("value"))
    };
    helix_theme::schedule::LocationConfig {
        custom_schedule: entry("location.custom_schedule")
            .and_then(|v| v.as_bool())
            .unwrap_or(false),
        sunset: entry("location.sunset")
            .and_then(|v| v.as_str())
            .unwrap_or_default()
            .to_string(),
        sunrise: entry("location.sunrise")
            .and_then(|v| v.as_str())
            .unwrap_or_default()
            .to_string(),
        latitude: entry("location.latitude").and_then(|v| v.as_f64()),
        longitude: entry("location.longitude").and_then(|v| v.as_f64()),
    }
}

/// The same answer from the settings list as JSON (what [`Desktop::settings`] returns).
pub fn theme_is_dark_json(
    settings: &serde_json::Value,
    now: helix_theme::schedule::LocalTime,
) -> bool {
    let mode = settings
        .as_array()
        .and_then(|entries| {
            entries
                .iter()
                .find(|e| e.get("id").and_then(|v| v.as_str()) == Some("theme.mode"))
        })
        .and_then(|e| e.get("value"))
        .and_then(|v| v.as_str());
    dark_for(mode, || location_from_json(settings), now)
}

pub fn sound_volume_label(volume: Option<f64>) -> String {
    match volume {
        Some(volume) => {
            let percent = (volume.clamp(0.0, 1.0) * 100.0).round() as u8;
            format!("Sound {percent}%")
        }
        None => "Sound unavailable".to_string(),
    }
}

fn as_dict(entry: &OwnedValue) -> Option<Dict<'_, '_>> {
    let value = match &**entry {
        Value::Value(inner) => inner.as_ref(),
        other => other,
    };
    match value {
        Value::Dict(dict) => dict.try_clone().ok(),
        _ => None,
    }
}

fn dict_string(dict: &Dict<'_, '_>, key: &str) -> Option<String> {
    let key = Value::from(key);
    let value = dict.iter().find(|(candidate, _)| *candidate == &key)?.1;
    value_string(value)
}

fn dict_number(dict: &Dict<'_, '_>, key: &str) -> Option<f64> {
    let key = Value::from(key);
    let value = dict.iter().find(|(candidate, _)| *candidate == &key)?.1;
    value_number(value)
}

fn value_string(value: &Value<'_>) -> Option<String> {
    match value {
        Value::Str(text) => Some(text.to_string()),
        Value::Value(inner) => value_string(inner),
        _ => None,
    }
}

/// Lookup a settings catalog row by id (from [`Desktop::settings`]).
pub fn setting_by_id(settings: &serde_json::Value, id: &str) -> zbus::Result<serde_json::Value> {
    settings
        .as_array()
        .and_then(|entries| {
            entries
                .iter()
                .find(|entry| entry.get("id").and_then(|value| value.as_str()) == Some(id))
        })
        .cloned()
        .ok_or_else(|| zbus::Error::Failure(format!("setting not found: {id}")))
}

fn value_number(value: &Value<'_>) -> Option<f64> {
    match value {
        Value::F64(number) => Some(*number),
        Value::I64(number) => Some(*number as f64),
        Value::U64(number) => Some(*number as f64),
        Value::I32(number) => Some(f64::from(*number)),
        Value::U32(number) => Some(f64::from(*number)),
        Value::Value(inner) => value_number(inner),
        _ => None,
    }
}

/// The service interfaces that follow the snapshot / action / changed pattern.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Domain {
    Audio,
    Network,
    Bluetooth,
    Displays,
    Appearance,
    /// Settings. Has no `Action`; use [`Desktop::settings`] and [`Desktop::update_setting`], and `watch` for changes.
    Configuration,
}

impl Domain {
    pub fn interface(self) -> &'static str {
        match self {
            Self::Audio => "org.helix.Desktop.Audio1",
            Self::Network => "org.helix.Desktop.Network1",
            Self::Bluetooth => "org.helix.Desktop.Bluetooth1",
            Self::Displays => "org.helix.Desktop.Displays1",
            Self::Appearance => "org.helix.Desktop.Appearance1",
            Self::Configuration => "org.helix.Desktop.Configuration1",
        }
    }
}

pub struct ServiceStarts {
    changes: zbus::fdo::NameOwnerChangedStream,
}

impl ServiceStarts {
    pub async fn next(&mut self) -> Option<()> {
        while let Some(change) = self.changes.next().await {
            let Ok(args) = change.args() else { continue };
            if args.name().as_str() == BUS_NAME && args.new_owner().is_some() {
                return Some(());
            }
        }
        None
    }
}

pub struct Desktop {
    connection: zbus::Connection,
}

impl Desktop {
    pub async fn session() -> zbus::Result<Self> {
        Ok(Self {
            connection: zbus::Connection::session().await?,
        })
    }

    pub fn connection(&self) -> &zbus::Connection {
        &self.connection
    }

    /// Second handle on the same session bus (for concurrent signal watches).
    pub fn clone_session(&self) -> Self {
        Self {
            connection: self.connection.clone(),
        }
    }

    /// Claim the desktop role so the shell may register as Agent Surface confirmer.
    pub async fn register_desktop(&self) -> zbus::Result<()> {
        let proxy = self.domain_proxy(Domain::Configuration).await?;
        proxy.call("RegisterDesktop", &()).await
    }

    /// Compositor capabilities from the registered desktop (`gamma-control`, …).
    pub async fn register_capabilities(
        &self,
        capabilities: &serde_json::Value,
    ) -> zbus::Result<()> {
        let proxy = self.domain_proxy(Domain::Configuration).await?;
        let map = args_from_json(capabilities).map_err(zbus::Error::Failure)?;
        proxy.call("RegisterCapabilities", &(map,)).await
    }

    /// A watch on who owns the service's name: `next()` completes each time a (new) process takes it. A signal, not a poll.
    pub async fn watch_service_starts(&self) -> zbus::Result<ServiceStarts> {
        let bus = zbus::fdo::DBusProxy::new(&self.connection).await?;
        Ok(ServiceStarts {
            changes: bus.receive_name_owner_changed().await?,
        })
    }

    pub fn manager(&self) -> ManagerClient {
        ManagerClient::new(self.connection.clone())
    }

    pub fn activity(&self) -> ActivityClient {
        ActivityClient::new(self.connection.clone())
    }

    pub fn confirmer(&self) -> ConfirmerClient {
        ConfirmerClient::new(self.connection.clone())
    }

    async fn domain_proxy(&self, domain: Domain) -> zbus::Result<zbus::Proxy<'static>> {
        zbus::Proxy::new(&self.connection, BUS_NAME, OBJECT_PATH, domain.interface()).await
    }

    /// The domain's `GetSnapshot` as JSON (for audio: `{available, sinks, sources, programs}`).
    pub async fn snapshot(&self, domain: Domain) -> zbus::Result<serde_json::Value> {
        let proxy = self.domain_proxy(domain).await?;
        let snapshot: std::collections::HashMap<String, OwnedValue> =
            proxy.call("GetSnapshot", &()).await?;
        let mut object = serde_json::Map::new();
        for (key, value) in &snapshot {
            object.insert(key.clone(), owned_to_json(value));
        }
        Ok(serde_json::Value::Object(object))
    }

    /// Starts one of the domain's actions and returns the operation id. The service checks everything again.
    pub async fn action(
        &self,
        domain: Domain,
        action: &str,
        args: &serde_json::Value,
    ) -> zbus::Result<u64> {
        let proxy = self.domain_proxy(domain).await?;
        let args = args_from_json(args).map_err(zbus::Error::Failure)?;
        proxy.call("Action", &(action, args)).await
    }

    /// Calls `on_change` after every `Changed` signal of the domain. Does not poll. Returns when the stream ends.
    pub async fn watch<F>(&self, domain: Domain, mut on_change: F) -> zbus::Result<()>
    where
        F: FnMut() + Send,
    {
        let proxy = self.domain_proxy(domain).await?;
        let mut changes = proxy.receive_signal("Changed").await?;
        while changes.next().await.is_some() {
            on_change();
        }
        Ok(())
    }

    /// Answers the question the service asked the pairing client (`Bluetooth1.Reply`). `cancel` refuses it.
    pub async fn bluetooth_reply(&self, response: &str, cancel: bool) -> zbus::Result<()> {
        let proxy = self.domain_proxy(Domain::Bluetooth).await?;
        proxy.call("Reply", &(response, cancel)).await
    }

    /// `Bluetooth1.Cancel(operation)` from the desktop registrant.
    pub async fn bluetooth_cancel(&self, operation: u64) -> zbus::Result<()> {
        let proxy = self.domain_proxy(Domain::Bluetooth).await?;
        proxy.call("Cancel", &(operation,)).await
    }

    /// `Audio1.StopInputMeter` (idempotent).
    pub async fn audio_stop_input_meter(&self) -> zbus::Result<()> {
        let proxy = self.domain_proxy(Domain::Audio).await?;
        proxy.call("StopInputMeter", &()).await
    }

    /// `Audio1.StartInputMeter` with the microphone's id (returns an error for an unknown one).
    /// Renews that caller's eight-second lease. Level samples are not part of the audio snapshot.
    pub async fn audio_start_input_meter(&self, source: u32) -> zbus::Result<()> {
        let proxy = self.domain_proxy(Domain::Audio).await?;
        proxy.call("StartInputMeter", &(source,)).await
    }

    /// Calls `on_level` for every `InputLevel` sent to this connection: source id, peak (0..=1 as the
    /// service sent it), and whether a sample was available.
    pub async fn watch_input_level<F>(&self, mut on_level: F) -> zbus::Result<()>
    where
        F: FnMut(u32, f64, bool) + Send,
    {
        let proxy = self.domain_proxy(Domain::Audio).await?;
        let mut levels = proxy.receive_signal("InputLevel").await?;
        while let Some(message) = levels.next().await {
            if let Ok((source, peak, available)) = message.body().deserialize::<(u32, f64, bool)>()
            {
                on_level(source, peak, available);
            }
        }
        Ok(())
    }

    /// Calls `on_prompt` with every `Prompt` signal the service sends this client for a pairing (kind, device, passkey, pin, entered).
    /// Returns when the stream ends.
    pub async fn watch_bluetooth_prompt<F>(&self, mut on_prompt: F) -> zbus::Result<()>
    where
        F: FnMut(serde_json::Value) + Send,
    {
        let proxy = self.domain_proxy(Domain::Bluetooth).await?;
        let mut prompts = proxy.receive_signal("Prompt").await?;
        while let Some(message) = prompts.next().await {
            if let Ok((body,)) = message
                .body()
                .deserialize::<(std::collections::HashMap<String, OwnedValue>,)>()
            {
                let mut object = serde_json::Map::new();
                for (key, value) in &body {
                    object.insert(key.clone(), owned_to_json(value));
                }
                on_prompt(serde_json::Value::Object(object));
            }
        }
        Ok(())
    }

    /// Calls `on_done` with `(operation, succeeded, message)` for every `Completed` signal of the domain.
    pub async fn watch_completed<F>(&self, domain: Domain, mut on_done: F) -> zbus::Result<()>
    where
        F: FnMut(u64, bool, String) + Send,
    {
        let proxy = self.domain_proxy(domain).await?;
        let mut done = proxy.receive_signal("Completed").await?;
        while let Some(message) = done.next().await {
            if let Ok((operation, ok, text)) = message.body().deserialize::<(u64, bool, String)>() {
                on_done(operation, ok, text);
            }
        }
        Ok(())
    }

    /// The service's settings list (`GetSettings`) as a JSON array of entries.
    pub async fn settings(&self) -> zbus::Result<serde_json::Value> {
        let proxy = self.domain_proxy(Domain::Configuration).await?;
        let entries: Vec<OwnedValue> = proxy.call("GetSettings", &()).await?;
        Ok(serde_json::Value::Array(
            entries.iter().map(owned_to_json).collect(),
        ))
    }

    /// `Configuration1.ActionHistory(limit)`: the one history of actions, newest first (`docs/design/actions.md`).
    pub async fn action_history(&self, limit: u32) -> zbus::Result<serde_json::Value> {
        let proxy = self.domain_proxy(Domain::Configuration).await?;
        let text: String = proxy.call("ActionHistory", &(limit,)).await?;
        Ok(serde_json::from_str(&text).unwrap_or(serde_json::Value::Array(Vec::new())))
    }

    /// `Configuration1.UndoAction(entry)`: puts back what history entry `entry` changed; returns the sentence of what was undone.
    pub async fn undo_action(&self, entry: u64) -> zbus::Result<String> {
        let proxy = self.domain_proxy(Domain::Configuration).await?;
        proxy.call("UndoAction", &(entry,)).await
    }

    /// `Configuration1.RunAppAction(action, params)`: runs an app's action for the person; the app's reply, with `history` and `sentence`.
    pub async fn run_app_action(
        &self,
        action: &str,
        params: &serde_json::Value,
    ) -> zbus::Result<serde_json::Value> {
        let proxy = self.domain_proxy(Domain::Configuration).await?;
        let text: String = proxy
            .call("RunAppAction", &(action, params.to_string()))
            .await?;
        Ok(serde_json::from_str(&text).unwrap_or(serde_json::Value::Null))
    }

    /// `Configuration1.Update(id, value)` for a boolean, a string or a number (whole numbers as 64-bit integers). The service validates the id and the value again.
    pub async fn update_setting(&self, id: &str, value: &serde_json::Value) -> zbus::Result<()> {
        let proxy = self.domain_proxy(Domain::Configuration).await?;
        let value: Value<'static> = match value {
            serde_json::Value::Bool(flag) => Value::Bool(*flag),
            serde_json::Value::String(text) => Value::from(text.clone()),
            serde_json::Value::Number(number) => match number.as_i64() {
                Some(whole) => Value::I64(whole),
                None => Value::F64(number.as_f64().unwrap_or(0.0)),
            },
            _ => {
                return Err(zbus::Error::Failure(
                    "only a boolean, a string or a number can be set".into(),
                ))
            }
        };
        proxy.call("Update", &(id, value)).await
    }

    /// `Configuration1.UpdateOrderedList(id, baseline, candidate)` for key-chords and other ordered lists.
    /// Each entry is a string wrapped as a variant (`av`). The service compares `baseline` to what it holds and refuses a stale write.
    pub async fn update_ordered_list(
        &self,
        id: &str,
        baseline: &[String],
        candidate: &[String],
    ) -> zbus::Result<()> {
        let baseline = serde_json::Value::Array(
            baseline
                .iter()
                .map(|s| serde_json::Value::String(s.clone()))
                .collect(),
        );
        let candidate = serde_json::Value::Array(
            candidate
                .iter()
                .map(|s| serde_json::Value::String(s.clone()))
                .collect(),
        );
        self.update_ordered_list_json(id, &baseline, &candidate)
            .await
    }

    /// `UpdateOrderedList` with JSON array baseline/candidate (strings, rule objects, …).
    pub async fn update_ordered_list_json(
        &self,
        id: &str,
        baseline: &serde_json::Value,
        candidate: &serde_json::Value,
    ) -> zbus::Result<()> {
        let proxy = self.domain_proxy(Domain::Configuration).await?;
        let baseline = json::json_to_ordered_list_av(baseline).map_err(zbus::Error::Failure)?;
        let candidate = json::json_to_ordered_list_av(candidate).map_err(zbus::Error::Failure)?;
        proxy
            .call("UpdateOrderedList", &(id, baseline, candidate))
            .await
    }

    /// `Configuration1.UpdateApplicationList` (dock pins and other `application-list` rows).
    pub async fn update_application_list(
        &self,
        id: &str,
        baseline: &[String],
        candidate: &[String],
    ) -> zbus::Result<()> {
        let baseline = serde_json::Value::Array(
            baseline
                .iter()
                .map(|s| serde_json::Value::String(s.clone()))
                .collect(),
        );
        let candidate = serde_json::Value::Array(
            candidate
                .iter()
                .map(|s| serde_json::Value::String(s.clone()))
                .collect(),
        );
        let proxy = self.domain_proxy(Domain::Configuration).await?;
        let baseline = json::json_to_ordered_list_av(&baseline).map_err(zbus::Error::Failure)?;
        let candidate = json::json_to_ordered_list_av(&candidate).map_err(zbus::Error::Failure)?;
        proxy
            .call("UpdateApplicationList", &(id, baseline, candidate))
            .await
    }

    /// `CommitOverrides(baseline, candidate)` for settings.toml sidecar writes (desktop registrant only).
    pub async fn commit_overrides(&self, baseline: &str, candidate: &str) -> zbus::Result<()> {
        let proxy = self.domain_proxy(Domain::Configuration).await?;
        proxy.call("CommitOverrides", &(baseline, candidate)).await
    }

    /// `Configuration1.Reset(id)` clears a user override for one catalog row.
    pub async fn reset_setting(&self, id: &str) -> zbus::Result<()> {
        let proxy = self.domain_proxy(Domain::Configuration).await?;
        proxy.call("Reset", &(id,)).await
    }

    /// One catalog row from [`Desktop::settings`], matched by `id`.
    pub async fn setting(&self, id: &str) -> zbus::Result<serde_json::Value> {
        let settings = self.settings().await?;
        setting_by_id(&settings, id)
    }

    pub async fn audio_snapshot(&self) -> zbus::Result<serde_json::Value> {
        self.snapshot(Domain::Audio).await
    }

    pub async fn audio_action(&self, action: &str, args: &serde_json::Value) -> zbus::Result<u64> {
        self.action(Domain::Audio, action, args).await
    }

    pub async fn watch_audio<F>(&self, on_change: F) -> zbus::Result<()>
    where
        F: FnMut() + Send,
    {
        self.watch(Domain::Audio, on_change).await
    }

    /// Calls `on_scale` with the interface scale now, then again after every Configuration1.Changed where it differs.
    /// An application applies it to its own windows; the shell's own chrome does not follow it.
    pub async fn watch_ui_scale<F>(&self, mut on_scale: F) -> zbus::Result<()>
    where
        F: FnMut(f64) + Send,
    {
        let proxy = ConfigurationProxy::new(&self.connection).await?;
        let mut last = ui_scale(&proxy.get_settings().await?);
        on_scale(last);
        let mut changes = proxy.receive_changed().await?;
        while changes.next().await.is_some() {
            let scale = ui_scale(&proxy.get_settings().await?);
            if (scale - last).abs() > f64::EPSILON {
                last = scale;
                on_scale(scale);
            }
        }
        Ok(())
    }

    /// Calls `on_look` with how applications should look now (dark or light, and the accent colour), then again whenever either changes: after a
    /// settings change, and once a minute so an automatic theme turns at sunset and sunrise.
    pub async fn watch_theme<F>(&self, mut on_look: F) -> zbus::Result<()>
    where
        F: FnMut(Look) + Send,
    {
        let proxy = ConfigurationProxy::new(&self.connection).await?;
        let mut entries = proxy.get_settings().await?;
        let now = || helix_theme::schedule::LocalTime::now();
        let mut last = Look {
            dark: theme_is_dark(&entries, now()),
            accent: accent_of(&entries),
        };
        on_look(last.clone());
        let mut changes = proxy.receive_changed().await?;
        let mut tick = tokio::time::interval(std::time::Duration::from_secs(60));
        tick.tick().await;
        loop {
            tokio::select! {
                changed = changes.next() => {
                    if changed.is_none() {
                        return Ok(());
                    }
                    entries = proxy.get_settings().await?;
                }
                _ = tick.tick() => {}
            }
            let look = Look {
                dark: theme_is_dark(&entries, now()),
                accent: accent_of(&entries),
            };
            if look != last {
                last = look.clone();
                on_look(look);
            }
        }
    }

    /// Calls `on_dark` with whether applications should draw dark now, then again whenever that changes: after a settings change, and
    /// once a minute so an automatic theme turns at sunset and sunrise.
    pub async fn watch_theme_dark<F>(&self, mut on_dark: F) -> zbus::Result<()>
    where
        F: FnMut(bool) + Send,
    {
        let proxy = ConfigurationProxy::new(&self.connection).await?;
        let mut entries = proxy.get_settings().await?;
        let mut last = theme_is_dark(&entries, helix_theme::schedule::LocalTime::now());
        on_dark(last);
        let mut changes = proxy.receive_changed().await?;
        let mut tick = tokio::time::interval(std::time::Duration::from_secs(60));
        tick.tick().await;
        loop {
            tokio::select! {
                changed = changes.next() => {
                    if changed.is_none() {
                        return Ok(());
                    }
                    entries = proxy.get_settings().await?;
                }
                _ = tick.tick() => {}
            }
            let dark = theme_is_dark(&entries, helix_theme::schedule::LocalTime::now());
            if dark != last {
                last = dark;
                on_dark(dark);
            }
        }
    }

    pub async fn sound_volume(&self) -> zbus::Result<Option<f64>> {
        let proxy = ConfigurationProxy::new(&self.connection).await?;
        Ok(sound_volume(&proxy.get_settings().await?))
    }

    /// Calls `on_volume` with the current value, then again after every Configuration1.Changed.
    pub async fn watch_sound_volume<F>(&self, mut on_volume: F) -> zbus::Result<()>
    where
        F: FnMut(Option<f64>) + Send,
    {
        let proxy = ConfigurationProxy::new(&self.connection).await?;
        on_volume(sound_volume(&proxy.get_settings().await?));
        let mut changes = proxy.receive_changed().await?;
        while changes.next().await.is_some() {
            on_volume(sound_volume(&proxy.get_settings().await?));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use zbus::zvariant::{Dict, OwnedValue, Signature, Value};

    use super::{
        sound_volume, sound_volume_label, theme_is_dark, theme_is_dark_json, ui_scale, BUS_NAME,
        OBJECT_PATH,
    };

    fn setting(id: &str, value: impl Into<Value<'static>>) -> OwnedValue {
        let mut dict = Dict::new(
            &Signature::try_from("s").unwrap(),
            &Signature::try_from("v").unwrap(),
        );
        dict.append(Value::from("id"), Value::Value(Box::new(Value::from(id))))
            .unwrap();
        dict.append(Value::from("value"), Value::Value(Box::new(value.into())))
            .unwrap();
        OwnedValue::try_from(Value::Dict(dict)).unwrap()
    }

    #[test]
    fn reads_the_sound_volume_setting() {
        let entries = vec![
            setting("audio.enable_sounds", Value::from(true)),
            setting("audio.sound_volume", Value::from(0.25f64)),
        ];
        assert_eq!(sound_volume(&entries), Some(0.25));
        assert_eq!(sound_volume_label(Some(0.5)), "Sound 50%");
        assert_eq!(sound_volume_label(Some(1.4)), "Sound 100%");
        assert_eq!(sound_volume_label(None), "Sound unavailable");
        assert_eq!(BUS_NAME, "org.helix.Desktop");
        assert_eq!(OBJECT_PATH, "/org/helix/Desktop");
    }

    #[test]
    fn the_interface_scale_is_read_and_kept_in_bounds() {
        let with = |value: f64| vec![setting("accessibility.ui_scale", Value::from(value))];
        assert_eq!(ui_scale(&with(1.25)), 1.25);
        assert_eq!(ui_scale(&with(3.0)), 1.5);
        assert_eq!(ui_scale(&with(0.1)), 0.8);
        assert_eq!(ui_scale(&with(f64::NAN)), 1.0);
        assert_eq!(
            ui_scale(&[setting("audio.sound_volume", Value::from(0.5f64))]),
            1.0,
            "a missing row means no scaling"
        );
    }

    #[test]
    fn applications_follow_the_theme_mode_and_the_schedule() {
        use helix_theme::schedule::LocalTime;
        let at = |minute_of_day| LocalTime {
            minute_of_day,
            second: 0,
            day_of_year: 100,
            gmt_offset_secs: 0,
        };
        let mode = |mode: &str| vec![setting("theme.mode", Value::from(mode.to_string()))];
        assert!(theme_is_dark(&mode("dark"), at(12 * 60)));
        assert!(!theme_is_dark(&mode("light"), at(23 * 60)));
        assert!(theme_is_dark(&[], at(12 * 60)), "no setting means dark");
        let mut auto = mode("auto");
        auto.push(setting("location.custom_schedule", Value::from(true)));
        auto.push(setting("location.sunset", Value::from("19:00".to_string())));
        auto.push(setting(
            "location.sunrise",
            Value::from("07:00".to_string()),
        ));
        assert!(!theme_is_dark(&auto, at(12 * 60)), "light by day");
        assert!(theme_is_dark(&auto, at(21 * 60)), "dark by night");
        assert!(theme_is_dark(&auto, at(3 * 60)), "dark before sunrise");
        let json = serde_json::json!([
            {"id": "theme.mode", "value": "auto"},
            {"id": "location.custom_schedule", "value": true},
            {"id": "location.sunset", "value": "19:00"},
            {"id": "location.sunrise", "value": "07:00"},
        ]);
        assert!(
            !theme_is_dark_json(&json, at(12 * 60)),
            "the JSON form agrees: light by day"
        );
        assert!(
            theme_is_dark_json(&json, at(21 * 60)),
            "the JSON form agrees: dark by night"
        );
        assert!(!theme_is_dark_json(
            &serde_json::json!([{"id": "theme.mode", "value": "light"}]),
            at(23 * 60)
        ));
        assert!(
            theme_is_dark_json(&serde_json::Value::Null, at(12 * 60)),
            "no settings means dark"
        );
    }

    #[test]
    fn wrapped_variants_still_parse() {
        let mut dict = Dict::new(
            &Signature::try_from("s").unwrap(),
            &Signature::try_from("v").unwrap(),
        );
        dict.append(
            Value::from("id"),
            Value::Value(Box::new(Value::from("audio.sound_volume"))),
        )
        .unwrap();
        dict.append(
            Value::from("value"),
            Value::Value(Box::new(Value::from(0.5f64))),
        )
        .unwrap();
        let wrapped = OwnedValue::try_from(Value::Value(Box::new(Value::Dict(dict)))).unwrap();
        assert_eq!(sound_volume(&[wrapped]), Some(0.5));
    }

    #[test]
    fn client_calls_only_methods_the_contract_lists() {
        let interfaces = super::parse_interfaces(super::protocol_xml());
        let configuration = &interfaces["org.helix.Desktop.Configuration1"];
        for method in ["GetSettings", "Update"] {
            assert!(
                configuration.methods.iter().any(|name| name == method),
                "{method}"
            );
        }
        assert!(configuration.signals.iter().any(|name| name == "Changed"));
        for domain in [
            super::Domain::Audio,
            super::Domain::Network,
            super::Domain::Bluetooth,
        ] {
            let interface = &interfaces[domain.interface()];
            for method in ["GetSnapshot", "Action"] {
                assert!(
                    interface.methods.iter().any(|name| name == method),
                    "{} {method}",
                    domain.interface()
                );
            }
            assert!(
                interface.signals.iter().any(|name| name == "Changed"),
                "{}",
                domain.interface()
            );
        }
    }
}
