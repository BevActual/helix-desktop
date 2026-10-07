//! The Helix shell: one layer surface hosting the bar and every panel in a single page. The assistant is a separate process
//! (`helix-assistant`); this binary never calls Agent1 and never talks to the model. What the surface looks like in each state
//! is decided by helix_shell_core::ShellSurface; layer.rs only applies it.

mod apps;
mod layer;

use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use helix_desktop_api::{sound_volume_label, Desktop, Domain};
use helix_shell_core::audio::{Audio, AudioView, Request, RequestError};
use helix_shell_core::network::{
    Network, NetworkView, Request as NetworkRequest, RequestError as NetworkError,
};
use helix_shell_core::{Confirmation, Escape, Phase, Session, ShellSurface, Spawn, ARMING_MS};
use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager, WebviewUrl, WebviewWindowBuilder};

struct ShellState {
    session: Session,
    confirmation: Confirmation,
    surface: ShellSurface,
    /// The applications the launcher lists, read each time it opens. The page refers to them by id only.
    apps: Vec<helix_shell_core::launcher::App>,
    history: helix_shell_core::launcher::History,
    /// The last Audio1 snapshot, read after every Changed signal. Writes are built from this and nothing else.
    audio: Audio,
    /// The last Network1 snapshot; Wi-Fi writes are built from it and nothing else.
    network: Network,
    /// Icon data URIs by app id (empty when the app has no usable icon), filled as results are shown.
    icons: std::collections::HashMap<String, String>,
    started: Instant,
    last_phase: Phase,
}

#[derive(Clone, Serialize)]
struct AssistantView {
    phase: &'static str,
    text: String,
    description: String,
    reason: String,
    failed: bool,
    unverified: bool,
    armed: bool,
}

fn now_ms(started: Instant) -> u64 {
    u64::try_from(started.elapsed().as_millis()).unwrap_or(u64::MAX)
}

fn phase_name(phase: Phase) -> &'static str {
    match phase {
        Phase::Idle => "idle",
        Phase::Thinking => "thinking",
        Phase::Confirm => "confirm",
        Phase::Acting => "acting",
        Phase::Result => "result",
    }
}

fn assistant_view(shell: &ShellState) -> AssistantView {
    let view = shell.session.view();
    AssistantView {
        phase: phase_name(view.phase),
        text: view.text.clone(),
        description: view.description.clone(),
        reason: view.reason.clone(),
        failed: view.failed,
        unverified: view.unverified,
        armed: view.phase == Phase::Confirm && shell.confirmation.armed(now_ms(shell.started)),
    }
}

/// Returns whether this call is the moment a confirmation appeared, so Do it can arm 700 ms later.
fn note_phase(shell: &mut ShellState) -> bool {
    let phase = shell.session.view().phase;
    let entered = phase == Phase::Confirm && shell.last_phase != Phase::Confirm;
    if entered {
        shell.confirmation.show(now_ms(shell.started));
    }
    shell.last_phase = phase;
    entered
}

fn shell_state(app: &AppHandle) -> Arc<Mutex<ShellState>> {
    app.state::<Arc<Mutex<ShellState>>>().inner().clone()
}

fn emit_view(app: &AppHandle, view: &AssistantView) {
    eprintln!("ASSISTANT {}", view.phase);
    let _ = app.emit("assistant-view", view);
}

fn schedule_arm(app: AppHandle) {
    std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(ARMING_MS));
        let shell = shell_state(&app);
        let view = assistant_view(&shell.lock().expect("shell state"));
        emit_view(&app, &view);
    });
}

