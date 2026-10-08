//! Helix Settings. A separate Tauri application with one window and a page per area; this file is the thin layer between the pages and
//! `helix-shell-core` / `helix-desktop-api`. It holds the Agent1 *manager* role (and nothing else of Agent1): it can read who has which
//! tiers and the audit, change tiers and revoke, but it is not an agent, cannot approve anything, and never talks to the model. What a
//! change may be is decided by the pure models in `helix-shell-core`; the page only sends names those models listed.

use std::sync::{Arc, Mutex};
use std::time::Duration;

use helix_desktop_api::{Desktop, Domain};
use helix_shell_core::bluetooth::{Bluetooth, BluetoothView, Request as BtRequest, RequestError as BtError};
use helix_shell_core::network::{Network, NetworkView, Request as NetRequest, RequestError as NetError};
use helix_wallpaper::picker::{Picker, PickerView};
use helix_wallpaper::{choice_path, choice_to_toml, load_config, Source};
use helix_shell_core::shortcuts::{self, ShortcutGroup};
use helix_shell_core::catalog::{Catalog, Change, RowView};
use helix_shell_core::grants::{Grants, GrantsView, Request, RequestError};
use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager, WebviewUrl, WebviewWindowBuilder};

const WINDOW: &str = "settings";

/// The service settings the Appearance page shows, in order. A change to any other id is refused before it is sent.
const APPEARANCE: &[&str] = &["theme.mode", "theme.pure_black_dark"];

const DISPLAYS: &[&str] = &[
    "nightlight.enabled",
    "nightlight.force",
    "nightlight.temperature_day",
    "nightlight.temperature_night",
    "brightness.sync_all_monitors",
    "brightness.enable_ddcutil",
    "brightness.minimum_brightness",
];
const ACCESSIBILITY: &[&str] = &["accessibility.ui_scale", "accessibility.high_contrast"];
const SOUND: &[&str] = &["audio.enable_sounds", "audio.sound_volume", "audio.enable_overdrive"];
const NOTIFICATIONS: &[&str] = &[
    "notification.enable_daemon",
    "notification.show_app_name",
    "notification.show_actions",
    "notification.max_visible",
    "notification.collapse_on_dismiss",
    "notification.border",
    "notification.background_opacity",
    "notification.scale",
    "notification.offset_x",
    "notification.offset_y",
    "notification.keep_dismissed_in_history",
    "notification.history_retention_hours",
];
const POWER: &[&str] = &[
    "lockscreen.enabled",
    "lockscreen.lock_before_suspend",
    "lockscreen.lid_close_keep_external_displays",
    "lockscreen.allow_empty_password",
    "lockscreen.blurred_desktop",
    "lockscreen.blur_intensity",
    "lockscreen.tint_intensity",
    "idle.pre_action_fade_seconds",
    "battery.warning_threshold",
];

/// Every page that is only rows of service settings: its name (the page's id in the window) and the ids it shows.
const DOCK: &[&str] = &[
    "dock.enabled",
    "dock.position",
    "dock.auto_hide",
    "dock.smart_auto_hide",
    "dock.reserve_space",
    "dock.active_monitor_only",
    "dock.icon_size",
    "dock.magnification",
    "dock.magnification_scale",
    "dock.show_running",
    "dock.show_dots",
    "dock.show_instance_count",
    "dock.shadow",
    "dock.background_opacity",
    "hot_corners.enabled",
    "hot_corners.delay_ms",
    "hot_corners.top_left.action",
    "hot_corners.top_right.action",
    "hot_corners.bottom_left.action",
    "hot_corners.bottom_right.action",
];

const ROW_PAGES: [(&str, &[&str]); 7] = [
    ("appearance", APPEARANCE),
    ("accessibility", ACCESSIBILITY),
    ("displays", DISPLAYS),
    ("dock", DOCK),
    ("sound", SOUND),
    ("notifications", NOTIFICATIONS),
    ("power", POWER),
];

fn page_ids(page: &str) -> Option<&'static [&'static str]> {
    ROW_PAGES.iter().find(|(name, _)| *name == page).map(|(_, ids)| *ids)
}

struct State {
    grants: Grants,
    catalog: Catalog,
    network: Network,
    bluetooth: Bluetooth,
    picker: Picker,
    status: &'static str,
}

