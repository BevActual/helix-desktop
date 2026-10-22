//! The agent display, from the shell's side: starts and stops the headless compositor that only an agent's programs run on, launches an installed
//! application onto it, and takes the picture the viewer shows. What the display is, and what a program on it may inherit, is
//! `helix_agent::agent_display`; the design is `docs/design/computer-use.md`.

use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use helix_agent::agent_display::{
    compositor_env, config_kdl, parse_started, places, program_env, Places, Started, REMOVE,
};
use helix_compositor::compositor_ipc::request_json_at;
use serde::Serialize;
use serde_json::{json, Value};
use tauri::{AppHandle, Emitter};

use crate::apps;

/// The display's hands: the input helper process, written to line by line and answering line by line.
struct Hands {
    child: Child,
    stdin: std::process::ChildStdin,
    answers: std::sync::mpsc::Receiver<String>,
}

struct Running {
    niri: Child,
    bus: Child,
    hands: Option<Hands>,
    started: Started,
    places: Places,
}

static RUNNING: Mutex<Option<Running>> = Mutex::new(None);

/// The person has the display's mouse and keyboard (in the viewer). Everything that would act for the agent checks this and waits: a person's hand on the
/// display always wins.
static PERSON_CONTROLLING: std::sync::atomic::AtomicBool =
    std::sync::atomic::AtomicBool::new(false);

pub fn person_controlling() -> bool {
    PERSON_CONTROLLING.load(std::sync::atomic::Ordering::SeqCst)
}

#[derive(Clone, Debug, Default, Serialize, PartialEq, Eq)]
pub struct WindowFact {
    pub id: u64,
    pub app_id: String,
    pub title: String,
}

#[derive(Clone, Debug, Default, Serialize, PartialEq, Eq)]
pub struct Status {
    pub running: bool,
    pub windows: Vec<WindowFact>,
}

fn person_runtime() -> Result<PathBuf, String> {
    std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .ok_or_else(|| "no runtime directory".to_string())
}

fn data_home() -> Result<PathBuf, String> {
    std::env::var_os("XDG_DATA_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| Path::new(&h).join(".local/share")))
        .ok_or_else(|| "no data directory".to_string())
}

fn make_private(dir: &Path) -> Result<(), String> {
    use std::os::unix::fs::PermissionsExt;
    std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    std::fs::set_permissions(dir, std::fs::Permissions::from_mode(0o700)).map_err(|e| e.to_string())
}

fn clean(command: &mut Command) {
    apps::scrub_shell_env(command);
    for name in REMOVE {
        command.env_remove(name);
    }
}

/// Starts the display if it is not running. Returns once its compositor says where it listens.
pub fn start(app: &AppHandle) -> Result<Status, String> {
    {
        let guard = RUNNING.lock().expect("agent display");
        if guard.is_some() {
            drop(guard);
            return Ok(status());
        }
    }
    let places = places(&person_runtime()?, &data_home()?);
    make_private(&places.runtime)?;
    std::fs::create_dir_all(&places.shots).map_err(|e| e.to_string())?;
    std::fs::create_dir_all(places.home.join(".config")).map_err(|e| e.to_string())?;
    let kdl = places.runtime.join("niri.kdl");
    std::fs::write(&kdl, config_kdl(&places.shots.to_string_lossy())).map_err(|e| e.to_string())?;
    let _ = std::fs::remove_file(&places.bus);

    // The display's own session bus, so nothing on it can talk to the person's.
    let mut bus_command = Command::new("dbus-daemon");
    clean(&mut bus_command);
    let mut bus = bus_command
        .args([
            "--session",
            "--nofork",
            &format!("--address=unix:path={}", places.bus.display()),
        ])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|e| format!("cannot start the display's session bus: {e}"))?;
    let until = Instant::now() + Duration::from_secs(5);
    while !places.bus.exists() && Instant::now() < until {
        std::thread::sleep(Duration::from_millis(50));
    }
    if !places.bus.exists() {
        let _ = bus.kill();
        return Err("the display's session bus did not start".into());
    }

    let log_path = places.runtime.join("niri.log");
    let log = std::fs::File::create(&log_path).map_err(|e| e.to_string())?;
    let niri_program = std::env::var("HELIX_AGENT_DISPLAY_NIRI").unwrap_or_else(|_| "niri".into());
    let mut niri_command = Command::new(niri_program);
    clean(&mut niri_command);
    niri_command.env_remove("WAYLAND_DISPLAY");
    for (key, value) in compositor_env(&places) {
        niri_command.env(key, value);
    }
    let mut niri = niri_command
        .arg("-c")
        .arg(&kdl)
        .stdin(Stdio::null())
        .stdout(log.try_clone().map_err(|e| e.to_string())?)
        .stderr(log)
        .spawn()
        .map_err(|e| format!("cannot start the display's compositor: {e}"))?;
    let until = Instant::now() + Duration::from_secs(15);
    let started = loop {
        if let Some(found) = std::fs::read_to_string(&log_path)
            .ok()
            .and_then(|text| parse_started(&text))
        {
            break found;
        }
        if niri.try_wait().ok().flatten().is_some() || Instant::now() > until {
            let _ = niri.kill();
            let _ = bus.kill();
            return Err("the display's compositor did not start".into());
        }
        std::thread::sleep(Duration::from_millis(100));
    };
    eprintln!("AGENT-DISPLAY started on {}", started.wayland_display);
    let hands = start_hands(&places, &started);
    *RUNNING.lock().expect("agent display") = Some(Running {
        niri,
        bus,
        hands,
        started,
        places,
    });
    watch(app.clone());
    Ok(status())
}

