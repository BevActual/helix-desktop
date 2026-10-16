//! Helix Settings. A separate Tauri application with one window and a page per area; this file is the thin layer between the pages and
//! the core crates / `helix-desktop-api`. It holds the Agent Surface *manager* role (and nothing else of Agent Surface): it can read who has which
//! tiers and the audit, change tiers and revoke, but it is not an agent, cannot approve anything, and never talks to the model. What a
//! change may be is decided by the pure models in the core crates; the page only sends names those models listed.

mod wallhaven;

use std::sync::{Arc, Mutex};
use std::time::Duration;

use helix_desktop_api::{Desktop, Domain};
use helix_models::audio::{
    Audio, AudioView, Request as AudioRequest, RequestError as AudioError,
};
use helix_models::bluetooth::{
    Bluetooth, BluetoothView, Request as BtRequest, RequestError as BtError,
};
use helix_config::catalog::{Catalog, Change, RowView};
use helix_agent::grants::{Grants, GrantsView, Request, RequestError};
use helix_config::keybinds::{self, KeybindRow};
use helix_models::network::{
    EnterpriseInput, Network, NetworkView, Request as NetRequest, RequestError as NetError,
};
use helix_models::pairing::{Pairing, PairingView};
use helix_compositor::shortcuts::{self, ShortcutGroup};
use helix_wallpaper::picker::{Picker, PickerView};
use helix_wallpaper::{choice_path, choice_to_toml, load_config, Source};
use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager, WebviewUrl, WebviewWindowBuilder};

const WINDOW: &str = "settings";

/// The service settings the Appearance page shows, in order. A change to any other id is refused before it is sent.
const APPEARANCE: &[&str] = &[
    "theme.mode",
    "theme.pure_black_dark",
    "shell.greeter_sync.auto_sync",
    "shell.greeter_sync.privilege_command",
];

/// Only what something on this desktop acts on (the service applies high contrast to GTK applications).
const ACCESSIBILITY: &[&str] = &["accessibility.high_contrast"];

/// Every page that is only rows of service settings: its name (the page's id in the window) and the ids it shows.
const ROW_PAGES: [(&str, &[&str]); 2] =
    [("appearance", APPEARANCE), ("accessibility", ACCESSIBILITY)];

fn page_ids(page: &str) -> Option<&'static [&'static str]> {
    ROW_PAGES
        .iter()
        .find(|(name, _)| *name == page)
        .map(|(_, ids)| *ids)
}

struct State {
    grants: Grants,
    catalog: Catalog,
    network: Network,
    bluetooth: Bluetooth,
    audio: Audio,
    picker: Picker,
    pairing: Pairing,
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
    let snapshot = desktop
        .snapshot(Domain::Network)
        .await
        .unwrap_or(serde_json::Value::Null);
    let network = Network::from_snapshot(&snapshot);
    let view = network.view();
    shared(app).lock().expect("state").network = network;
    eprintln!("SETTINGS network networks={}", view.networks.len());
    let _ = app.emit("network-view", view);
}

async fn refresh_audio(app: &AppHandle) {
    let Some(desktop) = current() else { return };
    let snapshot = desktop
        .snapshot(Domain::Audio)
        .await
        .unwrap_or(serde_json::Value::Null);
    let audio = Audio::from_snapshot(&snapshot);
    let view = audio.view();
    shared(app).lock().expect("state").audio = audio;
    eprintln!(
        "SETTINGS audio outputs={} inputs={}",
        view.outputs.len(),
        view.inputs.len()
    );
    let _ = app.emit("audio-view", view);
}

async fn refresh_bluetooth(app: &AppHandle) {
    let Some(desktop) = current() else { return };
    let snapshot = desktop
        .snapshot(Domain::Bluetooth)
        .await
        .unwrap_or(serde_json::Value::Null);
    let bluetooth = Bluetooth::from_snapshot(&snapshot);
    let view = bluetooth.view();
    {
        let shared = shared(app);
        let mut state = shared.lock().expect("state");
        state.pairing.follow(&bluetooth);
        state.bluetooth = bluetooth;
    }
    eprintln!("SETTINGS bluetooth devices={}", view.devices.len());
    let _ = app.emit("bluetooth-view", view);
    publish_pairing(app);
}

fn pairing_view_of(state: &State) -> PairingView {
    state.pairing.view(&state.bluetooth)
}

fn publish_pairing(app: &AppHandle) {
    let view = pairing_view_of(&shared(app).lock().expect("state"));
    eprintln!(
        "SETTINGS pairing active={} stage={}",
        view.active, view.stage
    );
    let _ = app.emit("pairing-view", view);
}

#[tauri::command]
fn pairing_view(app: AppHandle) -> PairingView {
    pairing_view_of(&shared(&app).lock().expect("state"))
}

