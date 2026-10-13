//! The shell's command socket, in the wire format the desktop service already speaks to a shell (`helix-<display>.sock`, one line in,
//! one reply out). It is what makes an agent's approved action happen under this shell: the service checks the client, tier, grant and
//! confirmation (Agent Surface) and then sends the command here. Only the commands in `helix_shell_core::agent_msg::SUPPORTED` exist; the line is
//! parsed by that pure model and each command runs through the same code as the sliders, launcher and key bindings, so every range and
//! refusal that applies there applies here. Free-text payloads (`clipboard-copy`, `app-launch`) are never written to the log. The socket
//! is private to the user (it lives in `XDG_RUNTIME_DIR`, mode 0600), and like `org.helix.Shell1` it is not an authority boundary against
//! other processes of the same user: Agent Surface is what gates agents.

use std::path::PathBuf;
use std::time::Duration;

use helix_shell_core::agent_msg::{parse, Command, Direction, Theme, MAX_LINE, SUPPORTED};
use helix_shell_core::agent_status::{status_json, StatusInput};
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
    let display = std::env::var("WAYLAND_DISPLAY").ok().filter(|d| !d.is_empty() && !d.contains('/'))?;
    Some(runtime_dir()?.join(format!("helix-{display}.sock")))
}

/// Where the shell says which commands it carries out, so the service lists only those (first line: this shell's pid, so a stale file from
/// a shell that has gone is ignored).
fn tools_path() -> Option<PathBuf> {
    let display = std::env::var("WAYLAND_DISPLAY").ok().filter(|d| !d.is_empty() && !d.contains('/'))?;
    Some(runtime_dir()?.join(format!("helix-shell-tools-{display}")))
}

async fn execute(app: &AppHandle, command: Command) -> Result<String, String> {
    let ok = || Ok("ok\n".to_string());
    match command {
        Command::VolumeStep(delta) => {
            crate::adjust_volume(app, delta).await;
            ok()
        }
        Command::VolumeToggleMute => {
            crate::toggle_mute(app).await;
            ok()
        }
        Command::VolumeSet(percent) => {
            crate::audio_set_volume(app.clone(), percent).await?;
            ok()
        }
        Command::BrightnessStep(delta) => {
            crate::adjust_brightness(app, delta).await;
            ok()
        }
        Command::BrightnessSet(percent) => {
            crate::brightness_set(app.clone(), percent).await?;
            ok()
        }
        Command::ThemeSet(Theme::Dark) => {
            crate::quick_set_dark(app.clone(), true).await?;
            ok()
        }
        Command::ThemeSet(Theme::Light) => {
            crate::quick_set_dark(app.clone(), false).await?;
            ok()
        }
        Command::ThemeSet(Theme::Auto) => {
            let update = shell_state(app).lock().expect("shell state").quick.set_auto();
            crate::send_setting(update).await?;
            ok()
        }
        Command::ThemeToggle => {
            crate::toggle_dark_theme(app).await;
            ok()
        }
        Command::ThemeGet => {
            let dark = shell_state(app).lock().expect("shell state").quick.view().dark;
            match dark {
                Some(true) => Ok("dark\n".into()),
                Some(false) => Ok("light\n".into()),
                None => Err("the theme is not available".into()),
            }
        }
        Command::NightLight(Some(on)) => {
            crate::quick_set_night_light(app.clone(), on).await?;
            ok()
        }
        Command::NightLight(None) => {
            crate::toggle_night_light(app).await;
            ok()
        }
        Command::DndSet(on) => {
            crate::notify::notification_set_dnd(app.clone(), on).await;
            ok()
        }
        Command::DndToggle => {
            crate::notify::toggle_dnd(app).await;
            ok()
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
                brightness: shell.brightness.map(|b| b.percent().round().clamp(0., 100.) as u8),
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
            let known = shell_state(app).lock().expect("shell state").niri.window(id).is_some();
            if !known {
                return Err("no such window".into());
            }
            crate::dock::niri_action(&serde_json::json!({"CloseWindow": {"id": id}}))?;
            ok()
        }
        Command::WorkspaceSwitch(direction) => {
            let action = match direction {
                Direction::Next => "FocusWorkspaceDown",
                Direction::Prev => "FocusWorkspaceUp",
            };
            crate::dock::niri_action(&serde_json::json!({ action: {} }))?;
            ok()
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
            let Some(entry) = helix_shell_core::launcher::resolve_launch(&shell.apps, &query).cloned()
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
            crate::dock::agent_desk_focus()?;
            ok()
        }
        Command::BarShow => {
            crate::bar::set_visible(app, true)?;
            ok()
        }
        Command::BarHide => {
            crate::bar::set_visible(app, false)?;
            ok()
        }
        Command::BarToggle => {
            crate::bar::toggle(app)?;
            ok()
        }
    }
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
        (&mut stream).take(MAX_LINE as u64 + 1).read_to_end(&mut buffer),
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

/// Starts answering on the socket, unless another shell is already doing so.
pub fn serve(app: AppHandle) {
    let (Some(path), Some(tools)) = (socket_path(), tools_path()) else {
        eprintln!("AGENT-IPC no runtime directory or display: not serving");
        return;
    };
    tauri::async_runtime::spawn(async move {
        if UnixStream::connect(&path).await.is_ok() {
            eprintln!("AGENT-IPC another shell already serves {}", path.display());
            return;
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
        eprintln!("AGENT-IPC serving {} ({} commands)", path.display(), SUPPORTED.len());
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
