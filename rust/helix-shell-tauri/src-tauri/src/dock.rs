//! The dock's bridge: niri's event stream in, the dock model's view out, and what a click does (focus a window through niri, start an
//! application through the launcher). What the dock shows and which ids may be acted on is decided by `helix_shell_core::dock`; this
//! file only moves values. niri is spoken to with its documented JSON IPC over `$NIRI_SOCKET`: a long-lived connection for the event
//! stream (a blocked read, no polling) and a short one per action.

use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::time::Duration;

use helix_shell_core::dock::{Action, ItemView};
use serde::Serialize;
use serde_json::Value;
use tauri::{AppHandle, Emitter};

use crate::{apps, shell_state};

fn socket_path() -> Option<PathBuf> {
    std::env::var_os("NIRI_SOCKET")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
}

fn config_path() -> Option<PathBuf> {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")))?;
    Some(base.join("helix/dock.json"))
}

/// The pinned application ids saved last time, or `None` on a first run.
pub fn load_pins() -> Option<Vec<String>> {
    let text = std::fs::read_to_string(config_path()?).ok()?;
    let value: Value = serde_json::from_str(&text).ok()?;
    Some(
        value
            .get("pinned")?
            .as_array()?
            .iter()
            .filter_map(|v| v.as_str().map(str::to_string))
            .collect(),
    )
}

pub fn save_pins(pins: &[String]) {
    use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
    let Some(path) = config_path() else { return };
    let Some(dir) = path.parent() else { return };
    if std::fs::DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(dir)
        .is_err()
    {
        return;
    }
    let text = serde_json::json!({"version": 1, "pinned": pins}).to_string();
    let temporary = path.with_extension("json.tmp");
    let written = std::fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(0o600)
        .open(&temporary)
        .and_then(|mut file| file.write_all(text.as_bytes()))
        .and_then(|()| std::fs::rename(&temporary, &path));
    if written.is_err() {
        let _ = std::fs::remove_file(&temporary);
    }
}

/// One request to niri on its own short connection; returns the reply line.
fn request(json: &str) -> Result<String, String> {
    let path = socket_path().ok_or("no niri socket")?;
    let mut stream = UnixStream::connect(path).map_err(|e| e.to_string())?;
    stream
        .set_read_timeout(Some(Duration::from_secs(2)))
        .map_err(|e| e.to_string())?;
    stream
        .write_all(format!("{json}\n").as_bytes())
        .map_err(|e| e.to_string())?;
    let mut reply = String::new();
    BufReader::new(stream)
        .read_line(&mut reply)
        .map_err(|e| e.to_string())?;
    Ok(reply)
}

fn focus_window(id: u64) -> Result<(), String> {
    let reply = request(&serde_json::json!({"Action": {"FocusWindow": {"id": id}}}).to_string())?;
    if reply.contains("\"Ok\"") {
        Ok(())
    } else {
        Err(format!("niri refused: {}", reply.trim()))
    }
}

#[derive(Clone, Serialize)]
pub struct DockItem {
    #[serde(flatten)]
    item: ItemView,
    /// A `data:` image for the icon, when one could be read; the page draws a lettered tile otherwise.
    icon_uri: Option<String>,
}

fn items(app: &AppHandle) -> Vec<DockItem> {
    let state = shell_state(app);
    let mut shell = state.lock().expect("shell state");
    let views = shell.dock.view();
    views
        .into_iter()
        .map(|item| {
            let uri = if item.icon.is_empty() {
                None
            } else {
                if !shell.icons.contains_key(&item.icon) {
                    let uri = apps::icon_data_uri(&item.icon).unwrap_or_default();
                    shell.icons.insert(item.icon.clone(), uri);
                }
                shell
                    .icons
                    .get(&item.icon)
                    .filter(|u| !u.is_empty())
                    .cloned()
            };
            DockItem {
                item,
                icon_uri: uri,
            }
        })
        .collect()
}

/// Recomputes the dock from the installed applications and open windows, and tells the page.
pub fn refresh(app: &AppHandle) {
    {
        let state = shell_state(app);
        let mut guard = state.lock().expect("shell state");
        let shell = &mut *guard;
        shell.dock.refresh(&shell.dock_apps, &shell.niri);
    }
    let view = items(app);
    eprintln!(
        "DOCK view items={} running={}",
        view.len(),
        view.iter().filter(|i| i.item.running > 0).count()
    );
    let _ = app.emit("dock-view", view);
    publish_workspaces(app);
}

/// Tells the page the workspace strip, but only when it differs from what it was last told (the event stream is chatty).
fn publish_workspaces(app: &AppHandle) {
    static LAST: std::sync::Mutex<Vec<helix_shell_core::niri::WorkspaceView>> =
        std::sync::Mutex::new(Vec::new());
    let strip = shell_state(app)
        .lock()
        .expect("shell state")
        .niri
        .workspace_strip();
    let mut last = LAST.lock().expect("workspace strip");
    if *last != strip {
        eprintln!(
            "WORKSPACES {} active={:?}",
            strip.len(),
            strip.iter().find(|w| w.active).map(|w| w.idx)
        );
        *last = strip.clone();
        let _ = app.emit("workspaces-view", strip);
    }
}