/// Starts pairing with an unpaired device; the questions arrive as `Prompt` signals.
#[tauri::command]
async fn bluetooth_pair(app: AppHandle, path: String) -> Result<(), String> {
    let request = {
        let shared = shared(&app);
        let mut state = shared.lock().expect("state");
        let State {
            pairing, bluetooth, ..
        } = &mut *state;
        pairing
            .begin(bluetooth, &path)
            .map_err(|error| format!("{error:?}"))?
    };
    publish_pairing(&app);
    match send_bluetooth_operation(Ok(request)).await {
        Ok(operation) => {
            shared(&app)
                .lock()
                .expect("state")
                .pairing
                .set_operation(operation);
            publish_pairing(&app);
            Ok(())
        }
        Err(error) => {
            shared(&app).lock().expect("state").pairing.finish();
            publish_pairing(&app);
            Err(error)
        }
    }
}

async fn send_reply(
    app: &AppHandle,
    reply: Result<helix_models::pairing::Reply, helix_models::pairing::PairingError>,
) -> Result<(), String> {
    let reply = reply.map_err(|error| format!("{error:?}"))?;
    let desktop = current().ok_or("not connected to the desktop service")?;
    desktop
        .bluetooth_reply(&reply.response, reply.cancel)
        .await
        .map_err(|error| error.to_string())?;
    eprintln!("SETTINGS pairing answered");
    publish_pairing(app);
    Ok(())
}

/// A PIN or passkey the person typed. Used for this one answer and kept nowhere.
#[tauri::command]
async fn bluetooth_pair_answer(app: AppHandle, text: String) -> Result<(), String> {
    let reply = shared(&app)
        .lock()
        .expect("state")
        .pairing
        .answer_text(&text);
    send_reply(&app, reply).await
}

#[tauri::command]
async fn bluetooth_pair_accept(app: AppHandle) -> Result<(), String> {
    let reply = shared(&app).lock().expect("state").pairing.accept();
    send_reply(&app, reply).await
}