/// Starts the input helper for this display. Without it the display can still be watched; it just cannot be driven.
fn start_hands(places: &Places, started: &Started) -> Option<Hands> {
    let program = std::env::var("HELIX_AGENT_INPUT").unwrap_or_else(|_| "helix-agent-input".into());
    let mut command = Command::new(program);
    clean(&mut command);
    for (key, value) in program_env(places, &started.wayland_display) {
        command.env(key, value);
    }
    let mut child = command
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .ok()?;
    let stdin = child.stdin.take()?;
    let stdout = child.stdout.take()?;
    let (sender, answers) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        use std::io::BufRead;
        for line in std::io::BufReader::new(stdout)
            .lines()
            .map_while(Result::ok)
        {
            if sender.send(line).is_err() {
                return;
            }
        }
    });
    let hands = Hands {
        child,
        stdin,
        answers,
    };
    // The first line is `ready` once it is connected to the display.
    match hands.answers.recv_timeout(Duration::from_secs(5)) {
        Ok(line) if line == "ready" => Some(hands),
        _ => {
            let mut hands = hands;
            let _ = hands.child.kill();
            None
        }
    }
}

/// Sends one command (a JSON object, checked by the helper) to the display's hands and waits for its answer.
pub fn input(command: &str) -> Result<(), String> {
    use std::io::Write;
    if command.len() > 64 * 1024 || command.contains('\n') {
        return Err("that is not one command".into());
    }
    let mut guard = RUNNING.lock().expect("agent display");
    let running = guard.as_mut().ok_or("the agent display is not running")?;
    let hands = running
        .hands
        .as_mut()
        .ok_or("the display has no input helper")?;
    while hands.answers.try_recv().is_ok() {}
    writeln!(hands.stdin, "{command}").map_err(|e| format!("the input helper is gone: {e}"))?;
    match hands.answers.recv_timeout(Duration::from_secs(10)) {
        Ok(line) if line == "ok" => Ok(()),
        Ok(line) => Err(line.strip_prefix("error: ").unwrap_or(&line).to_string()),
        Err(_) => Err("the input helper did not answer".into()),
    }
}

/// A command from the agent: refused while the person has the display. (The viewer's own input, `input`, is the person's and is never refused.)
pub fn agent_input(command: &str) -> Result<(), String> {
    if person_controlling() {
        return Err("the person is in control of the display".into());
    }
    input(command)
}

/// Stops the display; every program on it goes with it.
pub fn stop() {
    if let Some(mut running) = RUNNING.lock().expect("agent display").take() {
        if let Some(mut hands) = running.hands.take() {
            let _ = hands.child.kill();
            let _ = hands.child.wait();
        }
        let _ = running.niri.kill();
        let _ = running.niri.wait();
        let _ = running.bus.kill();
        let _ = running.bus.wait();
        let _ = std::fs::remove_file(&running.places.bus);
        eprintln!("AGENT-DISPLAY stopped");
    }
}

/// Whether it is running, and what is on it.
pub fn status() -> Status {
    let mut guard = RUNNING.lock().expect("agent display");
    let Some(running) = guard.as_mut() else {
        return Status::default();
    };
    if running.niri.try_wait().ok().flatten().is_some() {
        // It died: forget it, so the next start makes a new one.
        *guard = None;
        return Status::default();
    }
    let socket = PathBuf::from(&running.started.ipc_socket);
    drop(guard);
    let windows = request_json_at(&socket, "\"Windows\"")
        .and_then(|reply| reply.get("Windows").cloned())
        .and_then(|list| list.as_array().cloned())
        .unwrap_or_default()
        .iter()
        .filter_map(|w| {
            Some(WindowFact {
                id: w.get("id")?.as_u64()?,
                app_id: helix_shell_model::display::sanitize_for_display(
                    w.get("app_id").and_then(Value::as_str).unwrap_or(""),
                    60,
                ),
                title: helix_shell_model::display::sanitize_for_display(
                    w.get("title").and_then(Value::as_str).unwrap_or(""),
                    80,
                ),
            })
        })
        .collect();
    Status {
        running: true,
        windows,
    }
}

/// How many windows are on the display, or `None` when it is not running (the island says nothing then).
pub fn window_count() -> Option<usize> {
    let status = status();
    status.running.then_some(status.windows.len())
}