type Shared = Arc<Mutex<State>>;

/// The connection that registered as manager: the service knows the manager by this connection, so every change goes through it.
static DESKTOP: Mutex<Option<Arc<Desktop>>> = Mutex::new(None);

fn current() -> Option<Arc<Desktop>> {
    DESKTOP.lock().expect("desktop").clone()
}

#[derive(Serialize, Clone)]
struct AgentsView {
    /// `connected`, `denied` (this copy is not the installed Settings app) or `unavailable` (no desktop service).
    status: &'static str,
    #[serde(flatten)]
    view: GrantsView,
}

fn shared(app: &AppHandle) -> Shared {
    app.state::<Shared>().inner().clone()
}

fn agents_view_of(state: &State) -> AgentsView {
    AgentsView {
        status: state.status,
        view: state.grants.view(),
    }
}

fn publish(app: &AppHandle) {
    let view = agents_view_of(&shared(app).lock().expect("state"));
    eprintln!(
        "SETTINGS agents status={} clients={}",
        view.status,
        view.view.clients.len()
    );
    let _ = app.emit("agents-view", view);
}

/// Reads grants and the audit again and tells the page.
async fn refresh(app: &AppHandle) {
    let Some(desktop) = current() else { return };
    let manager = desktop.manager();
    let grants = manager.grants().await;
    let audit = manager.audit(0, 400).await;
    {
        let shared = shared(app);
        let mut state = shared.lock().expect("state");
        match grants {
            Ok(grants) => state.grants.set_grants(&grants),
            Err(error) => eprintln!("SETTINGS grants: {error}"),
        }
        if let Ok(audit) = audit {
            state.grants.set_audit(&audit);
        }
    }
    publish(app);
}

#[derive(Serialize, Clone)]
struct RowsView {
    page: &'static str,
    rows: Vec<RowView>,
}


fn rows_of(state: &State) -> Vec<RowsView> {
    ROW_PAGES
        .iter()
        .map(|(page, ids)| RowsView {
            page,
            rows: state.catalog.rows(ids),
        })
        .collect()
}

/// Reads the service's settings again and tells the page.
async fn refresh_settings(app: &AppHandle) {
    let Some(desktop) = current() else { return };
    match desktop.settings().await {
        Ok(settings) => {
            let view = {
                let shared = shared(app);
                let mut state = shared.lock().expect("state");
                state.catalog.set(&settings);
                rows_of(&state)
            };
            for page in view {
                eprintln!("SETTINGS rows page={} n={}", page.page, page.rows.len());
                let _ = app.emit("rows-view", page);
            }
        }
        Err(error) => eprintln!("SETTINGS settings: {error}"),
    }
}

/// Network and Bluetooth: the control center's models, with the same request rules, shown as pages. Each follows its own `Changed` signal.
async fn refresh_network(app: &AppHandle) {
    let Some(desktop) = current() else { return };
    let snapshot = desktop.snapshot(Domain::Network).await.unwrap_or(serde_json::Value::Null);
    let network = Network::from_snapshot(&snapshot);
    let view = network.view();
    shared(app).lock().expect("state").network = network;
    eprintln!("SETTINGS network networks={}", view.networks.len());
    let _ = app.emit("network-view", view);
}

async fn refresh_bluetooth(app: &AppHandle) {
    let Some(desktop) = current() else { return };
    let snapshot = desktop.snapshot(Domain::Bluetooth).await.unwrap_or(serde_json::Value::Null);
    let bluetooth = Bluetooth::from_snapshot(&snapshot);
    let view = bluetooth.view();
    shared(app).lock().expect("state").bluetooth = bluetooth;
    eprintln!("SETTINGS bluetooth devices={}", view.devices.len());
    let _ = app.emit("bluetooth-view", view);
}

fn follow(
    app: &AppHandle,
    desktop: &Arc<Desktop>,
    domain: Domain,
) -> tauri::async_runtime::JoinHandle<()> {
    let (app, desktop) = (app.clone(), desktop.clone());
    tauri::async_runtime::spawn(async move {
        let watcher = app.clone();
        let _ = desktop
            .watch(domain, move || {
                let app = watcher.clone();
                tauri::async_runtime::spawn(async move {
                    match domain {
                        Domain::Network => refresh_network(&app).await,
                        Domain::Bluetooth => refresh_bluetooth(&app).await,
                        Domain::Configuration => refresh_settings(&app).await,
                        _ => {}
                    }
                });
            })
            .await;
    })
}