#[tauri::command]
async fn bluetooth_pair_cancel(app: AppHandle) -> Result<(), String> {
    let request = shared(&app).lock().expect("state").pairing.cancel();
    publish_pairing(&app);
    match request {
        Ok(request) => send_bluetooth(Ok(request)).await,
        // Nothing was under way, so there is nothing to cancel.
        Err(_) => Ok(()),
    }
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
                        Domain::Audio => refresh_audio(&app).await,
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

async fn send_audio(request: Result<AudioRequest, AudioError>) -> Result<(), String> {
    let request = request.map_err(|error| format!("{error:?}"))?;
    let desktop = current().ok_or("not connected to the desktop service")?;
    desktop
        .action(Domain::Audio, request.action, &request.args)
        .await
        .map_err(|error| error.to_string())?;
    eprintln!("SETTINGS audio {} sent", request.action);
    Ok(())
}

async fn send_bluetooth(request: Result<BtRequest, BtError>) -> Result<(), String> {
    send_bluetooth_operation(request).await.map(|_| ())
}

/// Like `send_bluetooth`, giving the service's id for the operation.
async fn send_bluetooth_operation(request: Result<BtRequest, BtError>) -> Result<u64, String> {
    let request = request.map_err(|error| format!("{error:?}"))?;
    let desktop = current().ok_or("not connected to the desktop service")?;
    let operation = desktop
        .action(Domain::Bluetooth, request.action, &request.args)
        .await
        .map_err(|error| error.to_string())?;
    eprintln!("SETTINGS bluetooth {} sent", request.action);
    Ok(operation)
}

fn network_with<T>(app: &AppHandle, build: impl FnOnce(&Network) -> T) -> T {
    build(&shared(app).lock().expect("state").network)
}

fn audio_with<T>(app: &AppHandle, build: impl FnOnce(&Audio) -> T) -> T {
    build(&shared(app).lock().expect("state").audio)
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
/// Joins a secured network with the password the person typed. The password is used for this one request and kept nowhere.
#[tauri::command]
async fn network_connect_password(
    app: AppHandle,
    path: String,
    password: String,
) -> Result<(), String> {
    send_network(network_with(&app, |n| {
        n.connect_with_password(&path, &password)
    }))
    .await
}

/// What the sign-in form sends: every field as typed.
#[derive(serde::Deserialize)]
struct EnterpriseForm {
    identity: String,
    anonymous: String,
    password: String,
    certificate: String,
    domain: String,
    eap: String,
    phase2: String,
}

/// Joins an enterprise network with what the person typed. Used for this one request and kept nowhere.
#[tauri::command]
async fn network_connect_enterprise(
    app: AppHandle,
    path: String,
    form: EnterpriseForm,
) -> Result<(), String> {
    let input = EnterpriseInput {
        identity: form.identity,
        anonymous: form.anonymous,
        password: form.password,
        certificate: form.certificate,
        domain: form.domain,
        eap: form.eap,
        phase2: form.phase2,
    };
    send_network(network_with(&app, |n| n.connect_enterprise(&path, &input))).await
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
fn audio_view(app: AppHandle) -> AudioView {
    audio_with(&app, Audio::view)
}
#[tauri::command]
async fn audio_set_volume(app: AppHandle, percent: f64) -> Result<(), String> {
    send_audio(audio_with(&app, |a| a.set_volume(percent))).await
}
#[tauri::command]
async fn audio_set_muted(app: AppHandle, muted: bool) -> Result<(), String> {
    send_audio(audio_with(&app, |a| a.set_muted(muted))).await
}
#[tauri::command]
async fn audio_set_default(app: AppHandle, id: u32) -> Result<(), String> {
    send_audio(audio_with(&app, |a| a.set_default(id))).await
}
#[tauri::command]
async fn audio_set_input_volume(app: AppHandle, percent: f64) -> Result<(), String> {
    send_audio(audio_with(&app, |a| a.set_input_volume(percent))).await
}
#[tauri::command]
async fn audio_set_input_muted(app: AppHandle, muted: bool) -> Result<(), String> {
    send_audio(audio_with(&app, |a| a.set_input_muted(muted))).await
}
#[tauri::command]
async fn audio_set_default_input(app: AppHandle, id: u32) -> Result<(), String> {
    send_audio(audio_with(&app, |a| a.set_default_input(id))).await
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
                    refresh_audio(&app).await;
                    let tasks = [
                        follow(&app, &desktop, Domain::Configuration),
                        follow(&app, &desktop, Domain::Network),
                        follow(&app, &desktop, Domain::Bluetooth),
                        follow(&app, &desktop, Domain::Audio),
                    ];
                    let prompt_app = app.clone();
                    let prompt_desktop = desktop.clone();
                    let prompts = tauri::async_runtime::spawn(async move {
                        let watcher = prompt_app.clone();
                        let _ = prompt_desktop
                            .watch_bluetooth_prompt(move |prompt| {
                                shared(&watcher)
                                    .lock()
                                    .expect("state")
                                    .pairing
                                    .on_prompt(&prompt);
                                publish_pairing(&watcher);
                            })
                            .await;
                    });
                    let done_app = app.clone();
                    let done_desktop = desktop.clone();
                    let completed = tauri::async_runtime::spawn(async move {
                        let watcher = done_app.clone();
                        let _ = done_desktop
                            .watch_completed(Domain::Bluetooth, move |operation, _, _| {
                                // The pairing operation ended, however it ended: the question is over. Other operations (a scan) do not count.
                                shared(&watcher)
                                    .lock()
                                    .expect("state")
                                    .pairing
                                    .operation_done(operation);
                                publish_pairing(&watcher);
                            })
                            .await;
                    });
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
                            prompts.abort();
                            completed.abort();
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
                            prompts.abort();
                            completed.abort();
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
struct CompositorSettingsView {
    config: helix_compositor::compositor_config::CompositorConfigView,
    caps: helix_compositor::compositor_ipc::HelixCaps,
    desk: helix_compositor::compositor_ipc::AgentDeskStatus,
}

#[tauri::command]
fn compositor_view() -> CompositorSettingsView {
    CompositorSettingsView {
        config: helix_compositor::compositor_config::read_view(),
        caps: helix_compositor::compositor_ipc::helix_capabilities(),
        desk: helix_compositor::compositor_ipc::agent_desk_status(),
    }
}

#[tauri::command]
fn compositor_set_agent_desk_name(name: String) -> Result<(), String> {
    helix_compositor::compositor_config::save_snippet(
        &helix_compositor::compositor_config::HelixSnippet {
            agent_desk_name: name,
        },
    )?;
    helix_compositor::compositor_ipc::reload_config();
    Ok(())
}

#[tauri::command]
fn compositor_set_placement(on: bool) -> Result<(), String> {
    let prefs = helix_compositor::compositor_config::CompositorPrefs {
        agent_desk_placement: on,
    };
    helix_compositor::compositor_config::save_prefs(&prefs)
}

#[tauri::command]
fn compositor_focus_agent_desk() -> Result<(), String> {
    let line = serde_json::json!({"Action": {"HelixFocusAgentDesk": {}}}).to_string();
    helix_compositor::compositor_ipc::request_json(&line)
        .ok_or_else(|| "the compositor did not accept focus agent desk".to_string())?;
    Ok(())
}

#[derive(Serialize)]
struct InferenceRuntimeView {
    installed: bool,
    service_active: bool,
}

fn inference_installed() -> bool {
    std::path::Path::new("/usr/local/bin/helix-inference").is_file()
}

fn apply_sudo_askpass(cmd: &mut std::process::Command) {
    if std::env::var_os("SUDO_ASKPASS").is_some() {
        return;
    }
    if let Ok(home) = std::env::var("HOME") {
        let askpass = std::path::PathBuf::from(home).join(".local/bin/helix-askpass");
        if askpass.is_file() {
            cmd.env("SUDO_ASKPASS", askpass);
        }
    }
}

fn spawn_inference_installer(path: &std::path::Path) -> Result<(), std::io::Error> {
    let mut cmd = std::process::Command::new(path);
    cmd.stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null());
    apply_sudo_askpass(&mut cmd);
    cmd.spawn().map(|_| ())
}

fn inference_service_active() -> bool {
    std::process::Command::new("systemctl")
        .args(["--user", "is-active", "--quiet", "helix-inference.service"])
        .status()
        .map(|status| status.success())
        .unwrap_or(false)
}

#[tauri::command]
fn inference_runtime_view() -> InferenceRuntimeView {
    InferenceRuntimeView {
        installed: inference_installed(),
        service_active: inference_service_active(),
    }
}

#[tauri::command]
fn inference_install() -> Result<(), String> {
    if inference_installed() {
        return Err("helix-inference is already installed under /usr/local/bin.".into());
    }
    let mut candidates: Vec<std::path::PathBuf> = Vec::new();
    if let Ok(home) = std::env::var("HOME") {
        candidates.push(std::path::PathBuf::from(home).join(".local/bin/helix-install-inference"));
    }
    for path in candidates {
        if !path.is_file() {
            continue;
        }
        if spawn_inference_installer(&path).is_ok() {
            return Ok(());
        }
    }
    if spawn_inference_installer(std::path::Path::new("helix-install-inference")).is_ok() {
        return Ok(());
    }
    Err(
        "helix-install-inference is not on PATH. Run tools/switch-shell.sh install, then helix-install-inference from a terminal."
            .into(),
    )
}

#[tauri::command]
fn inference_restart_service() -> Result<(), String> {
    if !inference_installed() {
        return Err("Install the model runtime first.".into());
    }
    let status = std::process::Command::new("systemctl")
        .args(["--user", "restart", "helix-inference.service"])
        .status()
        .map_err(|error| format!("could not restart helix-inference: {error}"))?;
    if status.success() {
        Ok(())
    } else {
        Err("systemctl could not restart helix-inference.service".into())
    }
}

#[tauri::command]
fn compositor_open_config(which: String) -> Result<(), String> {
    let dir = helix_compositor::compositor_config::niri_config_dir().ok_or("no config home")?;
    let path = helix_compositor::compositor_config::resolve_include_path(&dir, &which)
        .ok_or_else(|| {
            if which == "main" {
                "niri config.kdl is not present yet".into()
            } else {
                format!("{which} is not on disk yet — Helix will create it when you save related settings")
            }
        })?;
    let path = path.to_string_lossy().into_owned();
    if path.is_empty() {
        return Err("that file is not configured yet".into());
    }
    std::process::Command::new("xdg-open")
        .arg(&path)
        .spawn()
        .map_err(|error| format!("could not open the file: {error}"))?;
    Ok(())
}

/// Deep-link into Mobius for models & routing (grants stay in Settings).
#[tauri::command]
fn open_mobius() -> Result<(), String> {
    for bin in ["helix-mobius"] {
        match std::process::Command::new(bin)
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .spawn()
        {
            Ok(_) => return Ok(()),
            Err(_) => continue,
        }
    }
    Err("Mobius is not installed (helix-mobius).".into())
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
        service: if current().is_some() {
            "Running"
        } else {
            "Not running"
        },
        agent_access: match state.status {
            "connected" => "Managing",
            "denied" => "Not permitted: this is not the installed Settings",
            _ => "Unavailable",
        },
        settings: ROW_PAGES
            .iter()
            .map(|(_, ids)| state.catalog.rows(ids).len())
            .sum(),
        network: state.network.view().available,
        bluetooth: state.bluetooth.view().available,
    }
}

/// The key binds in the compositor's config files (read only: Settings never writes them).
#[tauri::command]
fn shortcuts_view() -> Vec<ShortcutGroup> {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(std::path::PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME").map(|home| std::path::PathBuf::from(home).join(".config"))
        });
    let Some(dir) = base.map(|base| base.join("niri")) else {
        return Vec::new();
    };
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
        if std::fs::metadata(&path)
            .map(|m| m.len() > 512 * 1024)
            .unwrap_or(true)
        {
            continue;
        }
        if let (Ok(text), Some(name)) = (std::fs::read_to_string(&path), path.file_name()) {
            binds.extend(shortcuts::parse(&text, &name.to_string_lossy()));
        }
    }
    shortcuts::view(&binds)
}

