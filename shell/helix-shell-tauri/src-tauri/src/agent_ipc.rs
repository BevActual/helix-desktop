//! The shell's command socket, in the wire format the desktop service already speaks to a shell (`helix-<display>.sock`, one line in,
//! one reply out). It is what makes an agent's approved action happen under this shell: the service checks the client, tier, grant and
//! confirmation (Agent Surface) and then sends the command here. Only the commands in `helix_agent::agent_msg::SUPPORTED` exist; the line is
//! parsed by that pure model and each command runs through the same code as the sliders, launcher and key bindings, so every range and
//! refusal that applies there applies here. Free-text payloads (`clipboard-copy`, `app-launch`) are never written to the log. The socket
//! is private to the user (it lives in `XDG_RUNTIME_DIR`, mode 0600), and like `org.helix.Shell1` it is not an authority boundary against
//! other processes of the same user: Agent Surface is what gates agents.

use std::path::PathBuf;
use std::time::Duration;

use helix_agent::agent_msg::{parse, Command, Direction, Theme, MAX_LINE, SUPPORTED};
use helix_agent::agent_status::{status_json, StatusInput};
use helix_agent::assistant::Phase;
use helix_agent::change_ledger::{
    sentence, volume_machine, volume_text, ChangeLedger, ChangeRecord, SettingKey,
};
use tauri::AppHandle;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{UnixListener, UnixStream};

use crate::control::{current_panel, on_main};
use crate::{close_panel, open_panel, shell_state};

/// `XDG_RUNTIME_DIR`, or the folder a test points `HELIX_AGENT_IPC_DIR` at so a nested session never touches the real one.
fn runtime_dir() -> Option<PathBuf> {
    std::env::var_os("HELIX_AGENT_IPC_DIR")
        .or_else(|| std::env::var_os("XDG_RUNTIME_DIR"))
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
}

/// `helix-<WAYLAND_DISPLAY>.sock`: the name the service looks for first.
fn socket_path() -> Option<PathBuf> {
    let display = std::env::var("WAYLAND_DISPLAY")
        .ok()
        .filter(|d| !d.is_empty() && !d.contains('/'))?;
    Some(runtime_dir()?.join(format!("helix-{display}.sock")))
}

/// Where the shell says which commands it carries out, so the service lists only those (first line: this shell's pid, so a stale file from
/// a shell that has gone is ignored).
fn tools_path() -> Option<PathBuf> {
    let display = std::env::var("WAYLAND_DISPLAY")
        .ok()
        .filter(|d| !d.is_empty() && !d.contains('/'))?;
    Some(runtime_dir()?.join(format!("helix-shell-tools-{display}")))
}

