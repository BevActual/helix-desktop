//! The dock's bridge: niri's event stream in, the dock model's view out, and what a click does (focus a window through niri, start an
//! application through the launcher). What the dock shows and which ids may be acted on is decided by `helix_shell_model::dock`; this
//! file only moves values. niri is spoken to with its documented JSON IPC over `$NIRI_SOCKET`: a long-lived connection for the event
//! stream (a blocked read, no polling) and a short one per action.

use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::time::Duration;

use helix_shell_model::dock::{Action, ItemView};
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

/// The dock's look and behaviour as the person set it in Settings (`dock-settings.json`), re-read when the file changes.
static PREFS: std::sync::Mutex<Option<helix_config::dock_prefs::DockPrefs>> =
    std::sync::Mutex::new(None);

pub fn prefs_path() -> Option<PathBuf> {
    config_path().map(|p| p.with_file_name("dock-settings.json"))
}

fn read_prefs() -> helix_config::dock_prefs::DockPrefs {
    prefs_path()
        .and_then(|path| std::fs::read_to_string(path).ok())
        .map(|text| helix_config::dock_prefs::DockPrefs::parse(&text))
        .unwrap_or_default()
}

/// Applies what is not the page's business: the strip the compositor keeps free for the dock.
/// When `helix-shell-strut` is running it owns the dock exclusive zone so bar restart does not flap it.
fn apply_reserve(app: &AppHandle, prefs: &helix_config::dock_prefs::DockPrefs) {
    if crate::strut::strut_unit_active() {
        return;
    }
    let height = prefs.reserved_height().map(crate::ui_scale::scaled);
    let _ = app.run_on_main_thread(move || crate::layer::set_dock_strut(height));
}

/// Applies the dock's strip again (the interface scale changed its size).
pub fn reapply_reserve(app: &AppHandle) {
    apply_reserve(app, &prefs());
}

pub fn prefs() -> helix_config::dock_prefs::DockPrefs {
    PREFS
        .lock()
        .expect("dock prefs")
        .get_or_insert_with(read_prefs)
        .clone()
}

#[tauri::command]
pub fn dock_prefs() -> helix_config::dock_prefs::DockPrefs {
    prefs()
}