/// Helix Shell Navigation keybinds from the service catalog (editable via UpdateOrderedList).
#[tauri::command]
fn keybinds_view(app: AppHandle) -> Vec<KeybindRow> {
    let shared = shared(&app);
    let state = shared.lock().expect("state");
    keybinds::view(state.catalog.entries())
}

/// Replace the chords for one navigation action. The pure model validates; the service compares the baseline.
#[tauri::command]
async fn keybinds_set(app: AppHandle, id: String, chords: String) -> Result<(), String> {
    let candidate: Vec<String> = chords
        .split(';')
        .map(str::trim)
        .filter(|part| !part.is_empty())
        .map(str::to_string)
        .collect();
    let (baseline, next) = {
        let shared = shared(&app);
        let state = shared.lock().expect("state");
        let baseline = keybinds::baseline(state.catalog.entries(), &id).ok_or("unknown keybind")?;
        let next = keybinds::update(state.catalog.entries(), &id, &candidate)
            .map_err(|error| format!("{error:?}"))?;
        (baseline, next)
    };
    let desktop = current().ok_or("not connected to the desktop service")?;
    if let Err(error) = desktop.update_ordered_list(&id, &baseline, &next).await {
        eprintln!("SETTINGS keybinds {id} refused: {error}");
        return Err(error.to_string());
    }
    eprintln!("SETTINGS keybinds {id} set");
    refresh_settings(&app).await;
    Ok(())
}