async fn execute(app: &AppHandle, command: Command) -> Result<String, String> {
    let ok = || Ok("ok\n".to_string());
    match command {
        Command::VolumeStep(delta) => {
            let before = audio_level(app);
            crate::adjust_volume(app, delta).await;
            crate::refresh_audio(app).await;
            noted_volume(app, before, audio_level(app))
        }
        Command::VolumeToggleMute => {
            let before = audio_level(app);
            crate::toggle_mute(app).await;
            crate::refresh_audio(app).await;
            noted_volume(app, before, audio_level(app))
        }
        Command::VolumeSet(percent) => {
            let before = audio_level(app);
            crate::audio_set_volume(app.clone(), percent).await?;
            crate::refresh_audio(app).await;
            noted_volume(app, before, audio_level(app))
        }
        Command::BrightnessStep(delta) => {
            let before = brightness_level(app);
            crate::adjust_brightness(app, delta).await;
            noted_level(
                app,
                SettingKey::Brightness,
                "Brightness",
                before,
                brightness_level(app),
            )
        }
        Command::BrightnessSet(percent) => {
            let before = brightness_level(app);
            crate::brightness_set(app.clone(), percent).await?;
            noted_level(
                app,
                SettingKey::Brightness,
                "Brightness",
                before,
                brightness_level(app),
            )
        }
        Command::ThemeSet(Theme::Dark) => {
            let before = theme_mode(app);
            crate::quick_set_dark(app.clone(), true).await?;
            crate::refresh_quick(app).await;
            noted_text(app, SettingKey::Theme, "Theme", before, theme_mode(app))
        }
        Command::ThemeSet(Theme::Light) => {
            let before = theme_mode(app);
            crate::quick_set_dark(app.clone(), false).await?;
            crate::refresh_quick(app).await;
            noted_text(app, SettingKey::Theme, "Theme", before, theme_mode(app))
        }
        Command::ThemeSet(Theme::Auto) => {
            let before = theme_mode(app);
            let update = shell_state(app)
                .lock()
                .expect("shell state")
                .quick
                .set_auto();
            crate::send_setting(update).await?;
            crate::refresh_quick(app).await;
            noted_text(app, SettingKey::Theme, "Theme", before, theme_mode(app))
        }
        Command::ThemeToggle => {
            let before = theme_mode(app);
            crate::toggle_dark_theme(app).await;
            crate::refresh_quick(app).await;
            noted_text(app, SettingKey::Theme, "Theme", before, theme_mode(app))
        }
        Command::ThemeGet => {
            let dark = shell_state(app)
                .lock()
                .expect("shell state")
                .quick
                .view()
                .dark;
            match dark {
                Some(true) => Ok("dark\n".into()),
                Some(false) => Ok("light\n".into()),
                None => Err("the theme is not available".into()),
            }
        }
        Command::NightLight(Some(on)) => {
            let before = night_light(app);
            crate::quick_set_night_light(app.clone(), on).await?;
            crate::refresh_quick(app).await;
            noted_text(
                app,
                SettingKey::NightLight,
                "Night light",
                before,
                night_light(app),
            )
        }
        Command::NightLight(None) => {
            let before = night_light(app);
            crate::toggle_night_light(app).await;
            crate::refresh_quick(app).await;
            noted_text(
                app,
                SettingKey::NightLight,
                "Night light",
                before,
                night_light(app),
            )
        }
        Command::DndSet(on) => {
            let before = dnd(app);
            crate::notify::notification_set_dnd(app.clone(), on).await;
            noted_text(
                app,
                SettingKey::DoNotDisturb,
                "Do not disturb",
                before,
                dnd(app),
            )
        }
        Command::DndToggle => {
            let before = dnd(app);
            crate::notify::toggle_dnd(app).await;
            noted_text(
                app,
                SettingKey::DoNotDisturb,
                "Do not disturb",
                before,
                dnd(app),
            )
        }
        Command::DndStatus => {
            let on = crate::notify::dnd_is_on(app);
            Ok(if on { "on\n" } else { "off\n" }.into())
        }
        Command::PanelOpen(id) => {
            on_main(app, move |app| open_panel(app, &id)).map_err(|e| e.to_string())?;
            ok()
        }
        Command::PanelToggle(id) => {
            let is_open = current_panel(app).await.as_deref() == Some(id.as_str());
            on_main(app, move |app| {
                if is_open {
                    close_panel(app)
                } else {
                    open_panel(app, &id)
                }
            })
            .map_err(|e| e.to_string())?;
            ok()
        }
        Command::Status => {
            let panel = current_panel(app).await;
            let state = shell_state(app);
            let shell = state.lock().expect("shell state");
            let now = crate::notify::now_ms_of(&shell);
            let audio = shell.audio.view();
            let quick = shell.quick.view();
            let input = StatusInput {
                panel,
                dark: quick.dark,
                night_light: quick.night_light,
                do_not_disturb: shell.notifications.view(now).dnd,
                volume: audio.available.then_some(audio.percent),
                muted: audio.available.then_some(audio.muted),
                brightness: shell
                    .brightness
                    .map(|b| b.percent().round().clamp(0., 100.) as u8),
                workspaces: shell
                    .niri
                    .workspace_strip()
                    .iter()
                    .map(|w| (w.idx, w.active, w.windows))
                    .collect(),
                windows: shell.dock.window_facts(),
            };
            Ok(format!("{}\n", status_json(&input)))
        }
        Command::WindowClose(id) => {
            // Only a window the shell knows (what `status` listed) can be named; closing is a request to the program, not a kill.
            // "The focused window" is refused: focus can move between the person reading the dialog and pressing Allow, so only a named window is closed.
            let Some(id) = id else {
                return Err("name the window: window-close <id> (ids are in status)".into());
            };
            let known = shell_state(app)
                .lock()
                .expect("shell state")
                .niri
                .window(id)
                .is_some();
            if !known {
                return Err("no such window".into());
            }
            crate::dock::niri_action(&serde_json::json!({"CloseWindow": {"id": id}}))?;
            ok()
        }
        Command::WorkspaceSwitch(direction) => {
            let before = active_workspace(app);
            let after = workspace_neighbor(app, direction);
            let action = match direction {
                Direction::Next => "FocusWorkspaceDown",
                Direction::Prev => "FocusWorkspaceUp",
            };
            crate::dock::niri_action(&serde_json::json!({ action: {} }))?;
            noted_workspace(app, before, after)
        }
        Command::PanelClose(which) => {
            let open = current_panel(app).await;
            // Closing a named panel that is not the open one does nothing, like the native shell.
            if which.is_none() || which == open {
                on_main(app, close_panel).map_err(|e| e.to_string())?;
            }
            ok()
        }
        Command::SettingsOpen(section) => {
            let mut command = std::process::Command::new("helix-settings");
            crate::apps::scrub_shell_env(&mut command);
            if let Some(page) = section {
                command.arg("--page").arg(page);
            }
            match command.spawn() {
                Ok(_) => ok(),
                Err(error) => Err(format!("could not open Settings: {error}")),
            }
        }
        Command::AppLaunch(query) => {
            let state = shell_state(app);
            let mut shell = state.lock().expect("shell state");
            if shell.apps.is_empty() {
                shell.apps = crate::apps::scan();
            }
            let Some(entry) =
                helix_shell_model::launcher::resolve_launch(&shell.apps, &query).cloned()
            else {
                return Err("no matching application".into());
            };
            drop(shell);
            let _ = crate::dock::place_on_agent_desk(1);
            match crate::apps::launch(&entry) {
                Ok(()) => {
                    let mut shell = state.lock().expect("shell state");
                    shell.history.record(&entry.id);
                    crate::apps::save_history(&shell.history);
                    eprintln!("AGENT-IPC launched {}", entry.id);
                    ok()
                }
                Err(error) => Err(error),
            }
        }
        Command::ClipboardText => {
            let text = crate::insert::paste_clipboard_text()?;
            Ok(format!("{text}\n"))
        }
        Command::ClipboardCopy(text) => {
            crate::insert::copy_text(&text)?;
            ok()
        }
        Command::ClipboardClear => {
            shell_state(app)
                .lock()
                .expect("shell state")
                .insert
                .forget_clipboard();
            crate::insert::clear_clipboard_selection();
            ok()
        }
        Command::DesktopCapture(target) => {
            let app = app.clone();
            tokio::task::spawn_blocking(move || crate::capture::desktop_capture(&app, target))
                .await
                .map_err(|_| "desktop capture failed".to_string())?
        }
        Command::AgentDeskFocus => {
            let before = active_workspace(app);
            let after = agent_desk(app);
            crate::dock::agent_desk_focus()?;
            noted_workspace(app, before, after)
        }
        Command::BarShow => {
            let before = bar_text();
            crate::bar::set_visible(app, true)?;
            noted_text(app, SettingKey::Bar, "Bar", before, bar_text())
        }
        Command::BarHide => {
            let before = bar_text();
            crate::bar::set_visible(app, false)?;
            noted_text(app, SettingKey::Bar, "Bar", before, bar_text())
        }
        Command::BarToggle => {
            let before = bar_text();
            crate::bar::toggle(app)?;
            noted_text(app, SettingKey::Bar, "Bar", before, bar_text())
        }
    }
}

