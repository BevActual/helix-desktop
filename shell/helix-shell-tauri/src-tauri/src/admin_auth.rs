//! Graphical sudo / admin password prompt for `SUDO_ASKPASS` (via `tools/helix-askpass`).

use std::sync::Mutex;

use gtk::prelude::*;
use gtk_layer_shell::{Edge, KeyboardMode, Layer, LayerShell};
use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager, WebviewUrl, WebviewWindowBuilder};
use tokio::sync::oneshot;

pub const WINDOW: &str = "admin-auth";

struct Pending {
    reply: oneshot::Sender<Result<String, String>>,
}

static PENDING: Mutex<Option<Pending>> = Mutex::new(None);

#[derive(Clone, Serialize)]
pub struct AdminAuthView {
    pub prompt: String,
}

fn ensure_window(app: &AppHandle) -> Result<(), String> {
    if app.get_webview_window(WINDOW).is_some() {
        return Ok(());
    }
    let window = WebviewWindowBuilder::new(app, WINDOW, WebviewUrl::App("admin-auth.html".into()))
        .title("Administrator authentication")
        .inner_size(480.0, 220.0)
        .resizable(false)
        .decorations(false)
        .transparent(true)
        .always_on_top(true)
        .skip_taskbar(true)
        .visible(false)
        .build()
        .map_err(|e| e.to_string())?;
    setup_overlay(&window)?;
    Ok(())
}

fn setup_overlay(window: &tauri::WebviewWindow) -> Result<(), String> {
    let gtk_window = window.gtk_window().map_err(|e| e.to_string())?;
    gtk_window.set_size_request(1, 1);
    gtk_window.init_layer_shell();
    gtk_window.set_namespace("helix-tauri-admin-auth");
    gtk_window.set_layer(Layer::Overlay);
    for edge in [Edge::Top, Edge::Bottom, Edge::Left, Edge::Right] {
        gtk_window.set_anchor(edge, true);
    }
    gtk_window.set_keyboard_mode(KeyboardMode::Exclusive);
    gtk_window.set_exclusive_zone(0);
    Ok(())
}

fn finish(result: Result<String, String>) {
    let pending = PENDING.lock().expect("admin auth").take();
    if let Some(p) = pending {
        let _ = p.reply.send(result);
    }
}

fn hide(app: &AppHandle) {
    if let Some(window) = app.get_webview_window(WINDOW) {
        let _ = window.hide();
    }
}

/// Blocks the D-Bus caller until the person submits or cancels.
pub async fn prompt(app: AppHandle, prompt: String) -> Result<String, String> {
    let trimmed = prompt.trim();
    if trimmed.is_empty() {
        return Err("empty prompt".into());
    }
    if trimmed.chars().count() > 500 {
        return Err("prompt is too long".into());
    }
    if PENDING.lock().expect("admin auth").is_some() {
        return Err("another authentication is already in progress".into());
    }
    let (tx, rx) = oneshot::channel();
    PENDING.lock().expect("admin auth").replace(Pending { reply: tx });
    let app2 = app.clone();
    let text = trimmed.to_string();
    let shown = tauri::async_runtime::spawn_blocking(move || {
        let app_main = app2.clone();
        app2.run_on_main_thread(move || {
            if let Err(error) = ensure_window(&app_main) {
                finish(Err(error));
                return;
            }
            let view = AdminAuthView { prompt: text };
            if let Some(window) = app_main.get_webview_window(WINDOW) {
                let _ = window.emit("admin-auth-view", &view);
                let _ = window.show();
                let _ = window.set_focus();
            } else {
                finish(Err("could not open the authentication window".into()));
            }
        })
    });
    let _ = shown.await;
    match rx.await {
        Ok(result) => result,
        Err(_) => Err("authentication was interrupted".into()),
    }
}

#[tauri::command]
pub fn admin_auth_submit(app: AppHandle, password: String) -> Result<(), String> {
    if password.is_empty() {
        return Err("password is empty".into());
    }
    if password.chars().any(|c| c.is_control()) {
        return Err("invalid password".into());
    }
    hide(&app);
    finish(Ok(password));
    Ok(())
}

#[tauri::command]
pub fn admin_auth_cancel(app: AppHandle) -> Result<(), String> {
    hide(&app);
    finish(Err("cancelled".into()));
    Ok(())
}