// Wallpaper -------------------------------------------------------------------------------------------------------------------------
fn home() -> String {
    std::env::var("HOME").unwrap_or_default()
}

/// The folder the picker offers: the service's wallpaper folder if one is set, otherwise Pictures/Wallpapers.
pub(crate) fn wallpaper_folder(app: &AppHandle) -> std::path::PathBuf {
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
    let path = shared(&app)
        .lock()
        .expect("state")
        .picker
        .file(index)?
        .to_path_buf();
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
pub(crate) fn write_choice(app: &AppHandle, source: Source) -> Result<(), String> {
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
    greeter_auto_sync(app, "wallpaper");
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

/// Whether approvals are allowed on a desktop that cannot protect the dialog (off unless chosen). Stored where the shell reads it.
fn approvals_path() -> Option<std::path::PathBuf> {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(std::path::PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME").map(|h| std::path::PathBuf::from(h).join(".config"))
        })?;
    Some(base.join("helix/approvals.json"))
}

#[tauri::command]
fn approvals_view() -> bool {
    approvals_path()
        .and_then(|path| std::fs::read_to_string(path).ok())
        .is_some_and(|text| helix_agent::approvals::allow_unprotected(&text))
}

#[tauri::command]
fn approvals_set(allow: bool) -> Result<bool, String> {
    use std::io::Write;
    use std::os::unix::fs::OpenOptionsExt;
    let path = approvals_path().ok_or("no folder for the choice")?;
    std::fs::create_dir_all(path.parent().ok_or("no folder")?).map_err(|e| e.to_string())?;
    let temporary = path.with_extension("tmp");
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(0o600)
        .open(&temporary)
        .map_err(|e| e.to_string())?;
    file.write_all(helix_agent::approvals::render(allow).as_bytes())
        .map_err(|e| e.to_string())?;
    std::fs::rename(&temporary, &path).map_err(|e| e.to_string())?;
    eprintln!("SETTINGS approvals allow_unprotected={allow}");
    Ok(allow)
}

/// A page reports something that went wrong (only the start of it, and never anything a person typed).
#[tauri::command]
fn ui_error(text: String) {
    eprintln!(
        "SETTINGS ui error: {}",
        text.chars().take(200).collect::<String>()
    );
}

#[tauri::command]
fn rows_view(app: AppHandle) -> Vec<RowsView> {
    rows_of(&shared(&app).lock().expect("state"))
}

fn dock_prefs_file() -> Option<std::path::PathBuf> {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(std::path::PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME").map(|h| std::path::PathBuf::from(h).join(".config"))
        })?;
    Some(base.join("helix/dock-settings.json"))
}

fn read_dock_prefs() -> helix_config::dock_prefs::DockPrefs {
    dock_prefs_file()
        .and_then(|path| std::fs::read_to_string(path).ok())
        .map(|text| helix_config::dock_prefs::DockPrefs::parse(&text))
        .unwrap_or_default()
}

/// The Dock page: settings the shell honours, kept in a file the shell watches.
#[tauri::command]
fn dock_rows() -> Vec<RowView> {
    read_dock_prefs().rows()
}

#[tauri::command]
fn dock_change(id: String, change: serde_json::Value) -> Result<Vec<RowView>, String> {
    use std::io::Write;
    use std::os::unix::fs::OpenOptionsExt;
    let change = match (
        change.get("kind").and_then(|k| k.as_str()),
        change.get("value"),
    ) {
        (Some("switch"), Some(v)) => Change::Switch(v.as_bool().ok_or("not a boolean")?),
        (Some("number"), Some(v)) => Change::Number(v.as_f64().ok_or("not a number")?),
        _ => return Err("unknown change".into()),
    };
    let next = read_dock_prefs()
        .apply(&id, &change)
        .map_err(|error| format!("{error:?}"))?;
    let path = dock_prefs_file().ok_or("no folder for the settings")?;
    std::fs::create_dir_all(path.parent().ok_or("no folder")?).map_err(|e| e.to_string())?;
    let temporary = path.with_extension("tmp");
    {
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(&temporary)
            .map_err(|e| e.to_string())?;
        file.write_all(next.to_json().as_bytes())
            .map_err(|e| e.to_string())?;
    }
    std::fs::rename(&temporary, &path).map_err(|e| e.to_string())?;
    eprintln!("SETTINGS dock {id} changed");
    Ok(next.rows())
}

