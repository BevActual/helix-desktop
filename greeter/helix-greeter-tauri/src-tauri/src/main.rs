//! Helix Greeter — the greetd login UI. Rust talks to greetd and decides what may be offered; the page (React) draws the login card
//! and sends back only a typed answer, a login name and a session id it was given. See `greeter/HELIX.md`.

mod appearance;
mod greetd;
mod power;
mod remember;
mod system;

use appearance::Appearance;
use greetd::{Conversation, Step};
use serde::Serialize;
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::sync::Mutex;
use system::{Session, User};
use tauri::{AppHandle, Manager, State, WebviewUrl, WebviewWindowBuilder};

const WINDOW: &str = "greeter";
const SESSIONS: &str = "/usr/share/wayland-sessions";

#[derive(Default)]
struct Login {
    talk: Mutex<Option<Conversation<UnixStream>>>,
    /// The user chosen for this attempt; sessions start only for an account that authenticated.
    user: Mutex<Option<String>>,
}

#[derive(Serialize)]
struct OfferColors {
    background: String,
    on_background: String,
    surface: String,
    on_surface: String,
    primary: String,
    on_primary: String,
}

#[derive(Serialize)]
struct Offer {
    users: Vec<User>,
    sessions: Vec<Session>,
    /// Appearance the shell synced for the login screen. Defaults to a dark card when nothing was synced.
    dark: bool,
    font: String,
    ui_scale: f64,
    corner_radius_scale: f64,
    transparency_mode: String,
    refraction: bool,
    high_contrast: bool,
    reduced_motion: bool,
    colors: Option<OfferColors>,
    /// The account and session the last login used, when this screen lists them.
    last_user: String,
    last_session: String,
    /// Asset URL of the synced wallpaper, empty when there is no image the page may load.
    wallpaper_url: String,
}

fn known_users() -> Vec<User> {
    std::fs::read_to_string("/etc/passwd")
        .map(|text| system::users_from_passwd(&text))
        .unwrap_or_default()
}

fn offer_from(app: &AppHandle, appearance: &Appearance) -> Offer {
    let wallpaper_url = appearance
        .wallpaper
        .as_ref()
        .and_then(|path| {
            let _ = app
                .asset_protocol_scope()
                .allow_directory(path.parent().unwrap_or(path), true);
            app.get_webview_window(WINDOW)
                .and_then(|window| window.convert_file_src(path, None).ok())
        })
        .unwrap_or_default();
    let users = known_users();
    let sessions: Vec<Session> = system::sessions_in(Path::new(SESSIONS))
        .into_iter()
        .map(|(session, _)| session)
        .collect();
    let last = remember::load();
    let last_user = users
        .iter()
        .find(|u| u.username == last.user)
        .map(|u| u.username.clone())
        .unwrap_or_default();
    let last_session = sessions
        .iter()
        .find(|s| s.id == last.session)
        .map(|s| s.id.clone())
        .unwrap_or_default();
    Offer {
        users,
        sessions,
        last_user,
        last_session,
        dark: appearance.dark,
        font: appearance.font.clone(),
        ui_scale: appearance.ui_scale,
        corner_radius_scale: appearance.corner_radius_scale,
        transparency_mode: appearance.transparency_mode.clone(),
        refraction: appearance.refraction && !appearance.high_contrast,
        high_contrast: appearance.high_contrast,
        reduced_motion: appearance.reduced_motion,
        colors: appearance.colors.as_ref().map(|colors| OfferColors {
            background: colors.background.clone(),
            on_background: colors.on_background.clone(),
            surface: colors.surface.clone(),
            on_surface: colors.on_surface.clone(),
            primary: colors.primary.clone(),
            on_primary: colors.on_primary.clone(),
        }),
        wallpaper_url,
    }
}

#[tauri::command]
fn greeter_offer(app: AppHandle) -> Offer {
    offer_from(&app, &appearance::load(&appearance::state_dir()))
}

fn connect() -> Result<Conversation<UnixStream>, String> {
    let path = std::env::var_os("GREETD_SOCK").ok_or("The login service is not running.")?;
    UnixStream::connect(path)
        .map(Conversation::new)
        .map_err(|error| format!("Cannot reach the login service: {error}"))
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
fn greeter_start(
    app: tauri::AppHandle,
    login: State<Login>,
    session: String,
) -> Result<(), String> {
    let (_, cmd) = system::sessions_in(Path::new(SESSIONS))
        .into_iter()
        .find(|(offered, _)| offered.id == session)
        .ok_or("That session is not available.")?;
    let mut guard = login.talk.lock().expect("login");
    let talk = guard.as_mut().ok_or("Sign in first.")?;
    match talk.start(&cmd).map_err(|error| error.to_string())? {
        Ok(()) => {
            if let Some(user) = login.user.lock().expect("login").clone() {
                remember::save(&remember::Last { user, session });
            }
            // greetd now ends this greeter and starts the session.
            app.exit(0);
            Ok(())
        }
        Err(message) => Err(message),
    }
}

#[tauri::command]
fn greeter_power(action: String) -> Result<(), String> {
    power::run(&action)
}

fn main() {
    tauri::Builder::default()
        .manage(Login::default())
        .invoke_handler(tauri::generate_handler![
            greeter_offer,
            greeter_begin,
            greeter_respond,
            greeter_cancel,
            greeter_start,
            greeter_power
        ])
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
