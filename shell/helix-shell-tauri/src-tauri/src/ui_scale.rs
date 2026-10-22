//! The person's interface scale (Settings → Accessibility) for the shell's own chrome. The page is zoomed like every other Helix window; what
//! zoom cannot reach is every size the compositor is told in pixels, so this also moves the bar's exclusive zone, the dock's, and the region that
//! takes the pointer, and tells the page (which multiplies what it reports back by the scale).

use std::sync::Mutex;

use helix_shell_model::ui_scale as model;
use tauri::{AppHandle, Emitter, Manager};

static CURRENT: Mutex<f64> = Mutex::new(1.0);

/// The scale in effect.
pub fn current() -> f64 {
    *CURRENT.lock().expect("ui scale")
}

/// `px` at the scale in effect.
pub fn scaled(px: i32) -> i32 {
    model::scaled(px, current())
}

#[tauri::command]
pub fn ui_scale() -> f64 {
    current()
}

/// Applies a scale now: zoom, the files the strut reads, the exclusive zones and the input region.
fn apply(app: &AppHandle, requested: f64) {
    let scale = model::clamp(requested);
    {
        let mut guard = CURRENT.lock().expect("ui scale");
        if (*guard - scale).abs() < f64::EPSILON {
            return;
        }
        *guard = scale;
    }
    eprintln!("SHELL ui scale {scale}");
    let _ = model::write(scale);
    // The window may not exist yet at start-up: try for a few seconds.
    let zoomer = app.clone();
    std::thread::spawn(move || {
        for _ in 0..40 {
            if let Some(window) = zoomer.get_webview_window(crate::layer::WINDOW) {
                let _ = window.set_zoom(scale);
                return;
            }
            std::thread::sleep(std::time::Duration::from_millis(250));
        }
    });
    // The top zone is written by whoever shows or hides the bar, so it is written again at the new size; the dock's follows the same file.
    if crate::bar::is_visible() {
        let _ = crate::bar::set_visible(app, true);
    }
    crate::dock::reapply_reserve(app);
    let refresh = app.clone();
    let _ = app.run_on_main_thread(move || crate::layer::refresh_input(&refresh));
    let _ = app.emit("ui-scale", scale);
}

/// Follows `accessibility.ui_scale` for as long as the shell runs.
pub fn start(app: AppHandle) {
    // What a previous run left is what the strut is using until the first answer comes.
    *CURRENT.lock().expect("ui scale") = model::read();
    tauri::async_runtime::spawn(async move {
        let Ok(desktop) = helix_desktop_api::Desktop::session().await else {
            return;
        };
        let _ = desktop
            .watch_ui_scale(move |scale| apply(&app, scale))
            .await;
    });
}