fn idle_prefs_file() -> Option<std::path::PathBuf> {
    dock_prefs_file().map(|p| p.with_file_name("idle.toml"))
}

fn read_idle_prefs() -> (helix_config::idle_prefs::IdlePrefs, String) {
    let text = idle_prefs_file()
        .and_then(|path| std::fs::read_to_string(path).ok())
        .unwrap_or_default();
    (helix_config::idle_prefs::IdlePrefs::parse(&text), text)
}

/// The Power & lock page: when the session locks and the screen goes off (`idle.toml`, read by helix-idle).
#[tauri::command]
fn power_rows() -> Vec<RowView> {
    read_idle_prefs().0.rows()
}

#[tauri::command]
fn power_change(id: String, change: serde_json::Value) -> Result<Vec<RowView>, String> {
    use std::io::Write;
    use std::os::unix::fs::OpenOptionsExt;
    let change = match (
        change.get("kind").and_then(|k| k.as_str()),
        change.get("value"),
    ) {
        (Some("switch"), Some(v)) => Change::Switch(v.as_bool().ok_or("not a boolean")?),
        (Some("number"), Some(v)) => Change::Number(v.as_f64().ok_or("not a number")?),
        _ => return Err("unknown change".into()),
    };
    let (current, text) = read_idle_prefs();
    let next = current
        .apply(&id, &change)
        .map_err(|error| format!("{error:?}"))?;
    let path = idle_prefs_file().ok_or("no folder for the settings")?;
    std::fs::create_dir_all(path.parent().ok_or("no folder")?).map_err(|e| e.to_string())?;
    let temporary = path.with_extension("tmp");
    {
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(&temporary)
            .map_err(|e| e.to_string())?;
        file.write_all(next.write_into(&text).as_bytes())
            .map_err(|e| e.to_string())?;
    }
    std::fs::rename(&temporary, &path).map_err(|e| e.to_string())?;
    // helix-idle reads the file when it starts, so it is started again (a fixed command; nothing typed reaches it).
    if !helix_base::nested_test_session() {
        let _ = std::process::Command::new("systemctl")
            .args(["--user", "restart", "helix-idle.service"])
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status();
    }
    eprintln!("SETTINGS power {id} changed");
    Ok(next.rows())
}

/// The Displays page: what is connected, as the compositor reports it (read only).
#[tauri::command]
fn displays_rows() -> Vec<RowView> {
    use std::io::{BufRead, BufReader, Write};
    use std::os::unix::net::UnixStream;
    let Some(path) = std::env::var_os("NIRI_SOCKET") else {
        return Vec::new();
    };
    let Ok(mut stream) = UnixStream::connect(path) else {
        return Vec::new();
    };
    let _ = stream.set_read_timeout(Some(Duration::from_secs(1)));
    if stream.write_all(b"\"Outputs\"\n").is_err() {
        return Vec::new();
    }
    let mut line = String::new();
    if BufReader::new(stream).read_line(&mut line).is_err() {
        return Vec::new();
    }
    let Ok(reply) = serde_json::from_str::<serde_json::Value>(&line) else {
        return Vec::new();
    };
    let Some(outputs) = reply.pointer("/Ok/Outputs").and_then(|o| o.as_object()) else {
        return Vec::new();
    };
    let mut rows: Vec<RowView> = outputs
        .iter()
        .map(|(name, output)| {
            let text = |key: &str| output.get(key).and_then(|v| v.as_str()).unwrap_or("");
            let make = format!("{} {}", text("make"), text("model"))
                .trim()
                .to_string();
            let mode = output
                .get("current_mode")
                .and_then(|i| i.as_u64())
                .and_then(|i| output.pointer(&format!("/modes/{i}")))
                .map(|m| {
                    let w = m.get("width").and_then(|v| v.as_u64()).unwrap_or(0);
                    let h = m.get("height").and_then(|v| v.as_u64()).unwrap_or(0);
                    let hz =
                        m.get("refresh_rate").and_then(|v| v.as_u64()).unwrap_or(0) as f64 / 1000.0;
                    format!("{w} × {h} at {hz:.0} Hz")
                })
                .unwrap_or_default();
            let scale = output
                .pointer("/logical/scale")
                .and_then(|v| v.as_f64())
                .map(|s| format!(", scale {s}"))
                .unwrap_or_default();
            RowView {
                id: name.chars().take(40).collect(),
                title: helix_base::display::sanitize_for_display(name, 40),
                group: "Connected".into(),
                description: helix_base::display::sanitize_for_display(&make, 80),
                overridden: false,
                control: helix_config::catalog::Control::Text {
                    value: helix_base::display::sanitize_for_display(&format!("{mode}{scale}"), 80),
                },
            }
        })
        .collect();
    rows.sort_by(|a, b| a.id.cmp(&b.id));
    rows
}