fn launch_assistant(app: AppHandle, spawn: Spawn) {
    std::thread::spawn(move || {
        let result = std::process::Command::new("helix-assistant")
            .args(&spawn.args)
            .output();
        let state = shell_state(&app);
        let mut shell = state.lock().expect("shell state");
        match result {
            Ok(output) => {
                let code = output.status.code().unwrap_or(1);
                let text = String::from_utf8_lossy(&output.stdout).into_owned();
                shell
                    .session
                    .deliver_generation(spawn.generation, code, text);
            }
            Err(_) => shell.session.launch_failed(spawn.generation),
        }
        let arm = note_phase(&mut shell);
        let view = assistant_view(&shell);
        drop(shell);
        if arm {
            schedule_arm(app.clone());
        }
        emit_view(&app, &view);
    });
}

#[derive(Clone, Serialize)]
struct ShellMode {
    panel: Option<String>,
}

fn emit_mode(app: &AppHandle, shell: &ShellState) {
    let mode = ShellMode {
        panel: shell.surface.open_panel_id().map(str::to_string),
    };
    let _ = app.emit("shell-mode", mode);
}

/// A panel always opens and closes fresh: nothing from an earlier visit can still be waiting for an answer.
fn reset_assistant(shell: &mut ShellState) {
    shell.session.cancel();
    shell.confirmation = Confirmation::new();
    shell.last_phase = Phase::Idle;
}

fn open_panel(app: &AppHandle, id: &str) {
    let state = shell_state(app);
    let mut shell = state.lock().expect("shell state");
    let plan = match shell.surface.open_panel(id) {
        Ok(plan) => plan,
        Err(error) => {
            eprintln!("open panel {id:?}: {error:?}");
            return;
        }
    };
    let Some(plan) = plan else { return };
    reset_assistant(&mut shell);
    if id == "launcher" {
        shell.apps = apps::scan();
        shell.icons.clear();
        eprintln!("LAUNCHER apps {}", shell.apps.len());
    }
    if id == "control-center" {
        let app = app.clone();
        tauri::async_runtime::spawn(async move {
            refresh_audio(&app).await;
            refresh_network(&app).await;
        });
    }
    let view = assistant_view(&shell);
    emit_mode(app, &shell);
    drop(shell);
    layer::apply(app, plan);
    eprintln!("PANEL open");
    emit_view(app, &view);
}

fn close_panel(app: &AppHandle) {
    let state = shell_state(app);
    let mut shell = state.lock().expect("shell state");
    let Some(plan) = shell.surface.close() else {
        return;
    };
    reset_assistant(&mut shell);
    let view = assistant_view(&shell);
    emit_mode(app, &shell);
    drop(shell);
    layer::apply(app, plan);
    eprintln!("PANEL closed");
    emit_view(app, &view);
}

/// Escape backs out of a request or a confirmation first; only when there is nothing to back out of does the panel close.
fn dismiss_escape(app: &AppHandle) {
    let state = shell_state(app);
    let mut shell = state.lock().expect("shell state");
    // Only the assistant has a request to back out of; Escape in the launcher just closes it.
    let active = shell.surface.open_panel_id() == Some("assistant")
        && shell.session.view().phase != Phase::Idle;
    match shell.surface.escape(active) {
        Escape::CancelRequest => {
            shell.confirmation.escape();
            shell.session.cancel();
            shell.last_phase = Phase::Idle;
            let view = assistant_view(&shell);
            drop(shell);
            emit_view(app, &view);
        }
        Escape::Close => {
            drop(shell);
            close_panel(app);
        }
        Escape::Nothing => {}
    }
}

fn dismiss_outside(app: &AppHandle) {
    close_panel(app);
}

fn control(app: &AppHandle, command: &str) {
    match command {
        "open" => open_panel(app, "assistant"),
        "launcher" => open_panel(app, "launcher"),
        "control-center" => open_panel(app, "control-center"),
        "close" => close_panel(app),
        "escape" => dismiss_escape(app),
        "outside" => dismiss_outside(app),
        _ => eprintln!("ignored surface command {command}"),
    }
}