fn changes() -> std::sync::MutexGuard<'static, ChangeLedger> {
    static LEDGER: std::sync::Mutex<ChangeLedger> = std::sync::Mutex::new(ChangeLedger::new());
    LEDGER.lock().expect("act change ledger")
}

fn assistant_running(app: &AppHandle) -> bool {
    let phase = shell_state(app)
        .lock()
        .expect("shell state")
        .session
        .view()
        .phase;
    matches!(phase, Phase::Thinking | Phase::Acting)
}

/// Returns the display sentence when both sides were read. Records an undo only for the assistant task
/// that is actually running, and only when the value moved.
fn noted(
    app: &AppHandle,
    key: SettingKey,
    label: &str,
    before: Option<String>,
    after: Option<String>,
    before_machine: Option<String>,
    after_machine: Option<String>,
) -> Result<String, String> {
    let (Some(before), Some(after)) = (before, after) else {
        return Ok("ok\n".into());
    };
    let line = sentence(label, &before, &after);
    if line.is_empty() {
        return Ok("ok\n".into());
    }
    if assistant_running(app) {
        if let (Some(before_machine), Some(after_machine)) = (before_machine, after_machine) {
            let recorded = changes().record(
                crate::act_feed::current_task(),
                key,
                &before_machine,
                &after_machine,
                &before,
                &after,
            );
            if recorded {
                crate::act_feed::note_change(&before, &after);
            }
        }
    }
    Ok(format!("{line}\n"))
}

