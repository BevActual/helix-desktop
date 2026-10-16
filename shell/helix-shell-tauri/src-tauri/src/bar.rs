//! Show / hide the painted bar and sync the external top strut exclusive zone.

use helix_shell_model::bar_visibility::{TopStrutState, write as write_strut};
use tauri::{AppHandle, Manager};

use crate::control::on_main;
use crate::layer::WINDOW;

pub fn is_visible() -> bool {
    helix_shell_model::bar_visibility::read().is_visible()
}

pub fn set_visible(app: &AppHandle, visible: bool) -> Result<(), String> {
    let state = if visible {
        TopStrutState::visible()
    } else {
        TopStrutState::hidden()
    };
    write_strut(&state)?;
    let zone = state.exclusive_zone;
    on_main(app, move |app| {
        if !crate::strut::strut_unit_active() {
            crate::layer::set_top_reserve_zone(zone);
        }
        let Some(window) = app.get_webview_window(WINDOW) else {
            return;
        };
        if visible {
            let _ = window.show();
        } else {
            let _ = window.hide();
            crate::close_panel(app);
        }
    })
    .map_err(|e| e.to_string())?;
    eprintln!(
        "BAR {}",
        if visible { "shown" } else { "hidden (strut released)" }
    );
    Ok(())
}

pub fn toggle(app: &AppHandle) -> Result<(), String> {
    set_visible(app, !is_visible())
}