/// Looks at the display once a second while it runs and tells the island and the viewer when what is on it changes.
fn watch(app: AppHandle) {
    std::thread::spawn(move || {
        let mut last: Option<Status> = None;
        loop {
            std::thread::sleep(Duration::from_secs(1));
            let now = status();
            if last.as_ref() != Some(&now) {
                last = Some(now.clone());
                crate::island::publish(&app);
                let _ = app.emit("agent-display-status", now.clone());
            }
            if !now.running {
                return;
            }
        }
    });
}

/// Starts an installed application (by desktop id, like the launcher) on the display, never on the person's desktop.
pub fn launch(id: &str) -> Result<(), String> {
    let (places, display) = {
        let guard = RUNNING.lock().expect("agent display");
        let running = guard.as_ref().ok_or("the agent display is not running")?;
        (
            running.places.clone(),
            running.started.wayland_display.clone(),
        )
    };
    let app = apps::scan()
        .into_iter()
        .find(|a| a.id == id)
        .ok_or("that application is not installed")?;
    let (program, args) = app.argv.split_first().ok_or("the entry has no command")?;
    let mut command = Command::new(program);
    clean(&mut command);
    for (key, value) in program_env(&places, &display) {
        command.env(key, value);
    }
    command
        .args(args)
        .current_dir(&places.home)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    let mut child = command.spawn().map_err(|e| e.to_string())?;
    std::thread::spawn(move || {
        let _ = child.wait();
    });
    eprintln!("AGENT-DISPLAY launched {id}");
    Ok(())
}

/// A picture of the display as PNG bytes. The compositor writes it to the display's own folder; nothing is copied to the person's clipboard (the display
/// has its own) and no notification is shown to the person.
pub fn frame() -> Result<Vec<u8>, String> {
    let (socket, shots) = {
        let guard = RUNNING.lock().expect("agent display");
        let running = guard.as_ref().ok_or("the agent display is not running")?;
        (
            PathBuf::from(&running.started.ipc_socket),
            running.places.shots.clone(),
        )
    };
    let path = shots.join(format!(
        "frame-{}.png",
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0)
    ));
    let request = json!({"Action": {"ScreenshotScreen": {"write_to_disk": true, "show_pointer": true, "path": path.to_string_lossy()}}}).to_string();
    request_json_at(&socket, &request).ok_or("the display did not answer")?;
    // The file is written by a thread: wait until it exists and has stopped growing.
    let until = Instant::now() + Duration::from_secs(5);
    let mut last = 0u64;
    loop {
        let size = std::fs::metadata(&path).map(|m| m.len()).unwrap_or(0);
        if size > 0 && size == last {
            break;
        }
        last = size;
        if Instant::now() > until {
            return Err("the display did not produce a picture".into());
        }
        std::thread::sleep(Duration::from_millis(60));
    }
    let mut bytes = Vec::new();
    std::fs::File::open(&path)
        .and_then(|mut f| f.read_to_end(&mut bytes))
        .map_err(|e| e.to_string())?;
    let _ = std::fs::remove_file(&path);
    Ok(bytes)
}

#[tauri::command]
pub fn agent_display_status() -> Status {
    status()
}

#[tauri::command]
pub async fn agent_display_start(app: AppHandle) -> Result<Status, String> {
    tauri::async_runtime::spawn_blocking(move || start(&app))
        .await
        .map_err(|e| e.to_string())?
}

/// One command for the display's hands: a move, a button, a scroll, a key or some text. The helper checks it.
#[tauri::command]
pub async fn agent_display_input(command: serde_json::Value) -> Result<(), String> {
    let line = command.to_string();
    tauri::async_runtime::spawn_blocking(move || input(&line))
        .await
        .map_err(|e| e.to_string())?
}

/// The viewer says the person has taken over (or handed back).
#[tauri::command]
pub fn agent_display_control(app: AppHandle, on: bool) {
    PERSON_CONTROLLING.store(on, std::sync::atomic::Ordering::SeqCst);
    eprintln!(
        "AGENT-DISPLAY person {}",
        if on { "in control" } else { "handed back" }
    );
    let _ = app.emit("agent-display-control", on);
}

#[tauri::command]
pub fn agent_display_stop(app: AppHandle) {
    stop();
    crate::island::publish(&app);
    let _ = app.emit("agent-display-status", status());
}

#[tauri::command]
pub async fn agent_display_launch(id: String) -> Result<(), String> {
    tauri::async_runtime::spawn_blocking(move || launch(&id))
        .await
        .map_err(|e| e.to_string())?
}

/// One picture of the display as a `data:` URL, for the viewer.
#[tauri::command]
pub async fn agent_display_frame() -> Result<String, String> {
    use base64::Engine;
    static SERVED: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let n = SERVED.fetch_add(1, std::sync::atomic::Ordering::Relaxed) + 1;
    if n == 1 || n.is_multiple_of(50) {
        eprintln!("AGENT-DISPLAY viewer frame {n}");
    }
    let bytes = tauri::async_runtime::spawn_blocking(frame)
        .await
        .map_err(|e| e.to_string())??;
    Ok(format!(
        "data:image/png;base64,{}",
        base64::engine::general_purpose::STANDARD.encode(bytes)
    ))
}