/// The ledger entry for this task, if the shell still holds one.
pub(crate) fn remembered(task: u64) -> Option<ChangeRecord> {
    changes().get(task).cloned()
}

/// Whether the live setting is still the value this task wrote.
pub(crate) fn still_current(app: &AppHandle, record: &ChangeRecord) -> bool {
    live_machine(app, record.key).as_deref() == Some(record.after_machine.as_str())
}

/// Writes the previous value back. The ledger entry is removed only after that write succeeds.
pub(crate) async fn undo(app: &AppHandle, task: u64) -> Result<(), String> {
    let record = changes()
        .get(task)
        .cloned()
        .ok_or_else(|| "Nothing to undo.".to_string())?;
    let live = live_machine(app, record.key)
        .ok_or_else(|| "That setting is not available.".to_string())?;
    if live != record.after_machine {
        let _ = changes().take_if_current(task, &live);
        crate::act_feed::clear_undoable(task);
        return Err("The desktop has changed since then.".into());
    }
    restore(app, &record).await?;
    let _ = changes().take_if_current(task, &record.after_machine);
    crate::act_feed::note_restored(task, &format!("Restored to {}.", record.before));
    Ok(())
}

fn live_machine(app: &AppHandle, key: SettingKey) -> Option<String> {
    match key {
        SettingKey::Volume => audio_level(app).map(|(percent, muted)| volume_machine(percent, muted)),
        SettingKey::Brightness => brightness_level(app).map(|percent| format!("{percent}%")),
        SettingKey::Theme => theme_mode(app),
        SettingKey::NightLight => night_light(app),
        SettingKey::DoNotDisturb => dnd(app),
        SettingKey::Bar => bar_text(),
        SettingKey::Workspace => active_workspace(app).map(|(id, _)| id.to_string()),
    }
}

fn parse_volume(machine: &str) -> Option<(u8, bool)> {
    let (percent, flag) = machine.split_once(' ')?;
    let percent: u8 = percent.parse().ok()?;
    if percent > 100 {
        return None;
    }
    let muted = match flag {
        "1" => true,
        "0" => false,
        _ => return None,
    };
    Some((percent, muted))
}

fn on_off(machine: &str) -> Result<bool, String> {
    match machine {
        "on" => Ok(true),
        "off" => Ok(false),
        _ => Err("That setting is not available.".into()),
    }
}

async fn restore(app: &AppHandle, record: &ChangeRecord) -> Result<(), String> {
    match record.key {
        SettingKey::Volume => {
            let (percent, muted) =
                parse_volume(&record.before_machine).ok_or("That setting is not available.")?;
            crate::send_audio(crate::audio_request(app, |audio| {
                audio.set_volume(f64::from(percent))
            }))
            .await?;
            crate::send_audio(crate::audio_request(app, |audio| audio.set_muted(muted))).await?;
            crate::refresh_audio(app).await;
        }
        SettingKey::Brightness => {
            let percent: f64 = record
                .before_machine
                .trim_end_matches('%')
                .parse()
                .map_err(|_| "That setting is not available.".to_string())?;
            crate::brightness_set(app.clone(), percent).await?;
        }
        SettingKey::Theme => {
            match record.before_machine.as_str() {
                "dark" => crate::quick_set_dark(app.clone(), true).await?,
                "light" => crate::quick_set_dark(app.clone(), false).await?,
                "auto" => {
                    let update = shell_state(app).lock().expect("shell state").quick.set_auto();
                    crate::send_setting(update).await?;
                }
                _ => return Err("That setting is not available.".into()),
            }
            crate::refresh_quick(app).await;
        }
        SettingKey::NightLight => {
            let on = on_off(&record.before_machine)?;
            crate::quick_set_night_light(app.clone(), on).await?;
            crate::refresh_quick(app).await;
        }
        SettingKey::DoNotDisturb => {
            let on = on_off(&record.before_machine)?;
            crate::notify::notification_set_dnd(app.clone(), on).await;
        }
        SettingKey::Bar => {
            let shown = match record.before_machine.as_str() {
                "shown" => true,
                "hidden" => false,
                _ => return Err("That setting is not available.".into()),
            };
            crate::bar::set_visible(app, shown)?;
        }
        SettingKey::Workspace => {
            let id: u64 = record
                .before_machine
                .parse()
                .map_err(|_| "That setting is not available.".to_string())?;
            crate::dock::workspace_focus(app.clone(), id)?;
        }
    }
    Ok(())
}