fn watch_control(app: AppHandle, path: std::path::PathBuf) {
    std::thread::spawn(move || {
        let mut seen = 0usize;
        loop {
            std::thread::sleep(Duration::from_millis(200));
            let Ok(text) = std::fs::read_to_string(&path) else {
                continue;
            };
            let lines: Vec<String> = text
                .lines()
                .skip(seen)
                .map(str::trim)
                .map(str::to_string)
                .filter(|line| !line.is_empty())
                .collect();
            seen = text.lines().count();
            for line in lines {
                let command = line;
                let runner = app.clone();
                let _ = app.run_on_main_thread(move || control(&runner, &command));
            }
        }
    });
}

static DESKTOP: tokio::sync::OnceCell<Arc<Desktop>> = tokio::sync::OnceCell::const_new();

/// One connection to the session bus, shared by every call.
async fn desktop() -> Option<Arc<Desktop>> {
    DESKTOP
        .get_or_try_init(|| async { Desktop::session().await.map(Arc::new) })
        .await
        .ok()
        .cloned()
}

/// Reads the audio snapshot, keeps it, and tells the page. A service that is down or not answering is "no audio".
async fn refresh_audio(app: &AppHandle) {
    let snapshot = match desktop().await {
        Some(desktop) => desktop
            .audio_snapshot()
            .await
            .unwrap_or(serde_json::Value::Null),
        None => serde_json::Value::Null,
    };
    let audio = Audio::from_snapshot(&snapshot);
    let view = audio.view();
    shell_state(app).lock().expect("shell state").audio = audio;
    let _ = app.emit("audio-view", view);
}

/// Follows Audio1.Changed (no polling). Reads are coalesced: a volume drag is a burst of signals and one read.
fn watch_audio(app: AppHandle) {
    tauri::async_runtime::spawn(async move {
        let pending = Arc::new(std::sync::atomic::AtomicBool::new(false));
        loop {
            if let Some(desktop) = desktop().await {
                refresh_audio(&app).await;
                let (watcher_app, flag) = (app.clone(), pending.clone());
                let _ = desktop
                    .watch_audio(move || {
                        if flag.swap(true, std::sync::atomic::Ordering::SeqCst) {
                            return;
                        }
                        let (app, flag) = (watcher_app.clone(), flag.clone());
                        tauri::async_runtime::spawn(async move {
                            tokio::time::sleep(Duration::from_millis(40)).await;
                            flag.store(false, std::sync::atomic::Ordering::SeqCst);
                            refresh_audio(&app).await;
                        });
                    })
                    .await;
            }
            // The service went away or has not started yet; try again shortly.
            tokio::time::sleep(Duration::from_secs(2)).await;
        }
    });
}

async fn send_audio(request: Result<Request, RequestError>) -> Result<(), String> {
    let request = request.map_err(|error| format!("{error:?}"))?;
    let desktop = desktop()
        .await
        .ok_or("the desktop service is not running")?;
    desktop
        .audio_action(request.action, &request.args)
        .await
        .map_err(|error| error.to_string())?;
    eprintln!("AUDIO {} sent", request.action);
    Ok(())
}

/// Reads the network snapshot, keeps it, and tells the page. A service that is down is "no network".
async fn refresh_network(app: &AppHandle) {
    let snapshot = match desktop().await {
        Some(desktop) => desktop
            .snapshot(Domain::Network)
            .await
            .unwrap_or(serde_json::Value::Null),
        None => serde_json::Value::Null,
    };
    let network = Network::from_snapshot(&snapshot);
    let view = network.view();
    shell_state(app).lock().expect("shell state").network = network;
    let _ = app.emit("network-view", view);
}

/// Follows Network1.Changed (no polling); reads are coalesced like the audio ones.
fn watch_network(app: AppHandle) {
    tauri::async_runtime::spawn(async move {
        let pending = Arc::new(std::sync::atomic::AtomicBool::new(false));
        loop {
            if let Some(desktop) = desktop().await {
                refresh_network(&app).await;
                let (watcher_app, flag) = (app.clone(), pending.clone());
                let _ = desktop
                    .watch(Domain::Network, move || {
                        if flag.swap(true, std::sync::atomic::Ordering::SeqCst) {
                            return;
                        }
                        let (app, flag) = (watcher_app.clone(), flag.clone());
                        tauri::async_runtime::spawn(async move {
                            tokio::time::sleep(Duration::from_millis(100)).await;
                            flag.store(false, std::sync::atomic::Ordering::SeqCst);
                            refresh_network(&app).await;
                        });
                    })
                    .await;
            }
            tokio::time::sleep(Duration::from_secs(2)).await;
        }
    });
}

