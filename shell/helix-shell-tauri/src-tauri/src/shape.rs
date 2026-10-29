//! How round each part of the shell is, and how clear its glass is: the person's choices (Settings → Appearance) are in
//! `~/.config/helix/shape-settings.json`. This reads them, tells the page when they change and answers at start-up. The page puts the numbers on the root
//! element as custom properties (`--shape-bar`, `--shape-dock`, `--shape-toasts`, `--shape-ui` for the panels, `--glass-t`).

use helix_config::shape_prefs;
use tauri::{AppHandle, Emitter};

/// The numbers a page is given: (custom property, value) pairs.
fn vars(shape: &shape_prefs::Shape) -> Vec<(String, String)> {
    shape
        .css_vars("shell")
        .into_iter()
        .map(|(k, v)| (k.to_string(), v))
        .collect()
}

#[tauri::command]
pub fn shape_vars() -> Vec<(String, String)> {
    vars(&shape_prefs::read())
}

pub fn start(app: AppHandle) {
    shape_prefs::watch(move |shape| {
        eprintln!(
            "SHAPE bar={} dock={} panels={} toasts={} glass={}",
            shape.bar, shape.dock, shape.panels, shape.toasts, shape.glass
        );
        let _ = app.emit("shape-vars", vars(&shape));
    });
}