fn noted_text(
    app: &AppHandle,
    key: SettingKey,
    label: &str,
    before: Option<String>,
    after: Option<String>,
) -> Result<String, String> {
    noted(
        app,
        key,
        label,
        before.clone(),
        after.clone(),
        before,
        after,
    )
}

fn noted_level(
    app: &AppHandle,
    key: SettingKey,
    label: &str,
    before: Option<u8>,
    after: Option<u8>,
) -> Result<String, String> {
    let text = |value: Option<u8>| value.map(|value| format!("{value}%"));
    noted_text(app, key, label, text(before), text(after))
}

fn noted_volume(
    app: &AppHandle,
    before: Option<(u8, bool)>,
    after: Option<(u8, bool)>,
) -> Result<String, String> {
    let text =
        |level: Option<(u8, bool)>| level.map(|(percent, muted)| volume_text(percent, muted));
    let machine =
        |level: Option<(u8, bool)>| level.map(|(percent, muted)| volume_machine(percent, muted));
    noted(
        app,
        SettingKey::Volume,
        "Volume",
        text(before),
        text(after),
        machine(before),
        machine(after),
    )
}

fn noted_workspace(
    app: &AppHandle,
    before: Option<(u64, String)>,
    after: Option<(u64, String)>,
) -> Result<String, String> {
    let text = |place: &Option<(u64, String)>| place.as_ref().map(|(_, name)| name.clone());
    let machine = |place: &Option<(u64, String)>| place.as_ref().map(|(id, _)| id.to_string());
    noted(
        app,
        SettingKey::Workspace,
        "Workspace",
        text(&before),
        text(&after),
        machine(&before),
        machine(&after),
    )
}

fn audio_level(app: &AppHandle) -> Option<(u8, bool)> {
    let view = shell_state(app).lock().expect("shell state").audio.view();
    view.available.then_some((view.percent, view.muted))
}

fn brightness_level(app: &AppHandle) -> Option<u8> {
    shell_state(app)
        .lock()
        .expect("shell state")
        .brightness
        .map(|brightness| brightness.percent().round().clamp(0.0, 100.0) as u8)
}

fn theme_mode(app: &AppHandle) -> Option<String> {
    shell_state(app)
        .lock()
        .expect("shell state")
        .quick
        .mode()
        .map(str::to_string)
}

fn night_light(app: &AppHandle) -> Option<String> {
    shell_state(app)
        .lock()
        .expect("shell state")
        .quick
        .view()
        .night_light
        .map(|on| if on { "on" } else { "off" }.to_string())
}

fn dnd(app: &AppHandle) -> Option<String> {
    Some(
        if crate::notify::dnd_is_on(app) {
            "on"
        } else {
            "off"
        }
        .to_string(),
    )
}

fn bar_text() -> Option<String> {
    Some(
        if crate::bar::is_visible() {
            "shown"
        } else {
            "hidden"
        }
        .to_string(),
    )
}

fn workspace_label(name: &str, idx: u64) -> String {
    let name = helix_agent::display::sanitize_for_display(name, 40);
    if name.is_empty() {
        format!("workspace {idx}")
    } else {
        name
    }
}

fn workspaces(app: &AppHandle) -> Vec<helix_compositor::niri::WorkspaceView> {
    shell_state(app)
        .lock()
        .expect("shell state")
        .niri
        .workspace_strip()
}

fn active_workspace(app: &AppHandle) -> Option<(u64, String)> {
    workspaces(app)
        .into_iter()
        .find(|workspace| workspace.active)
        .map(|workspace| {
            (
                workspace.id,
                workspace_label(&workspace.name, workspace.idx),
            )
        })
}

/// The workspace `next` or `prev` will land on, in strip order (the same order as niri's up and down).
fn workspace_neighbor(app: &AppHandle, direction: Direction) -> Option<(u64, String)> {
    let strip = workspaces(app);
    let position = strip.iter().position(|workspace| workspace.active)?;
    let next = match direction {
        Direction::Next => strip.get(position + 1),
        Direction::Prev => position.checked_sub(1).and_then(|index| strip.get(index)),
    }?;
    Some((next.id, workspace_label(&next.name, next.idx)))
}