async fn send_network(request: Result<NetRequest, NetError>) -> Result<(), String> {
    let request = request.map_err(|error| format!("{error:?}"))?;
    let desktop = current().ok_or("not connected to the desktop service")?;
    desktop
        .action(Domain::Network, request.action, &request.args)
        .await
        .map_err(|error| error.to_string())?;
    eprintln!("SETTINGS network {} sent", request.action);
    Ok(())
}

async fn send_bluetooth(request: Result<BtRequest, BtError>) -> Result<(), String> {
    let request = request.map_err(|error| format!("{error:?}"))?;
    let desktop = current().ok_or("not connected to the desktop service")?;
    desktop
        .action(Domain::Bluetooth, request.action, &request.args)
        .await
        .map_err(|error| error.to_string())?;
    eprintln!("SETTINGS bluetooth {} sent", request.action);
    Ok(())
}

fn network_with<T>(app: &AppHandle, build: impl FnOnce(&Network) -> T) -> T {
    build(&shared(app).lock().expect("state").network)
}

fn bluetooth_with<T>(app: &AppHandle, build: impl FnOnce(&Bluetooth) -> T) -> T {
    build(&shared(app).lock().expect("state").bluetooth)
}

#[tauri::command]
fn network_view(app: AppHandle) -> NetworkView {
    network_with(&app, Network::view)
}
#[tauri::command]
async fn network_set_wifi(app: AppHandle, enabled: bool) -> Result<(), String> {
    send_network(network_with(&app, |n| n.set_wifi(enabled))).await
}
#[tauri::command]
async fn network_scan(app: AppHandle) -> Result<(), String> {
    send_network(network_with(&app, Network::scan)).await
}
#[tauri::command]
async fn network_connect(app: AppHandle, path: String) -> Result<(), String> {
    send_network(network_with(&app, |n| n.connect(&path))).await
}
#[tauri::command]
async fn network_disconnect(app: AppHandle) -> Result<(), String> {
    send_network(network_with(&app, Network::disconnect)).await
}
#[tauri::command]
async fn network_forget(app: AppHandle, path: String) -> Result<(), String> {
    send_network(network_with(&app, |n| n.forget(&path))).await
}
#[tauri::command]
async fn network_vpn(app: AppHandle, path: String, active: bool) -> Result<(), String> {
    send_network(network_with(&app, |n| n.set_vpn(&path, active))).await
}

#[tauri::command]
fn bluetooth_view(app: AppHandle) -> BluetoothView {
    bluetooth_with(&app, Bluetooth::view)
}
#[tauri::command]
async fn bluetooth_set_power(app: AppHandle, enabled: bool) -> Result<(), String> {
    send_bluetooth(bluetooth_with(&app, |b| b.set_power(enabled))).await
}
#[tauri::command]
async fn bluetooth_scan(app: AppHandle, on: bool) -> Result<(), String> {
    send_bluetooth(bluetooth_with(&app, |b| b.set_scanning(on))).await
}
#[tauri::command]
async fn bluetooth_connect(app: AppHandle, path: String) -> Result<(), String> {
    send_bluetooth(bluetooth_with(&app, |b| b.connect(&path))).await
}
#[tauri::command]
async fn bluetooth_disconnect(app: AppHandle, path: String) -> Result<(), String> {
    send_bluetooth(bluetooth_with(&app, |b| b.disconnect(&path))).await
}
#[tauri::command]
async fn bluetooth_forget(app: AppHandle, path: String) -> Result<(), String> {
    send_bluetooth(bluetooth_with(&app, |b| b.forget(&path))).await
}

fn set_status(app: &AppHandle, status: &'static str) {
    let changed = {
        let shared = shared(app);
        let mut state = shared.lock().expect("state");
        std::mem::replace(&mut state.status, status) != status
    };
    if changed {
        publish(app);
    }
}