/// Loads the applications and the pinned list, then follows niri's events for as long as the shell runs. Reconnects (after a pause) if
/// the connection drops, so a restarted compositor is picked up.
pub fn start(app: AppHandle) {
    {
        let state = shell_state(&app);
        let mut guard = state.lock().expect("shell state");
        guard.dock_apps = apps::scan();
        let pins = load_pins()
            .unwrap_or_else(|| helix_shell_core::dock::Dock::default_pins(&guard.dock_apps));
        guard.dock = helix_shell_core::dock::Dock::new(pins);
    }
    refresh(&app);
    std::thread::spawn(move || loop {
        match socket_path() {
            Some(path) => match UnixStream::connect(&path) {
                Ok(mut stream) => {
                    if stream.write_all(b"\"EventStream\"\n").is_ok() {
                        eprintln!("NIRI following the event stream");
                        follow(&app, stream);
                        eprintln!("NIRI event stream ended");
                    }
                }
                Err(error) => eprintln!("NIRI cannot connect to {}: {error}", path.display()),
            },
            None => eprintln!("NIRI no socket in the environment"),
        }
        std::thread::sleep(Duration::from_secs(3));
    });
}

fn follow(app: &AppHandle, stream: UnixStream) {
    for line in BufReader::new(stream).lines().map_while(Result::ok) {
        let Ok(event) = serde_json::from_str::<Value>(&line) else {
            continue;
        };
        let changed = shell_state(app)
            .lock()
            .expect("shell state")
            .niri
            .apply(&event);
        if changed {
            refresh(app);
        }
    }
}

fn run(
    app: &AppHandle,
    action: Result<Action, helix_shell_core::dock::RequestError>,
) -> Result<(), String> {
    match action.map_err(|e| format!("{e:?}"))? {
        Action::Focus(id) => focus_window(id),
        Action::Launch(id) => {
            let found = shell_state(app)
                .lock()
                .expect("shell state")
                .dock_apps
                .iter()
                .find(|a| a.id == id)
                .cloned();
            apps::launch(&found.ok_or("not installed")?)
        }
    }
}

#[tauri::command]
pub fn dock_view(app: AppHandle) -> Vec<DockItem> {
    items(&app)
}

#[tauri::command]
pub fn dock_activate(app: AppHandle, id: String) -> Result<(), String> {
    let action = shell_state(&app)
        .lock()
        .expect("shell state")
        .dock
        .activate(&id);
    eprintln!("DOCK activate {action:?}");
    let result = run(&app, action);
    if let Err(error) = &result {
        eprintln!("DOCK failed: {error}");
    }
    result
}

#[tauri::command]
pub fn dock_new_window(app: AppHandle, id: String) -> Result<(), String> {
    let action = shell_state(&app)
        .lock()
        .expect("shell state")
        .dock
        .launch_new(&id);
    eprintln!("DOCK new window {action:?}");
    run(&app, action)
}

fn edit(
    app: &AppHandle,
    change: impl FnOnce(
        &mut helix_shell_core::dock::Dock,
    ) -> Result<(), helix_shell_core::dock::RequestError>,
) -> Result<(), String> {
    let pins = {
        let state = shell_state(app);
        let mut shell = state.lock().expect("shell state");
        change(&mut shell.dock).map_err(|e| format!("{e:?}"))?;
        shell.dock.pinned().to_vec()
    };
    save_pins(&pins);
    refresh(app);
    Ok(())
}

#[tauri::command]
pub fn dock_pin(app: AppHandle, id: String) -> Result<(), String> {
    edit(&app, |d| d.pin(&id))
}

#[tauri::command]
pub fn dock_unpin(app: AppHandle, id: String) -> Result<(), String> {
    edit(&app, |d| d.unpin(&id))
}

#[tauri::command]
pub fn dock_move(app: AppHandle, id: String, to: usize) -> Result<(), String> {
    edit(&app, |d| d.move_pinned(&id, to))
}

#[tauri::command]
pub fn workspaces_view(app: AppHandle) -> Vec<helix_shell_core::niri::WorkspaceView> {
    shell_state(&app)
        .lock()
        .expect("shell state")
        .niri
        .workspace_strip()
}

/// Switches to a workspace of the strip the bar shows; any other id is refused here and never sent to niri.
#[tauri::command]
pub fn workspace_focus(app: AppHandle, id: u64) -> Result<(), String> {
    if !shell_state(&app)
        .lock()
        .expect("shell state")
        .niri
        .strip_has(id)
    {
        return Err("not a workspace the bar shows".into());
    }
    let reply = request(
        &serde_json::json!({"Action": {"FocusWorkspace": {"reference": {"Id": id}}}}).to_string(),
    )?;
    eprintln!("WORKSPACES focus {id}");
    if reply.contains("\"Ok\"") {
        Ok(())
    } else {
        Err(format!("niri refused: {}", reply.trim()))
    }
}