fn agent_desk(app: &AppHandle) -> Option<(u64, String)> {
    workspaces(app)
        .into_iter()
        .find(|workspace| workspace.agent_desk)
        .map(|workspace| {
            (
                workspace.id,
                workspace_label(&workspace.name, workspace.idx),
            )
        })
}

/// What the log may say about a command: never clipboard contents.
fn describe(command: &Command) -> String {
    match command {
        Command::ClipboardCopy(text) => format!("ClipboardCopy({} chars)", text.chars().count()),
        Command::AppLaunch(query) => format!("AppLaunch({query})"),
        other => format!("{other:?}"),
    }
}

async fn serve_one(app: AppHandle, mut stream: UnixStream) {
    let mut buffer = Vec::new();
    let read = tokio::time::timeout(
        Duration::from_secs(2),
        (&mut stream)
            .take(MAX_LINE as u64 + 1)
            .read_to_end(&mut buffer),
    )
    .await;
    if read.is_err() {
        return;
    }
    let reply = match String::from_utf8(buffer) {
        Ok(line) => match parse(&line) {
            Ok(command) => {
                eprintln!("AGENT-IPC {}", describe(&command));
                match execute(&app, command).await {
                    Ok(text) => text,
                    Err(error) => format!("error: {}\n", error.lines().next().unwrap_or("failed")),
                }
            }
            Err(error) => error.wire(),
        },
        Err(_) => "error: not text\n".to_string(),
    };
    let _ = stream.write_all(reply.as_bytes()).await;
    let _ = stream.shutdown().await;
}

/// Who, if anyone, answers on the socket.
enum Probe {
    Gone,
    /// helix-desktop's fallback listener (it answers `--help` with `Usage: helix-panel`).
    Service,
    Shell,
}

async fn probe(path: &std::path::Path) -> Probe {
    let Ok(mut stream) = UnixStream::connect(path).await else {
        return Probe::Gone;
    };
    let ask = async {
        stream.write_all(b"--help\n").await.ok()?;
        stream.shutdown().await.ok()?;
        let mut reply = String::new();
        stream.read_to_string(&mut reply).await.ok()?;
        Some(reply)
    };
    match tokio::time::timeout(Duration::from_secs(1), ask).await {
        Ok(Some(reply)) if is_service_help(&reply) => Probe::Service,
        _ => Probe::Shell,
    }
}

fn is_service_help(reply: &str) -> bool {
    reply.starts_with("Usage: helix-panel")
}

/// Starts answering on the socket, unless another shell is already doing so.
pub fn serve(app: AppHandle) {
    let (Some(path), Some(tools)) = (socket_path(), tools_path()) else {
        eprintln!("AGENT-IPC no runtime directory or display: not serving");
        return;
    };
    tauri::async_runtime::spawn(async move {
        match probe(&path).await {
            Probe::Gone => {}
            Probe::Service => {
                // The desktop service binds this path when it starts before the shell; the shell is the real owner.
                eprintln!(
                    "AGENT-IPC taking {} over from the desktop service",
                    path.display()
                );
            }
            Probe::Shell => {
                eprintln!("AGENT-IPC another shell already serves {}", path.display());
                return;
            }
        }
        // Nothing answered: whatever is at the path is a leftover of a shell that has gone.
        let _ = std::fs::remove_file(&path);
        let listener = match UnixListener::bind(&path) {
            Ok(listener) => listener,
            Err(error) => {
                eprintln!("AGENT-IPC cannot bind {}: {error}", path.display());
                return;
            }
        };
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600));
        }
        let mut list = format!("{}\n", std::process::id());
        for name in SUPPORTED {
            list.push_str(name);
            list.push('\n');
        }
        if std::fs::write(&tools, list).is_ok() {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(&tools, std::fs::Permissions::from_mode(0o600));
        }
        eprintln!(
            "AGENT-IPC serving {} ({} commands)",
            path.display(),
            SUPPORTED.len()
        );
        loop {
            match listener.accept().await {
                Ok((stream, _)) => {
                    tauri::async_runtime::spawn(serve_one(app.clone(), stream));
                }
                Err(error) => {
                    eprintln!("AGENT-IPC accept failed: {error}");
                    tokio::time::sleep(Duration::from_millis(200)).await;
                }
            }
        }
    });
}

#[cfg(test)]
mod probe_tests {
    #[test]
    fn recognises_the_service_help_text() {
        assert!(super::is_service_help(
            "Usage: helix-panel <command> [args]\n"
        ));
        assert!(!super::is_service_help("error: unknown command\n"));
    }
}
