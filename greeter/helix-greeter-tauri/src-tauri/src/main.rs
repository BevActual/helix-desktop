//! Helix Greeter — the greetd login UI. Rust talks to greetd and decides what may be offered; the page (React) draws the login card
//! and sends back only a typed answer, a login name and a session id it was given. See `greeter/HELIX.md`.

mod greetd;
mod system;

use greetd::{Conversation, Step};
use serde::Serialize;
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::sync::Mutex;
use system::{Session, User};
use tauri::{Manager, State, WebviewUrl, WebviewWindowBuilder};

const WINDOW: &str = "greeter";
const SESSIONS: &str = "/usr/share/wayland-sessions";

#[derive(Default)]
struct Login {
    talk: Mutex<Option<Conversation<UnixStream>>>,
    /// The user chosen for this attempt; sessions start only for an account that authenticated.
    user: Mutex<Option<String>>,
}

#[derive(Serialize)]
struct Offer {
    users: Vec<User>,
    sessions: Vec<Session>,
    /// Appearance the greeter was given by the shell (dark or light); defaults to dark.
    dark: bool,
}

fn known_users() -> Vec<User> {
    std::fs::read_to_string("/etc/passwd").map(|text| system::users_from_passwd(&text)).unwrap_or_default()
}

#[tauri::command]
fn greeter_offer() -> Offer {
    let dark = std::fs::read_to_string("/var/lib/helix-greeter/appearance.json")
        .ok()
        .and_then(|text| serde_json::from_str::<serde_json::Value>(&text).ok())
        .and_then(|value| value.get("dark").and_then(serde_json::Value::as_bool))
        .unwrap_or(true);
    Offer {
        users: known_users(),
        sessions: system::sessions_in(Path::new(SESSIONS)).into_iter().map(|(session, _)| session).collect(),
        dark,
    }
}

fn connect() -> Result<Conversation<UnixStream>, String> {
    let path = std::env::var_os("GREETD_SOCK").ok_or("The login service is not running.")?;
    UnixStream::connect(path).map(Conversation::new).map_err(|error| format!("Cannot reach the login service: {error}"))
}

fn finish(login: &Login, step: Result<Step, std::io::Error>) -> Result<Step, String> {
    let step = step.map_err(|error| format!("The login service stopped answering: {error}"))?;
    if matches!(step, Step::Failed { .. }) {
        // greetd ended the attempt; the next one opens a new conversation.
        *login.talk.lock().expect("login") = None;
        *login.user.lock().expect("login") = None;
    }
    Ok(step)
}

#[tauri::command]
fn greeter_begin(login: State<Login>, username: String) -> Result<Step, String> {
    // Only an account the greeter itself listed can be tried.
    if !known_users().iter().any(|user| user.username == username) {
        return Err("Unknown account.".into());
    }
    let mut talk = connect()?;
    let step = talk.begin(&username);
    *login.talk.lock().expect("login") = Some(talk);
    *login.user.lock().expect("login") = Some(username);
    finish(&login, step)
}

#[tauri::command]
fn greeter_respond(login: State<Login>, response: Option<String>) -> Result<Step, String> {
    let step = {
        let mut guard = login.talk.lock().expect("login");
        let talk = guard.as_mut().ok_or("Choose an account first.")?;
        talk.respond(response.as_deref())
    };
    finish(&login, step)
}

#[tauri::command]
fn greeter_cancel(login: State<Login>) {
    if let Some(mut talk) = login.talk.lock().expect("login").take() {
        let _ = talk.cancel();
    }
    *login.user.lock().expect("login") = None;
}

#[tauri::command]
fn greeter_start(app: tauri::AppHandle, login: State<Login>, session: String) -> Result<(), String> {
    let (_, cmd) = system::sessions_in(Path::new(SESSIONS))
        .into_iter()
        .find(|(offered, _)| offered.id == session)
        .ok_or("That session is not available.")?;
    let mut guard = login.talk.lock().expect("login");
    let talk = guard.as_mut().ok_or("Sign in first.")?;
    match talk.start(&cmd).map_err(|error| error.to_string())? {
        Ok(()) => {
            // greetd now ends this greeter and starts the session.
            app.exit(0);
            Ok(())
        }
        Err(message) => Err(message),
    }
}

fn main() {
    tauri::Builder::default()
        .manage(Login::default())
        .invoke_handler(tauri::generate_handler![greeter_offer, greeter_begin, greeter_respond, greeter_cancel, greeter_start])
        .setup(|app| {
            WebviewWindowBuilder::new(app, WINDOW, WebviewUrl::App("index.html".into()))
                .title("Helix")
                .fullscreen(true)
                .decorations(false)
                .build()?;
            let _ = app.handle().get_webview_window(WINDOW);
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("helix-greeter");
}