/// Looks at the settings file once a second (a stat, nothing more) and tells the page and the dock list when it changed.
fn watch_prefs(app: AppHandle) {
    std::thread::spawn(move || {
        let stamp = || {
            prefs_path()
                .and_then(|p| std::fs::metadata(p).ok())
                .and_then(|m| m.modified().ok())
        };
        let mut seen = stamp();
        loop {
            std::thread::sleep(Duration::from_secs(1));
            let now = stamp();
            if now == seen {
                continue;
            }
            seen = now;
            let next = read_prefs();
            let changed = {
                let mut guard = PREFS.lock().expect("dock prefs");
                let changed = guard.as_ref() != Some(&next);
                *guard = Some(next.clone());
                changed
            };
            if changed {
                eprintln!("DOCK settings changed");
                apply_reserve(&app, &next);
                let _ = app.emit("dock-prefs", next);
                refresh(&app);
            }
        }
    });
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

/// Queue the next opened window(s) on the Helix agent desk without stealing focus.
pub(crate) fn place_on_agent_desk(count: u8) -> Result<(), String> {
    helix_compositor::compositor_ipc::place_on_agent_desk(count)
}

/// One niri action (`{"CloseWindow": {"id": 3}}`); `Ok` only when niri said so.
pub(crate) fn niri_action(action: &Value) -> Result<(), String> {
    let reply = request(&serde_json::json!({ "Action": action }).to_string())?;
    if reply.contains("\"Ok\"") {
        Ok(())
    } else {
        Err(format!("niri refused: {}", reply.trim()))
    }
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
    let show_running = prefs().running;
    let views = shell.dock.view();
    views
        .into_iter()
        .filter(|item| show_running || item.pinned)
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

/// What each window's process really is, read from `/proc` (the pid is the compositor's, taken from the connection). A process that cannot
/// be read (it has gone) is simply left out: the dock then keeps the application its window names.
fn read_processes(
    niri: &helix_compositor::niri::Niri,
) -> std::collections::HashMap<u32, helix_shell_model::identity::Process> {
    let mut found = std::collections::HashMap::new();
    for pid in niri.windows().filter_map(|w| w.pid) {
        if found.contains_key(&pid) {
            continue;
        }
        let Ok(exe) = std::fs::read_link(format!("/proc/{pid}/exe")) else {
            continue;
        };
        let mut raw = std::fs::read(format!("/proc/{pid}/cmdline")).unwrap_or_default();
        raw.truncate(4096);
        let cmdline = raw
            .split(|b| *b == 0)
            .filter(|part| !part.is_empty())
            .take(16)
            .map(|part| String::from_utf8_lossy(part).into_owned())
            .collect();
        // A Flatpak sandbox records which application it belongs to in a file the program inside cannot choose.
        let flatpak = std::fs::read_to_string(format!("/proc/{pid}/root/.flatpak-info"))
            .ok()
            .and_then(|text| {
                text.lines()
                    .skip_while(|l| l.trim() != "[Application]")
                    .skip(1)
                    .take_while(|l| !l.starts_with('['))
                    .find_map(|l| l.strip_prefix("name=").map(|n| n.trim().to_string()))
            })
            .filter(|name| !name.is_empty());
        found.insert(
            pid,
            helix_shell_model::identity::Process {
                exe: exe.to_string_lossy().into_owned(),
                cmdline,
                flatpak,
            },
        );
    }
    found
}

/// Recomputes the dock from the installed applications and open windows, and tells the page.
pub fn refresh(app: &AppHandle) {
    {
        let state = shell_state(app);
        let mut guard = state.lock().expect("shell state");
        let shell = &mut *guard;
        let processes = read_processes(&shell.niri);
        shell
            .dock
            .refresh_checked(&shell.dock_apps, &shell.niri, &processes);
    }
    let view = items(app);
    eprintln!(
        "DOCK view items={} running={} unverified={}",
        view.len(),
        view.iter().filter(|i| i.item.running > 0).count(),
        view.iter().filter(|i| i.item.unverified).count()
    );
    let _ = app.emit("dock-view", view);
    publish_workspaces(app);
    publish_active_window(app);
    crate::island::publish(app);
}

#[derive(Clone, Default, PartialEq, Eq, Serialize)]
pub struct ActiveWindow {
    app: String,
    title: String,
}

/// The window that has the focus: the application's name (from its desktop entry, else the app id) and the window's title, for the bar.
fn active_window(app: &AppHandle) -> ActiveWindow {
    let state = shell_state(app);
    let shell = state.lock().expect("shell state");
    let Some(window) = shell.niri.windows().find(|w| w.focused) else {
        return ActiveWindow::default();
    };
    let id = window.app_id.to_lowercase();
    let name = shell
        .dock_apps
        .iter()
        .find(|a| {
            a.id.trim_end_matches(".desktop").to_lowercase() == id
                || (!a.startup_wm_class.is_empty() && a.startup_wm_class.to_lowercase() == id)
        })
        .map(|a| a.name.clone())
        .unwrap_or_else(|| window.app_id.clone());
    let clean = |text: &str, max: usize| -> String {
        text.chars()
            .filter(|c| !c.is_control())
            .take(max)
            .collect::<String>()
            .trim()
            .to_string()
    };
    ActiveWindow {
        app: clean(&name, 60),
        title: clean(&window.title, 120),
    }
}

#[tauri::command]
pub fn active_window_view(app: AppHandle) -> ActiveWindow {
    active_window(&app)
}

/// Tells the page which window has the focus, but only when it changed (the event stream is chatty).
fn publish_active_window(app: &AppHandle) {
    static LAST: std::sync::Mutex<Option<ActiveWindow>> = std::sync::Mutex::new(None);
    let now = active_window(app);
    let mut last = LAST.lock().expect("active window");
    if last.as_ref() != Some(&now) {
        eprintln!("ACTIVE-WINDOW app={:?} title={:?}", now.app, now.title);
        *last = Some(now.clone());
        let _ = app.emit("active-window", now);
    }
}

/// Tells the page the workspace strip, but only when it differs from what it was last told (the event stream is chatty).
fn publish_workspaces(app: &AppHandle) {
    static LAST: std::sync::Mutex<Vec<helix_compositor::niri::WorkspaceView>> =
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
            .unwrap_or_else(|| helix_shell_model::dock::Dock::default_pins(&guard.dock_apps));
        guard.dock = helix_shell_model::dock::Dock::new(pins);
    }
    refresh(&app);
    apply_reserve(&app, &prefs());
    watch_prefs(app.clone());
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
    action: Result<Action, helix_shell_model::dock::RequestError>,
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
        &mut helix_shell_model::dock::Dock,
    ) -> Result<(), helix_shell_model::dock::RequestError>,
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
pub fn workspaces_view(app: AppHandle) -> Vec<helix_compositor::niri::WorkspaceView> {
    shell_state(&app)
        .lock()
        .expect("shell state")
        .niri
        .workspace_strip()
}

/// Focus the Helix agent desk workspace (helix-niri).
#[tauri::command]
pub fn agent_desk_focus() -> Result<(), String> {
    helix_compositor::compositor_ipc::focus_agent_desk()
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
