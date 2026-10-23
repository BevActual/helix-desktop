//! `org.freedesktop.Notifications` for the shell: the bus face of `helix_shell_model::notifications`. Everything the model needs to
//! decide (what to keep, how long, what to show) lives there; this file only moves values between the bus, the model and the page.
//! The shell never starts anything because of a notification: a button press sends the key the application itself offered back
//! to that application as `ActionInvoked`, nothing more.

use std::collections::HashMap;
use std::sync::{Mutex, OnceLock};
use std::time::Duration;

use helix_shell_model::notifications::{Event, Incoming, NotificationsView, RequestError, Urgency};
use tauri::{AppHandle, Emitter};
use zbus::zvariant::OwnedValue;

use crate::{now_ms, shell_state};

const NAME: &str = "org.freedesktop.Notifications";
const PATH: &str = "/org/freedesktop/Notifications";

static CONNECTION: OnceLock<zbus::Connection> = OnceLock::new();
static TIMER: Mutex<Option<tauri::async_runtime::JoinHandle<()>>> = Mutex::new(None);

struct Server {
    app: AppHandle,
}

#[zbus::interface(name = "org.freedesktop.Notifications")]
impl Server {
    /// Only what the shell really does: plain text bodies (markup is reduced to text) and buttons.
    async fn get_capabilities(&self) -> Vec<String> {
        vec!["body".into(), "actions".into()]
    }

    #[allow(clippy::too_many_arguments)]
    async fn notify(
        &self,
        app_name: String,
        replaces_id: u32,
        _app_icon: String,
        summary: String,
        body: String,
        actions: Vec<String>,
        hints: HashMap<String, OwnedValue>,
        expire_timeout: i32,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> u32 {
        // The sender is the bus connection that made the call: it cannot be chosen by the caller, unlike the name it gives itself.
        let owner = header.sender().map(ToString::to_string).unwrap_or_default();
        let urgency = hints
            .get("urgency")
            .and_then(|value| u8::try_from(value.clone()).ok())
            .map_or(Urgency::Normal, Urgency::from_hint);
        let app_label: String = app_name.chars().take(20).collect();
        let incoming = Incoming {
            owner,
            app: app_name,
            replaces: replaces_id,
            summary,
            body,
            actions,
            urgency,
            timeout_ms: expire_timeout,
        };
        let state = shell_state(&self.app);
        let mut new_apps: Option<Vec<String>> = None;
        let (id, events) = {
            let mut shell = state.lock().expect("shell state");
            let now = now_ms(shell.started);
            let before = shell.notifications.seen_apps().len();
            let result = shell.notifications.notify(incoming, now);
            if shell.notifications.seen_apps().len() != before {
                new_apps = Some(shell.notifications.seen_apps().to_vec());
            }
            result
        };
        if let Some(apps) = new_apps {
            write_seen_apps(&apps);
        }
        eprintln!(
            "NOTIFY stored id={id} replaces={replaces_id} app={:?}",
            app_label
        );
        changed(&self.app, events).await;
        id
    }

    async fn close_notification(&self, id: u32) {
        let events = shell_state(&self.app)
            .lock()
            .expect("shell state")
            .notifications
            .close_by_app(id);
        changed(&self.app, events).await;
    }

    async fn get_server_information(&self) -> (String, String, String, String) {
        (
            "Helix Shell".into(),
            "Helix".into(),
            env!("CARGO_PKG_VERSION").into(),
            "1.2".into(),
        )
    }
}

async fn signal(name: &str, body: &(impl serde::Serialize + zbus::zvariant::DynamicType)) {
    if let Some(connection) = CONNECTION.get() {
        let _ = connection
            .emit_signal(None::<&str>, PATH, NAME, name, body)
            .await;
    }
}

/// Tells the applications what happened, the page what to draw, and the timer when to wake next.
pub async fn changed(app: &AppHandle, events: Vec<Event>) {
    for event in events {
        match event {
            Event::Closed { id, reason } => signal("NotificationClosed", &(id, reason)).await,
            Event::ActionInvoked { id, key } => signal("ActionInvoked", &(id, key)).await,
        }
    }
    publish(app);
    rearm(app);
}

fn view(app: &AppHandle) -> NotificationsView {
    let state = shell_state(app);
    let shell = state.lock().expect("shell state");
    shell.notifications.view(now_ms(shell.started))
}

fn publish(app: &AppHandle) {
    let view = view(app);
    eprintln!(
        "NOTIFICATIONS view toasts={} items={} unread={} dnd={}",
        view.toasts.len(),
        view.items.len(),
        view.unread,
        view.dnd
    );
    let _ = app.emit("notifications-view", view);
}

/// Sleeps until the earliest moment something in the store changes by itself (a toast ends, a notification stops being live).
/// One timer exists at a time; no polling.
fn rearm(app: &AppHandle) {
    let deadline = {
        let state = shell_state(app);
        let shell = state.lock().expect("shell state");
        shell
            .notifications
            .next_deadline()
            .map(|at| (at, now_ms(shell.started)))
    };
    let mut timer = TIMER.lock().expect("timer");
    if let Some(old) = timer.take() {
        old.abort();
    }
    let Some((at, now)) = deadline else { return };
    let app = app.clone();
    *timer = Some(tauri::async_runtime::spawn(async move {
        tokio::time::sleep(Duration::from_millis(at.saturating_sub(now))).await;
        let events = {
            let state = shell_state(&app);
            let mut shell = state.lock().expect("shell state");
            let now = now_ms(shell.started);
            shell.notifications.expire(now)
        };
        // changed() re-arms, so the next deadline is picked up (and this task's own handle is replaced).
        for event in &events {
            if let Event::Closed { id, reason } = event {
                signal("NotificationClosed", &(*id, *reason)).await;
            }
        }
        publish(&app);
        rearm(&app);
    }));
}

/// Claims the well-known name if nobody has it; never takes it from another notification daemon.
pub fn serve(app: AppHandle) {
    watch_prefs(app.clone());
    tauri::async_runtime::spawn(async move {
        let built = zbus::connection::Builder::session()
            .map(|builder| {
                builder
                    .allow_name_replacements(false)
                    .replace_existing_names(false)
            })
            .and_then(|builder| builder.name(NAME))
            .and_then(|builder| builder.serve_at(PATH, Server { app: app.clone() }));
        let connection = match built {
            Ok(builder) => builder.build().await,
            Err(error) => Err(error),
        };
        match connection {
            Ok(connection) => {
                eprintln!("NOTIFICATIONS serving {NAME}");
                let _ = CONNECTION.set(connection);
                publish(&app);
            }
            Err(error) => eprintln!("NOTIFICATIONS could not serve {NAME}: {error}"),
        }
    });
}

/// `~/.config/helix/notification-settings.json`, written by Settings.
fn prefs_path() -> Option<std::path::PathBuf> {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(std::path::PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME").map(|h| std::path::PathBuf::from(h).join(".config"))
        })?;
    Some(base.join("helix/notification-settings.json"))
}