fn catalog_bool(state: &State, id: &str) -> bool {
    state
        .catalog
        .entries()
        .iter()
        .find(|entry| entry.get("id").and_then(|value| value.as_str()) == Some(id))
        .and_then(|entry| entry.get("value").and_then(|value| value.as_bool()))
        .unwrap_or(false)
}

fn catalog_text(state: &State, id: &str) -> String {
    state
        .catalog
        .entries()
        .iter()
        .find(|entry| entry.get("id").and_then(|value| value.as_str()) == Some(id))
        .and_then(|entry| entry.get("value").and_then(|value| value.as_str()))
        .unwrap_or("")
        .to_string()
}

fn apply_bin() -> Result<std::path::PathBuf, String> {
    if let Some(path) = std::env::var_os("HELIX_GREETER_APPLY_BIN") {
        return Ok(std::path::PathBuf::from(path));
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            let sibling = dir.join("helix-greeter-apply-appearance");
            if sibling.is_file() {
                return Ok(sibling);
            }
        }
    }
    Err("helix-greeter-apply-appearance is not installed.".into())
}

fn greeter_dest() -> std::path::PathBuf {
    std::env::var_os("HELIX_GREETER_STATE")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| std::path::PathBuf::from("/var/lib/helix-greeter"))
}

fn staging_dir() -> Result<std::path::PathBuf, String> {
    let base = std::env::var_os("XDG_RUNTIME_DIR")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(std::env::temp_dir);
    let dir = base.join(format!("helix-greeter-sync-{}", std::process::id()));
    if dir.exists() {
        std::fs::remove_dir_all(&dir).map_err(|error| error.to_string())?;
    }
    std::fs::create_dir_all(&dir).map_err(|error| error.to_string())?;
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o700))
        .map_err(|error| error.to_string())?;
    Ok(dir)
}

struct ApplyExit {
    code: i32,
    message: String,
}

fn command_output(cmd: &std::ffi::OsStr, args: &[std::ffi::OsString]) -> Result<ApplyExit, String> {
    let output = std::process::Command::new(cmd)
        .args(args)
        .output()
        .map_err(|error| format!("could not run {}: {error}", cmd.to_string_lossy()))?;
    if output.status.success() {
        return Ok(ApplyExit {
            code: 0,
            message: String::new(),
        });
    }
    let message = String::from_utf8_lossy(&output.stderr);
    let message = message.trim();
    let message = if message.is_empty() {
        format!("greeter sync failed ({})", output.status)
    } else {
        message.to_string()
    };
    Ok(ApplyExit {
        code: output.status.code().unwrap_or(1),
        message,
    })
}

fn privilege_argv(
    command: &str,
    bin: &std::path::Path,
    staging: &std::path::Path,
    dest: &std::path::Path,
) -> Result<Vec<std::ffi::OsString>, String> {
    let mut args: Vec<std::ffi::OsString> = if command.trim().is_empty() {
        vec!["pkexec".into()]
    } else {
        command
            .split_whitespace()
            .map(std::ffi::OsString::from)
            .collect()
    };
    if args.is_empty() || args.len() > 8 {
        return Err("Greeter sync privilege command is too long.".into());
    }
    args.push(bin.as_os_str().to_os_string());
    args.push("--sync".into());
    args.push(staging.as_os_str().to_os_string());
    args.push("--dest".into());
    args.push(dest.as_os_str().to_os_string());
    Ok(args)
}

/// One sync at a time: the staging directory is shared, and a second password prompt on top of the first helps nobody.
static GREETER_SYNC: std::sync::Mutex<()> = std::sync::Mutex::new(());

/// Copy the current theme and wallpaper into the greeter state directory.
fn greeter_sync_blocking(app: &AppHandle) -> Result<(), String> {
    let _one_at_a_time = GREETER_SYNC.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    let bin = apply_bin()?;
    let staging = staging_dir()?;
    let result = greeter_sync_from(app, &bin, &staging);
    let _ = std::fs::remove_dir_all(&staging);
    result
}

fn greeter_sync_from(app: &AppHandle, bin: &std::path::Path, staging: &std::path::Path) -> Result<(), String> {
    let staged = command_output(
        bin.as_os_str(),
        &["--stage".into(), staging.as_os_str().to_os_string()],
    )?;
    if staged.code != 0 {
        return Err(staged.message);
    }
    let dest = greeter_dest();
    let direct = command_output(
        bin.as_os_str(),
        &[
            "--sync".into(),
            staging.as_os_str().to_os_string(),
            "--dest".into(),
            dest.as_os_str().to_os_string(),
        ],
    )?;
    // Exit 2 means the state directory is not writable. Anything else is a bad payload, and a nested
    // test must not ask for a password.
    if direct.code == 0 {
        return Ok(());
    }
    if direct.code != 2 || helix_base::nested_test_session() {
        return Err(direct.message);
    }
    let privilege = {
        let shared = shared(app);
        let state = shared.lock().expect("state");
        catalog_text(&state, "shell.greeter_sync.privilege_command")
    };
    let argv = privilege_argv(&privilege, bin, staging, &dest)?;
    let (cmd, rest) = argv.split_first().ok_or("empty privilege command")?;
    let installed = command_output(cmd, rest)?;
    if installed.code == 0 {
        Ok(())
    } else {
        Err(installed.message)
    }
}