/// Keeps the manager role: registers (retrying while the service is absent or refuses), reads, and follows `Changed`.
fn connect(app: AppHandle) {
    tauri::async_runtime::spawn(async move {
        loop {
            match Desktop::session().await {
                Ok(desktop) => {
                    let desktop = Arc::new(desktop);
                    *DESKTOP.lock().expect("desktop") = Some(desktop.clone());
                    // Everything but the agent pages needs no special role: settings, Wi-Fi and Bluetooth.
                    refresh_settings(&app).await;
                    refresh_network(&app).await;
                    refresh_bluetooth(&app).await;
                    let tasks = [
                        follow(&app, &desktop, Domain::Configuration),
                        follow(&app, &desktop, Domain::Network),
                        follow(&app, &desktop, Domain::Bluetooth),
                    ];
                    match desktop.manager().register().await {
                        Ok(()) => {
                            set_status(&app, "connected");
                            refresh(&app).await;
                            let watcher = app.clone();
                            let _ = desktop
                                .manager()
                                .watch(move || {
                                    let app = watcher.clone();
                                    tauri::async_runtime::spawn(async move { refresh(&app).await });
                                })
                                .await;
                        }
                        Err(error) => {
                            set_status(
                                &app,
                                if error.to_string().contains("Only Helix Settings") {
                                    "denied"
                                } else {
                                    "unavailable"
                                },
                            );
                            // Keep this connection until the service goes away; the other pages still work.
                            for task in tasks {
                                let _ = task.await;
                            }
                        }
                    }
                }
                Err(_) => set_status(&app, "unavailable"),
            }
            *DESKTOP.lock().expect("desktop") = None;
            tokio::time::sleep(Duration::from_secs(3)).await;
        }
    });
}

#[tauri::command]
fn agents_view(app: AppHandle) -> AgentsView {
    agents_view_of(&shared(&app).lock().expect("state"))
}

async fn send(app: &AppHandle, request: Result<Request, RequestError>) -> Result<(), String> {
    let request = request.map_err(|error| format!("{error:?}"))?;
    let desktop = current().ok_or("not connected to the desktop service")?;
    let manager = desktop.manager();
    match &request {
        Request::SetGrant { client, tiers } => manager.set_grant(client, tiers).await,
        Request::Revoke { client } => manager.revoke(client).await,
    }
    .map_err(|error| error.to_string())?;
    eprintln!(
        "SETTINGS agents {}",
        match &request {
            Request::SetGrant { .. } => "set",
            Request::Revoke { .. } => "revoke",
        }
    );
    refresh(app).await;
    Ok(())
}

#[tauri::command]
async fn agents_set_tier(
    app: AppHandle,
    client: String,
    tier: String,
    on: bool,
) -> Result<(), String> {
    let request = shared(&app)
        .lock()
        .expect("state")
        .grants
        .set_tier(&client, &tier, on);
    send(&app, request).await
}

#[tauri::command]
async fn agents_revoke(app: AppHandle, client: String) -> Result<(), String> {
    let request = shared(&app).lock().expect("state").grants.revoke(&client);
    send(&app, request).await
}

#[derive(Serialize)]
struct AboutView {
    version: &'static str,
    service: &'static str,
    agent_access: &'static str,
    settings: usize,
    network: bool,
    bluetooth: bool,
}

#[tauri::command]
fn about(app: AppHandle) -> AboutView {
    let shared = shared(&app);
    let state = shared.lock().expect("state");
    AboutView {
        version: env!("CARGO_PKG_VERSION"),
        service: if current().is_some() { "Running" } else { "Not running" },
        agent_access: match state.status {
            "connected" => "Managing",
            "denied" => "Not permitted: this is not the installed Settings",
            _ => "Unavailable",
        },
        settings: ROW_PAGES.iter().map(|(_, ids)| state.catalog.rows(ids).len()).sum(),
        network: state.network.view().available,
        bluetooth: state.bluetooth.view().available,
    }
}

