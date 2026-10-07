//! Helix shell surfaces. The assistant is a separate process (`helix-assistant`); this binary
//! never calls Agent1 and never talks to the model. Layer surfaces are created only through
//! helix_shell_core::surfaces.

mod layer;
mod webkit_mem;

use std::collections::BTreeSet;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use helix_desktop_api::{sound_volume_label, Desktop};
use helix_shell_core::surfaces::{
    validate_setup, Edge, ExclusiveZone, KeyboardMode, Layer, SurfaceError, SurfaceManager,
    SurfaceSpec,
};
use helix_shell_core::{Confirmation, Phase, Session, Spawn, ARMING_MS};
use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager, WebviewUrl, WebviewWindowBuilder};

struct ShellState {
    session: Session,
    confirmation: Confirmation,
    surfaces: SurfaceManager,
    started: Instant,
    last_phase: Phase,
    /// WebKitWebProcess PIDs while only the bar exists. Panel helpers outside this set are reaped.
    bar_webkit: BTreeSet<u32>,
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

fn bar_spec() -> SurfaceSpec {
    SurfaceSpec {
        id: "bar".to_string(),
        monitor: String::new(),
        width: -1,
        height: 42,
        layer: Layer::Top,
        anchors: vec![Edge::Top, Edge::Left, Edge::Right],
        keyboard: KeyboardMode::None,
        exclusive: ExclusiveZone::Auto,
        namespace: "helix-tauri-bar".to_string(),
        panel: false,
    }
}

fn panel_spec() -> SurfaceSpec {
    SurfaceSpec {
        id: "panel".to_string(),
        monitor: String::new(),
        width: 520,
        height: 260,
        layer: Layer::Overlay,
        anchors: Vec::new(),
        keyboard: KeyboardMode::Exclusive,
        exclusive: ExclusiveZone::None,
        namespace: "helix-tauri-panel".to_string(),
        panel: true,
    }
}

fn create_panel_window(app: &AppHandle) -> tauri::Result<()> {
    if app.get_webview_window("panel").is_some() {
        return Ok(());
    }
    let panel = WebviewWindowBuilder::new(app, "panel", WebviewUrl::App("panel.html".into()))
        .decorations(false)
        .transparent(true)
        .visible(false)
        .inner_size(520.0, 260.0)
        .build()?;
    if let Ok(gtk_panel) = panel.gtk_window() {
        webkit_mem::limit_web_processes(&gtk_panel);
        let handle = app.clone();
        layer::when_deactivated(&gtk_panel, move || dismiss_outside(&handle));
    }
    Ok(())
}

fn tear_down_panel(app: &AppHandle) {
    let state = shell_state(app);
    let mut shell = state.lock().expect("shell state");
    shell.session.cancel();
    shell.confirmation = Confirmation::new();
    shell.last_phase = Phase::Idle;
    let keep = shell.bar_webkit.clone();
    let steps = shell.surfaces.destroy_panel("panel").unwrap_or_default();
    let view = assistant_view(&shell);
    drop(shell);
    layer::apply(app, &steps);
    // destroy() queues on the event loop; log whether Tauri still tracks the label after the call.
    if app.get_webview_window("panel").is_some() {
        eprintln!("WEBVIEW panel still-present");
    } else {
        eprintln!("WEBVIEW panel absent");
    }
    if !keep.is_empty() {
        webkit_mem::schedule_reap_extra_webkits(keep);
    }
    emit_view(app, &view);
}

fn open_panel(app: &AppHandle) {
    let state = shell_state(app);
    let mut shell = state.lock().expect("shell state");
    shell.session.cancel();
    shell.confirmation = Confirmation::new();
    shell.last_phase = Phase::Idle;
    // Create the panel WebKit window only while it is open. A hidden second webview still costs
    // tens of MB and already put the bar-only shell near the whole-shell budget.
    let mut steps = Vec::new();
    match shell.surfaces.open_panel("panel") {
        Ok(show) => steps.extend(show),
        Err(SurfaceError::Unknown) => {
            steps.extend(shell.surfaces.add(panel_spec()).expect("panel"));
            validate_setup(&steps).expect("panel layer setup");
            steps.extend(shell.surfaces.open_panel("panel").expect("open panel"));
        }
        Err(error) => {
            eprintln!("open panel: {error:?}");
            return;
        }
    }
    drop(shell);
    if let Err(error) = create_panel_window(app) {
        eprintln!("panel window: {error}");
        return;
    }
    layer::apply(app, &steps);
    if let Some(panel) = app.get_webview_window("panel") {
        let _ = panel.set_focus();
    }
}

fn close_panel(app: &AppHandle) {
    tear_down_panel(app);
}

fn dismiss_escape(app: &AppHandle) {
    let state = shell_state(app);
    let mut shell = state.lock().expect("shell state");
    if shell.session.view().phase != Phase::Idle {
        shell.confirmation.escape();
        shell.session.cancel();
        shell.last_phase = Phase::Idle;
        let view = assistant_view(&shell);
        drop(shell);
        emit_view(app, &view);
        return;
    }
    drop(shell);
    tear_down_panel(app);
}

fn dismiss_outside(app: &AppHandle) {
    let state = shell_state(app);
    let shell = state.lock().expect("shell state");
    if !shell.surfaces.is_visible("panel") {
        return;
    }
    drop(shell);
    tear_down_panel(app);
}

fn control(app: &AppHandle, command: &str) {
    match command {
        "open" => open_panel(app),
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
    let mut shell = state.lock().expect("shell state");
    eprintln!(
        "READY {} after {} ms",
        label,
        shell.started.elapsed().as_millis()
    );
    if label == "bar" && shell.bar_webkit.is_empty() {
        shell.bar_webkit = webkit_mem::snapshot_webkit_web_pids();
        eprintln!("WEBKIT bar-baseline {:?}", shell.bar_webkit);
    }
}

#[tauri::command]
fn panel_open(app: AppHandle) {
    open_panel(&app);
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
fn assistant_escape(app: AppHandle) {
    dismiss_escape(&app);
}

fn main() {
    let started = Instant::now();
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            ready,
            panel_open,
            panel_outside,
            assistant_submit,
            assistant_confirm,
            assistant_cancel,
            assistant_escape
        ])
        .setup(move |app| {
            let bar = WebviewWindowBuilder::new(app, "bar", WebviewUrl::App("bar.html".into()))
                .decorations(false)
                .transparent(true)
                .visible(false)
                .inner_size(1280.0, 34.0)
                .build()?;
            if let Ok(gtk_bar) = bar.gtk_window() {
                webkit_mem::limit_web_processes(&gtk_bar);
            }

            let mut surfaces = SurfaceManager::new();
            let bar_steps = surfaces.add(bar_spec()).expect("bar");
            validate_setup(&bar_steps).expect("bar layer setup");
            let show_bar = surfaces.show_bar("bar").expect("show bar");

            let state = Arc::new(Mutex::new(ShellState {
                session: Session::new(|_| true),
                confirmation: Confirmation::new(),
                surfaces,
                started,
                last_phase: Phase::Idle,
                bar_webkit: BTreeSet::new(),
            }));
            app.manage(state);
            let handle = app.handle().clone();
            layer::apply(&handle, &bar_steps);
            layer::apply(&handle, &show_bar);
            let _ = bar;

            watch_volume(handle.clone());
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
                    let _ = handle.run_on_main_thread(move || open_panel(&opener));
                });
            }
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running the shell");
}
