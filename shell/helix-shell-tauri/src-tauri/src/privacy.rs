//! The microphone and camera indicator's data: who is capturing right now. PipeWire is followed through `pw-dump --monitor` (one client that prints
//! changes, so nothing is polled); programs that open a camera device directly are found by looking at `/proc` every few seconds, and only when the
//! computer has a camera at all. The page draws the result; this decides nothing about what is allowed.

use std::io::BufReader;
use std::process::{Command, Stdio};
use std::sync::Mutex;
use std::time::Duration;

use helix_shell_model::privacy::{camera_devices, video_users, Capture, Nodes};
use serde::Serialize;
use tauri::{AppHandle, Emitter};

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize)]
pub struct View {
    pub microphone: Vec<String>,
    pub camera: Vec<String>,
}

#[derive(Default)]
struct State {
    pipewire: Capture,
    direct_camera: Vec<String>,
    shown: View,
}

static STATE: Mutex<Option<State>> = Mutex::new(None);

fn with_state<T>(f: impl FnOnce(&mut State) -> T) -> T {
    let mut guard = STATE.lock().expect("privacy state");
    f(guard.get_or_insert_with(State::default))
}

/// Puts the two sources together; `Some` when what the bar shows has changed.
fn merge(state: &mut State) -> Option<View> {
    let mut camera = state.pipewire.camera.clone();
    for name in &state.direct_camera {
        if !camera.contains(name) {
            camera.push(name.clone());
        }
    }
    camera.sort();
    let view = View {
        microphone: state.pipewire.microphone.clone(),
        camera,
    };
    (view != state.shown).then(|| {
        state.shown = view.clone();
        view
    })
}

fn publish(app: &AppHandle, view: View) {
    eprintln!("PRIVACY mic={:?} camera={:?}", view.microphone, view.camera);
    let _ = app.emit("privacy-view", view);
}

#[tauri::command]
pub fn privacy_view() -> View {
    with_state(|s| s.shown.clone())
}

/// Follows PipeWire for as long as the shell runs, starting again if it goes away (or is not running yet).
fn follow_pipewire(app: AppHandle) {
    std::thread::spawn(move || {
        let program = std::env::var("HELIX_PW_DUMP").unwrap_or_else(|_| "pw-dump".into());
        loop {
            let child = Command::new(&program)
                .arg("--monitor")
                .arg("--no-colors")
                .stdin(Stdio::null())
                .stdout(Stdio::piped())
                .stderr(Stdio::null())
                .spawn();
            if let Ok(mut child) = child {
                if let Some(out) = child.stdout.take() {
                    let mut nodes = Nodes::default();
                    // The monitor prints one JSON array per batch of changes, one after another.
                    for batch in serde_json::Deserializer::from_reader(BufReader::new(out))
                        .into_iter::<serde_json::Value>()
                    {
                        let Ok(serde_json::Value::Array(objects)) = batch else {
                            break;
                        };
                        if nodes.apply(&objects) {
                            let next = with_state(|s| {
                                s.pipewire = nodes.capture();
                                merge(s)
                            });
                            if let Some(view) = next {
                                publish(&app, view);
                            }
                        }
                    }
                }
                let _ = child.kill();
                let _ = child.wait();
            }
            // PipeWire gone: nothing is being captured through it.
            let next = with_state(|s| {
                s.pipewire = Capture::default();
                merge(s)
            });
            if let Some(view) = next {
                publish(&app, view);
            }
            std::thread::sleep(Duration::from_secs(5));
        }
    });
}

/// Looks for programs holding a camera device open, only while a camera exists.
fn follow_devices(app: AppHandle) {
    std::thread::spawn(move || {
        let dev = std::env::var_os("HELIX_PRIVACY_DEV")
            .map_or_else(|| "/dev".into(), std::path::PathBuf::from);
        let proc = std::env::var_os("HELIX_PRIVACY_PROC")
            .map_or_else(|| "/proc".into(), std::path::PathBuf::from);
        loop {
            let devices = camera_devices(&dev);
            let users = if devices.is_empty() {
                Vec::new()
            } else {
                video_users(&proc, &devices, std::process::id())
            };
            let next = with_state(|s| {
                s.direct_camera = users;
                merge(s)
            });
            if let Some(view) = next {
                publish(&app, view);
            }
            std::thread::sleep(Duration::from_secs(3));
        }
    });
}

pub fn start(app: AppHandle) {
    follow_pipewire(app.clone());
    follow_devices(app);
}