/// The key binds in the compositor's config files (read only: Settings never writes them).
#[tauri::command]
fn shortcuts_view() -> Vec<ShortcutGroup> {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(std::path::PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| std::path::PathBuf::from(home).join(".config")));
    let Some(dir) = base.map(|base| base.join("niri")) else { return Vec::new() };
    let mut files: Vec<_> = std::fs::read_dir(&dir)
        .into_iter()
        .flatten()
        .flatten()
        .map(|entry| entry.path())
        .filter(|path| path.extension().is_some_and(|ext| ext == "kdl") && path.is_file())
        .collect();
    files.sort();
    let mut binds = Vec::new();
    for path in files.into_iter().take(40) {
        // A config file is small; anything bigger is not one.
        if std::fs::metadata(&path).map(|m| m.len() > 512 * 1024).unwrap_or(true) {
            continue;
        }
        if let (Ok(text), Some(name)) = (std::fs::read_to_string(&path), path.file_name()) {
            binds.extend(shortcuts::parse(&text, &name.to_string_lossy()));
        }
    }
    shortcuts::view(&binds)
}

// Wallpaper -------------------------------------------------------------------------------------------------------------------------
fn home() -> String {
    std::env::var("HOME").unwrap_or_default()
}

/// The folder the picker offers: the service's wallpaper folder if one is set, otherwise Pictures/Wallpapers.
fn wallpaper_folder(app: &AppHandle) -> std::path::PathBuf {
    let shared = shared(app);
    let state = shared.lock().expect("state");
    let from_service = state
        .catalog
        .text_of("wallpaper.directory")
        .filter(|path| path.starts_with('/'));
    from_service
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| std::path::PathBuf::from(home()).join("Pictures/Wallpapers"))
}

fn scan_wallpapers(app: &AppHandle) -> PickerView {
    let folder = wallpaper_folder(app);
    let files: Vec<std::path::PathBuf> = std::fs::read_dir(&folder)
        .into_iter()
        .flatten()
        .flatten()
        .take(5000)
        .map(|entry| entry.path())
        .filter(|path| path.is_file())
        .collect();
    let current = load_config(&home()).default;
    let shared = shared(app);
    let mut state = shared.lock().expect("state");
    state.picker.set_folder(&folder, files);
    state.picker.set_current(current);
    state.picker.view()
}

#[tauri::command]
fn wallpaper_view(app: AppHandle) -> PickerView {
    scan_wallpapers(&app)
}

/// A small picture of listed image number `index`, as a data URL. Only listed images can be asked for.
#[tauri::command]
async fn wallpaper_thumb(app: AppHandle, index: usize) -> Option<String> {
    let path = shared(&app).lock().expect("state").picker.file(index)?.to_path_buf();
    tauri::async_runtime::spawn_blocking(move || {
        use base64::Engine;
        let image = image::open(&path).ok()?.thumbnail(360, 225).to_rgb8();
        let mut bytes = Vec::new();
        image::codecs::jpeg::JpegEncoder::new_with_quality(&mut bytes, 80)
            .encode_image(&image)
            .ok()?;
        Some(format!(
            "data:image/jpeg;base64,{}",
            base64::engine::general_purpose::STANDARD.encode(bytes)
        ))
    })
    .await
    .ok()
    .flatten()
}

/// Writes the choice where the wallpaper process looks (atomically, owner only) and tells the page. The process reloads by itself.
fn write_choice(app: &AppHandle, source: Source) -> Result<(), String> {
    let path = choice_path(&home());
    let parent = path.parent().ok_or("no folder for the choice")?;
    std::fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    let temporary = path.with_extension("tmp");
    {
        use std::io::Write;
        use std::os::unix::fs::OpenOptionsExt;
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(&temporary)
            .map_err(|error| error.to_string())?;
        file.write_all(choice_to_toml(&source).as_bytes())
            .map_err(|error| error.to_string())?;
    }
    std::fs::rename(&temporary, &path).map_err(|error| error.to_string())?;
    eprintln!("SETTINGS wallpaper chosen");
    let view = scan_wallpapers(app);
    let _ = app.emit("wallpaper-view", view);
    Ok(())
}

#[tauri::command]
fn wallpaper_choose_image(app: AppHandle, index: usize) -> Result<(), String> {
    let source = shared(&app)
        .lock()
        .expect("state")
        .picker
        .choose_image(index)
        .map_err(|error| format!("{error:?}"))?;
    write_choice(&app, source)
}

#[tauri::command]
fn wallpaper_choose_swatch(app: AppHandle, index: usize) -> Result<(), String> {
    let source = shared(&app)
        .lock()
        .expect("state")
        .picker
        .choose_swatch(index)
        .map_err(|error| format!("{error:?}"))?;
    write_choice(&app, source)
}