/// The applications that have sent a notification, for Settings to offer a rule for each (`notification-apps.json` beside the settings).
fn seen_apps_path() -> Option<std::path::PathBuf> {
    prefs_path().map(|p| p.with_file_name("notification-apps.json"))
}

fn read_seen_apps() -> Vec<String> {
    seen_apps_path()
        .and_then(|path| std::fs::read_to_string(path).ok())
        .and_then(|text| serde_json::from_str::<Vec<String>>(&text).ok())
        .unwrap_or_default()
}

fn write_seen_apps(apps: &[String]) {
    let Some(path) = seen_apps_path() else {
        return;
    };
    if let Some(dir) = path.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let temporary = path.with_extension("tmp");
    if std::fs::write(&temporary, serde_json::to_vec(apps).unwrap_or_default()).is_ok() {
        let _ = std::fs::rename(&temporary, &path);
    }
}

fn read_prefs() -> helix_config::notification_prefs::NotificationPrefs {
    prefs_path()
        .and_then(|path| std::fs::read_to_string(path).ok())
        .map(|text| helix_config::notification_prefs::NotificationPrefs::parse(&text))
        .unwrap_or_default()
}

/// Writes Do not disturb into the settings file, keeping the banner time and count already there. A failure is logged, not hidden.
fn persist_dnd(on: bool) {
    if let Err(error) = write_dnd(on) {
        eprintln!("NOTIFICATIONS could not save do-not-disturb: {error}");
    }
}

fn write_dnd(on: bool) -> Result<(), String> {
    use std::io::Write;
    use std::os::unix::fs::OpenOptionsExt;
    let current = read_prefs();
    if current.dnd == on {
        return Ok(());
    }
    let next = current
        .set("dnd", &serde_json::json!(on))
        .map_err(|error| format!("{error:?}"))?;
    let path = prefs_path().ok_or("no settings folder")?;
    let parent = path.parent().ok_or("no settings folder")?;
    std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    // Settings writes this file too: a name of our own keeps the two from sharing one half-written temporary file.
    let temporary = path.with_extension(format!("{}.tmp", std::process::id()));
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(0o600)
        .open(&temporary)
        .map_err(|e| e.to_string())?;
    file.write_all(next.to_json().as_bytes())
        .map_err(|e| e.to_string())?;
    drop(file);
    std::fs::rename(&temporary, &path).map_err(|e| e.to_string())
}

/// The current local hour, 0–23. Quiet hours are checked against this, not against the file's clock.
fn local_hour() -> u32 {
    // SAFETY: `localtime_r` is called with a valid time and a zeroed `tm`.
    unsafe {
        let now = libc::time(std::ptr::null_mut());
        let mut tm: libc::tm = std::mem::zeroed();
        if libc::localtime_r(&now, &mut tm).is_null() {
            return 0;
        }
        tm.tm_hour.clamp(0, 23) as u32
    }
}