async fn send_network(request: Result<NetworkRequest, NetworkError>) -> Result<(), String> {
    let request = request.map_err(|error| format!("{error:?}"))?;
    let desktop = desktop()
        .await
        .ok_or("the desktop service is not running")?;
    desktop
        .action(Domain::Network, request.action, &request.args)
        .await
        .map_err(|error| error.to_string())?;
    eprintln!("NETWORK {} sent", request.action);
    Ok(())
}

fn network_request(
    app: &AppHandle,
    build: impl FnOnce(&Network) -> Result<NetworkRequest, NetworkError>,
) -> Result<NetworkRequest, NetworkError> {
    let state = shell_state(app);
    let shell = state.lock().expect("shell state");
    build(&shell.network)
}

#[tauri::command]
fn network_view(app: AppHandle) -> NetworkView {
    shell_state(&app)
        .lock()
        .expect("shell state")
        .network
        .view()
}

#[tauri::command]
async fn network_set_wifi(app: AppHandle, enabled: bool) -> Result<(), String> {
    send_network(network_request(&app, |n| n.set_wifi(enabled))).await
}

#[tauri::command]
async fn network_scan(app: AppHandle) -> Result<(), String> {
    send_network(network_request(&app, Network::scan)).await
}

/// Joins a network by the path it was listed with. A secured network that has never been joined is refused here, not sent.
#[tauri::command]
async fn network_connect(app: AppHandle, path: String) -> Result<(), String> {
    send_network(network_request(&app, |n| n.connect(&path))).await
}

#[tauri::command]
async fn network_disconnect(app: AppHandle) -> Result<(), String> {
    send_network(network_request(&app, Network::disconnect)).await
}

#[tauri::command]
async fn network_forget(app: AppHandle, path: String) -> Result<(), String> {
    send_network(network_request(&app, |n| n.forget(&path))).await
}

#[tauri::command]
async fn network_vpn(app: AppHandle, path: String, active: bool) -> Result<(), String> {
    send_network(network_request(&app, |n| n.set_vpn(&path, active))).await
}

fn audio_request(
    app: &AppHandle,
    build: impl FnOnce(&Audio) -> Result<Request, RequestError>,
) -> Result<Request, RequestError> {
    let state = shell_state(app);
    let shell = state.lock().expect("shell state");
    build(&shell.audio)
}

#[tauri::command]
fn audio_view(app: AppHandle) -> AudioView {
    shell_state(&app).lock().expect("shell state").audio.view()
}

/// Sets the volume of the current output, 0 to 100. The page never names a device.
#[tauri::command]
async fn audio_set_volume(app: AppHandle, percent: f64) -> Result<(), String> {
    send_audio(audio_request(&app, |audio| audio.set_volume(percent))).await
}

#[tauri::command]
async fn audio_set_mute(app: AppHandle, muted: bool) -> Result<(), String> {
    send_audio(audio_request(&app, |audio| audio.set_muted(muted))).await
}

/// Makes one of the listed outputs the default. An id that is not in the last snapshot is refused here.
#[tauri::command]
async fn audio_set_default(app: AppHandle, id: u32) -> Result<(), String> {
    send_audio(audio_request(&app, |audio| audio.set_default(id))).await
}