#[tauri::command]
fn rows_view(app: AppHandle) -> Vec<RowsView> {
    rows_of(&shared(&app).lock().expect("state"))
}

/// A change to one row. The page names the row and what it wants; the catalog model decides whether that is a legal write.
#[tauri::command]
async fn setting_change(
    app: AppHandle,
    page: String,
    id: String,
    change: serde_json::Value,
) -> Result<(), String> {
    let ids = page_ids(&page).ok_or("unknown page")?;
    let change = match (
        change.get("kind").and_then(|k| k.as_str()),
        change.get("value"),
    ) {
        (Some("switch"), Some(v)) => Change::Switch(v.as_bool().ok_or("not a boolean")?),
        (Some("choice"), Some(v)) => Change::Choice(v.as_str().ok_or("not text")?.to_string()),
        (Some("number"), Some(v)) => Change::Number(v.as_f64().ok_or("not a number")?),
        (Some("text"), Some(v)) => Change::Text(v.as_str().ok_or("not text")?.to_string()),
        _ => return Err("unknown change".into()),
    };
    let update = shared(&app)
        .lock()
        .expect("state")
        .catalog
        .update(ids, &id, change)
        .map_err(|error| format!("{error:?}"))?;
    let desktop = current().ok_or("not connected to the desktop service")?;
    desktop
        .update_setting(&update.id, &update.value)
        .await
        .map_err(|error| error.to_string())?;
    eprintln!("SETTINGS set {} {}", update.id, update.value);
    refresh_settings(&app).await;
    Ok(())
}

#[tauri::command]
async fn agents_refresh(app: AppHandle) {
    refresh(&app).await;
}

/// Whether the page should report where its controls are (a test asked for it).
#[tauri::command]
fn layout_reporting() -> bool {
    std::env::var_os("HELIX_UI_LAYOUT_LOG").is_some()
}

#[tauri::command]
fn report_layout(report: serde_json::Value) {
    let Some(path) = std::env::var_os("HELIX_UI_LAYOUT_LOG") else {
        return;
    };
    let path = std::path::PathBuf::from(path);
    let text = report.to_string();
    if text.len() > 64 * 1024 {
        return;
    }
    let temporary = path.with_extension("tmp");
    if std::fs::write(&temporary, &text).is_ok() {
        let _ = std::fs::rename(&temporary, &path);
    }
}

fn main() {
    // The web view's JIT costs memory this app does not need (same choice as the shell).
    // The window announces this application id, which is what the desktop file org.helix.Settings.desktop (name and icon) is matched by.
    gtk::glib::set_prgname(Some("org.helix.Settings"));
    if std::env::var_os("JSC_useJIT").is_none() {
        std::env::set_var("JSC_useJIT", "false");
    }
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            agents_view,
            agents_set_tier,
            agents_revoke,
            agents_refresh,
            rows_view,
            wallpaper_view,
            wallpaper_thumb,
            wallpaper_choose_image,
            wallpaper_choose_swatch,
            shortcuts_view,
            about,
            network_view,
            network_set_wifi,
            network_scan,
            network_connect,
            network_disconnect,
            network_forget,
            network_vpn,
            bluetooth_view,
            bluetooth_set_power,
            bluetooth_scan,
            bluetooth_connect,
            bluetooth_disconnect,
            bluetooth_forget,
            setting_change,
            layout_reporting,
            report_layout
        ])
        .setup(|app| {
            app.manage::<Shared>(Arc::new(Mutex::new(State {
                grants: Grants::default(),
                catalog: Catalog::default(),
                picker: Picker::default(),
                network: Network::from_snapshot(&serde_json::Value::Null),
                bluetooth: Bluetooth::from_snapshot(&serde_json::Value::Null),
                status: "unavailable",
            })));
            WebviewWindowBuilder::new(app, WINDOW, WebviewUrl::App("index.html".into()))
                .title("Settings")
                .decorations(false)
                .transparent(true)
                .inner_size(1040.0, 720.0)
                .min_inner_size(800.0, 560.0)
                .build()?;
            connect(app.handle().clone());
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running Helix Settings");
}