/// Applies banner time, banner count, Do not disturb and quiet hours from Settings. Returns whether quiet hours are in effect.
fn apply_prefs(app: &AppHandle) -> bool {
    let prefs = read_prefs();
    let quiet = prefs.quiet_at(local_hour());
    {
        let state = shell_state(app);
        let mut shell = state.lock().expect("shell state");
        shell.notifications.set_toast_prefs(
            u64::from(prefs.toast_seconds) * 1000,
            prefs.max_toasts as usize,
        );
        shell.notifications.set_dnd(prefs.dnd);
        shell.notifications.set_quiet(quiet);
        shell.notifications.remember_apps(read_seen_apps());
        shell
            .notifications
            .set_app_rules(prefs.app_rules.iter().map(|(name, rule)| {
                (
                    name.clone(),
                    *rule == helix_config::notification_prefs::AppRule::Off,
                )
            }));
    }
    eprintln!(
        "NOTIFICATIONS settings seconds={} toasts={} dnd={} quiet={}",
        prefs.toast_seconds, prefs.max_toasts, prefs.dnd, quiet
    );
    quiet
}

/// Applies the settings now, whenever the file changes, and when the hour crosses into or out of quiet hours (a stat once a second).
fn watch_prefs(app: AppHandle) {
    let mut quiet = apply_prefs(&app);
    tauri::async_runtime::spawn(async move {
        let stamp = || {
            prefs_path()
                .and_then(|p| std::fs::metadata(p).ok())
                .and_then(|m| m.modified().ok())
        };
        let mut seen = stamp();
        let mut hour = local_hour();
        loop {
            tokio::time::sleep(Duration::from_secs(1)).await;
            let now = stamp();
            let now_hour = local_hour();
            if now == seen && now_hour == hour {
                continue;
            }
            let file_changed = now != seen;
            seen = now;
            hour = now_hour;
            if !file_changed && read_prefs().quiet_at(now_hour) == quiet {
                continue;
            }
            quiet = apply_prefs(&app);
            changed(&app, Vec::new()).await;
        }
    });
}

fn refused(error: RequestError) -> String {
    format!("{error:?}")
}

#[tauri::command]
pub fn notifications_view(app: AppHandle) -> NotificationsView {
    view(&app)
}

#[tauri::command]
pub async fn notification_dismiss(app: AppHandle, id: u32) -> Result<(), String> {
    let events = shell_state(&app)
        .lock()
        .expect("shell state")
        .notifications
        .dismiss(id)
        .map_err(refused)?;
    changed(&app, events).await;
    Ok(())
}

#[tauri::command]
pub async fn notification_clear(app: AppHandle) {
    let events = shell_state(&app)
        .lock()
        .expect("shell state")
        .notifications
        .clear();
    changed(&app, events).await;
}

/// Presses the `index`th button of a live notification. The key sent to the application comes from the store, never from here.
#[tauri::command]
pub async fn notification_invoke(
    app: AppHandle,
    id: u32,
    revision: u32,
    index: usize,
) -> Result<(), String> {
    let events = shell_state(&app)
        .lock()
        .expect("shell state")
        .notifications
        .invoke(id, revision, index)
        .map_err(refused)?;
    changed(&app, events).await;
    Ok(())
}

#[tauri::command]
pub async fn notification_set_dnd(app: AppHandle, on: bool) {
    shell_state(&app)
        .lock()
        .expect("shell state")
        .notifications
        .set_dnd(on);
    persist_dnd(on);
    changed(&app, Vec::new()).await;
}

#[tauri::command]
pub async fn notifications_read(app: AppHandle) {
    shell_state(&app)
        .lock()
        .expect("shell state")
        .notifications
        .mark_all_read();
    changed(&app, Vec::new()).await;
}

/// Flips Do not disturb (what a key binding or the palette wants).
pub async fn toggle_dnd(app: &AppHandle) {
    {
        let state = shell_state(app);
        let mut shell = state.lock().expect("shell state");
        let now = now_ms(shell.started);
        let on = shell.notifications.view(now).dnd;
        shell.notifications.set_dnd(!on);
        drop(shell);
        persist_dnd(!on);
    }
    changed(app, Vec::new()).await;
}

/// Whether do-not-disturb is on.
pub fn dnd_is_on(app: &AppHandle) -> bool {
    let state = shell_state(app);
    let shell = state.lock().expect("shell state");
    let now = now_ms(shell.started);
    shell.notifications.view(now).dnd
}

/// The notifications clock's "now" for a state the caller already holds.
pub fn now_ms_of(shell: &crate::ShellState) -> u64 {
    now_ms(shell.started)
}