fn watch_volume(app: AppHandle) {
    tauri::async_runtime::spawn(async move {
        for _ in 0..50 {
            if let Ok(desktop) = Desktop::session().await {
                if desktop.sound_volume().await.is_ok() {
                    let app = app.clone();
                    let _ = desktop
                        .watch_sound_volume(move |volume| {
                            let label = sound_volume_label(volume);
                            eprintln!("LIVE {label}");
                            let _ = app.emit("live-value", label);
                        })
                        .await;
                    return;
                }
            }
            tokio::time::sleep(Duration::from_millis(200)).await;
        }
        eprintln!("LIVE Sound unavailable");
        let _ = app.emit("live-value", "Sound unavailable");
    });
}

#[tauri::command]
fn ready(window: tauri::Window) {
    let label = window.label().to_string();
    let state = shell_state(window.app_handle());
    let shell = state.lock().expect("shell state");
    eprintln!(
        "READY {} after {} ms",
        label,
        shell.started.elapsed().as_millis()
    );
}

#[tauri::command]
fn panel_open(app: AppHandle, id: String) {
    open_panel(&app, &id);
}

#[tauri::command]
fn panel_outside(app: AppHandle) {
    dismiss_outside(&app);
}

#[tauri::command]
fn assistant_submit(app: AppHandle, request: String) -> AssistantView {
    let state = shell_state(&app);
    let mut shell = state.lock().expect("shell state");
    shell.session.submit(&request);
    let arm = note_phase(&mut shell);
    let spawns = shell.session.take_spawns();
    let view = assistant_view(&shell);
    drop(shell);
    for spawn in spawns {
        launch_assistant(app.clone(), spawn);
    }
    if arm {
        schedule_arm(app.clone());
    }
    emit_view(&app, &view);
    view
}

#[tauri::command]
fn assistant_confirm(app: AppHandle) -> AssistantView {
    let state = shell_state(&app);
    let mut shell = state.lock().expect("shell state");
    let now = now_ms(shell.started);
    // Enter is not a command. Do it is ignored until the confirmation has been armed.
    if !shell.confirmation.activate(now) {
        return assistant_view(&shell);
    }
    shell.session.confirm();
    let arm = note_phase(&mut shell);
    let spawns = shell.session.take_spawns();
    let view = assistant_view(&shell);
    drop(shell);
    for spawn in spawns {
        launch_assistant(app.clone(), spawn);
    }
    if arm {
        schedule_arm(app.clone());
    }
    emit_view(&app, &view);
    view
}

#[tauri::command]
fn assistant_cancel(app: AppHandle) -> AssistantView {
    let state = shell_state(&app);
    let mut shell = state.lock().expect("shell state");
    shell.confirmation.escape();
    shell.session.cancel();
    shell.last_phase = Phase::Idle;
    let view = assistant_view(&shell);
    drop(shell);
    emit_view(&app, &view);
    view
}

#[tauri::command]
fn panel_escape(app: AppHandle) {
    dismiss_escape(&app);
}

#[derive(Clone, Serialize)]
struct AppHit {
    id: String,
    name: String,
    generic: String,
    /// A `data:` image URI, or empty.
    icon: String,
}

const LAUNCHER_ROWS: usize = 8;
const MAX_QUERY_CHARS: usize = 100;

/// The best matches for what was typed. Names are already display-safe; the page shows them as text.
#[tauri::command]
fn launcher_query(app: AppHandle, query: String) -> Vec<AppHit> {
    let state = shell_state(&app);
    let mut shell = state.lock().expect("shell state");
    if shell.surface.open_panel_id() != Some("launcher") {
        return Vec::new();
    }
    let query: String = query.chars().take(MAX_QUERY_CHARS).collect();
    let found = helix_shell_core::launcher::search_with_history(
        &shell.apps,
        &query,
        LAUNCHER_ROWS,
        &shell.history,
    );
    let mut hits = Vec::with_capacity(found.len());
    for index in found {
        let entry = shell.apps[index].clone();
        let icon = shell
            .icons
            .entry(entry.id.clone())
            .or_insert_with(|| apps::icon_data_uri(&entry.icon).unwrap_or_default())
            .clone();
        hits.push(AppHit {
            id: entry.id,
            name: entry.name,
            generic: entry.generic_name,
            icon,
        });
    }
    eprintln!(
        "LAUNCHER results {} first {} icons {}",
        hits.len(),
        hits.first().map_or("-", |hit| hit.id.as_str()),
        hits.iter().filter(|hit| !hit.icon.is_empty()).count()
    );
    hits
}