fn greeter_auto_sync(app: &AppHandle, id: &str) {
    if id == "shell.greeter_sync.privilege_command" {
        return;
    }
    let enabled = {
        let shared = shared(app);
        let state = shared.lock().expect("state");
        catalog_bool(&state, "shell.greeter_sync.auto_sync")
    };
    if !enabled {
        return;
    }
    // The sync can wait on an administrator password; keep it off the command that changed the setting.
    let app = app.clone();
    std::thread::spawn(move || {
        if let Err(error) = greeter_sync_blocking(&app) {
            eprintln!("SETTINGS greeter sync: {error}");
        }
    });
}

#[tauri::command]
async fn greeter_sync(app: AppHandle) -> Result<(), String> {
    tokio::task::spawn_blocking(move || greeter_sync_blocking(&app))
        .await
        .map_err(|error| error.to_string())?
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
    if let Err(error) = desktop.update_setting(&update.id, &update.value).await {
        eprintln!("SETTINGS set {} refused: {error}", update.id);
        return Err(error.to_string());
    }
    eprintln!("SETTINGS set {} {}", update.id, update.value);
    refresh_settings(&app).await;
    if page == "appearance" {
        greeter_auto_sync(&app, &id);
    }
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

/// Optional `--page <name>` from the command line (settings-open / desktop deep-link).
fn startup_page() -> Option<String> {
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        if arg == "--page" {
            return args.next().filter(|p| {
                p.len() <= 40
                    && p.bytes()
                        .all(|b| b.is_ascii_lowercase() || b == b'-' || b == b'_')
            });
        }
        if let Some(page) = arg.strip_prefix("--page=") {
            let page = page.to_string();
            if page.len() <= 40
                && page
                    .bytes()
                    .all(|b| b.is_ascii_lowercase() || b == b'-' || b == b'_')
            {
                return Some(page);
            }
        }
    }
    None
}

fn main() {
    // The web view's JIT costs memory this app does not need (same choice as the shell).
    // The window announces this application id, which is what the desktop file org.helix.Settings.desktop (name and icon) is matched by.
    gtk::glib::set_prgname(Some("org.helix.Settings"));
    if std::env::var_os("JSC_useJIT").is_none() {
        std::env::set_var("JSC_useJIT", "false");
    }
    let open_page = startup_page();
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            agents_view,
            agents_set_tier,
            agents_revoke,
            agents_refresh,
            open_mobius,
            compositor_view,
            compositor_set_agent_desk_name,
            compositor_set_placement,
            compositor_focus_agent_desk,
            compositor_open_config,
            inference_runtime_view,
            inference_install,
            inference_restart_service,
            rows_view,
            ui_error,
            approvals_view,
            approvals_set,
            wallpaper_view,
            wallpaper_thumb,
            wallpaper_choose_image,
            wallpaper_choose_swatch,
            dock_rows,
            power_rows,
            power_change,
            displays_rows,
            dock_change,
            wallhaven::wallhaven_search,
            wallhaven::wallhaven_thumb,
            wallhaven::wallhaven_use,
            shortcuts_view,
            keybinds_view,
            keybinds_set,
            about,
            network_view,
            network_set_wifi,
            network_scan,
            network_connect,
            network_connect_password,
            network_connect_enterprise,
            network_disconnect,
            network_forget,
            network_vpn,
            audio_view,
            audio_set_volume,
            audio_set_muted,
            audio_set_default,
            audio_set_input_volume,
            audio_set_input_muted,
            audio_set_default_input,
            bluetooth_view,
            bluetooth_set_power,
            bluetooth_scan,
            bluetooth_connect,
            bluetooth_disconnect,
            bluetooth_forget,
            bluetooth_pair,
            bluetooth_pair_answer,
            bluetooth_pair_accept,
            bluetooth_pair_cancel,
            pairing_view,
            setting_change,
            greeter_sync,
            layout_reporting,
            report_layout
        ])
        .setup(move |app| {
            app.manage::<Shared>(Arc::new(Mutex::new(State {
                grants: Grants::default(),
                catalog: Catalog::default(),
                picker: Picker::default(),
                pairing: Pairing::default(),
                network: Network::from_snapshot(&serde_json::Value::Null),
                bluetooth: Bluetooth::from_snapshot(&serde_json::Value::Null),
                audio: Audio::from_snapshot(&serde_json::Value::Null),
                status: "unavailable",
            })));
            let url = match &open_page {
                Some(page) => format!("index.html?page={page}"),
                None => "index.html".into(),
            };
            WebviewWindowBuilder::new(app, WINDOW, WebviewUrl::App(url.into()))
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