/// Starts the application with this id and closes the launcher. The page can only name an application the scan found; it
/// supplies no command, arguments or paths.
#[tauri::command]
fn launcher_launch(app: AppHandle, id: String) -> bool {
    let state = shell_state(&app);
    let shell = state.lock().expect("shell state");
    if shell.surface.open_panel_id() != Some("launcher") {
        return false;
    }
    let Some(entry) = shell
        .apps
        .iter()
        .find(|candidate| candidate.id == id)
        .cloned()
    else {
        eprintln!("LAUNCH refused: unknown id");
        return false;
    };
    drop(shell);
    match apps::launch(&entry) {
        Ok(()) => {
            eprintln!("LAUNCH {}", entry.id);
            {
                let mut shell = state.lock().expect("shell state");
                shell.history.record(&entry.id);
                apps::save_history(&shell.history);
            }
            close_panel(&app);
            true
        }
        Err(error) => {
            eprintln!("LAUNCH failed {}: {error}", entry.id);
            false
        }
    }
}

fn main() {
    // The page is a few lines of script. The JIT and its compile threads cost about 7 MB here and put executable memory in a
    // process that draws text from a model, so it is off. Set before WebKit starts any helper process, which inherit it.
    if std::env::var_os("JSC_useJIT").is_none() {
        std::env::set_var("JSC_useJIT", "false");
    }
    let started = Instant::now();
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            ready,
            panel_open,
            panel_outside,
            assistant_submit,
            assistant_confirm,
            assistant_cancel,
            panel_escape,
            launcher_query,
            launcher_launch,
            audio_view,
            audio_set_volume,
            audio_set_mute,
            audio_set_default,
            network_view,
            network_set_wifi,
            network_scan,
            network_connect,
            network_disconnect,
            network_forget,
            network_vpn
        ])
        .setup(move |app| {
            let window =
                WebviewWindowBuilder::new(app, layer::WINDOW, WebviewUrl::App("index.html".into()))
                    .decorations(false)
                    .transparent(true)
                    .visible(false)
                    .inner_size(1280.0, 800.0)
                    .build()?;

            let surface = ShellSurface::new();
            layer::setup(&window, surface.plan())?;
            layer::reserve_bar_space();
            let state = Arc::new(Mutex::new(ShellState {
                session: Session::new(|_| true),
                confirmation: Confirmation::new(),
                surface,
                apps: Vec::new(),
                history: apps::load_history(),
                audio: Audio::default(),
                network: Network::default(),
                icons: std::collections::HashMap::new(),
                started,
                last_phase: Phase::Idle,
            }));
            app.manage(state);
            let handle = app.handle().clone();

            watch_volume(handle.clone());
            watch_audio(handle.clone());
            watch_network(handle.clone());
            if let Some(path) = std::env::var_os("HELIX_SURFACE_CONTROL") {
                let path = std::path::PathBuf::from(path);
                if let Some(parent) = path.parent() {
                    let _ = std::fs::create_dir_all(parent);
                }
                let _ = std::fs::write(&path, "");
                watch_control(handle.clone(), path);
            }
            if let Some(seconds) = std::env::var("HELIX_SPIKE_AUTO_PANEL")
                .ok()
                .and_then(|value| value.parse::<u64>().ok())
            {
                let handle = handle.clone();
                std::thread::spawn(move || {
                    std::thread::sleep(Duration::from_secs(seconds));
                    let opener = handle.clone();
                    let _ = handle.run_on_main_thread(move || open_panel(&opener, "assistant"));
                });
            }
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running the shell");
}
