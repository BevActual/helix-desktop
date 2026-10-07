// A bar and a panel as layer-shell surfaces, each a Tauri webview. The panel drives helix-assistant exactly like the native one.
use gtk::prelude::*;
use gtk_layer_shell::{Edge, KeyboardMode, Layer, LayerShell};
use std::sync::OnceLock;
use std::time::Instant;
use tauri::{Manager, WebviewUrl, WebviewWindowBuilder};

static STARTED: OnceLock<Instant> = OnceLock::new();

/// Runs helix-assistant and returns its stdout (one JSON object). The UI never sees the model, only this.
#[tauri::command]
async fn assistant(args: Vec<String>) -> Result<String, String> {
    tauri::async_runtime::spawn_blocking(move || {
        std::process::Command::new("helix-assistant")
            .args(args)
            .output()
            .map(|out| String::from_utf8_lossy(&out.stdout).into_owned())
            .map_err(|e| format!("could not start helix-assistant: {e}"))
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
fn toggle_panel(app: tauri::AppHandle) {
    if let Some(panel) = app.get_webview_window("panel") {
        if panel.is_visible().unwrap_or(false) {
            let _ = panel.hide();
        } else {
            let _ = panel.show();
            let _ = panel.set_focus();
        }
    }
}

/// Lets the pages report when they first rendered, so startup time can be measured from outside.
#[tauri::command]
fn ready(window: tauri::Window) {
    if let Some(started) = STARTED.get() {
        eprintln!("READY {} after {} ms", window.label(), started.elapsed().as_millis());
    }
}

fn main() {
    let _ = STARTED.set(Instant::now());
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![assistant, toggle_panel, ready])
        .setup(|app| {
            // The bar: top edge, full width, reserves its own space like a real bar.
            let bar = WebviewWindowBuilder::new(app, "bar", WebviewUrl::App("bar.html".into()))
                .decorations(false)
                .transparent(true)
                .visible(false)
                .inner_size(1280.0, 34.0)
                .build()?;
            let gtk_bar = bar.gtk_window()?;
            gtk_bar.set_size_request(-1, 42);  // layer surfaces take their size from the widget, not from the window config
            gtk_bar.init_layer_shell();
            gtk_bar.set_layer(Layer::Top);
            gtk_bar.set_namespace("helix-tauri-bar");
            for edge in [Edge::Top, Edge::Left, Edge::Right] {
                gtk_bar.set_anchor(edge, true);
            }
            gtk_bar.auto_exclusive_zone_enable();
            gtk_bar.set_keyboard_mode(KeyboardMode::None);
            bar.show()?;

            // The panel: overlay, centred (no anchors), takes the keyboard while open.
            let panel = WebviewWindowBuilder::new(app, "panel", WebviewUrl::App("panel.html".into()))
                .decorations(false)
                .transparent(true)
                .visible(false)
                .inner_size(520.0, 260.0)
                .build()?;
            let gtk_panel = panel.gtk_window()?;
            gtk_panel.set_size_request(520, 260);
            gtk_panel.init_layer_shell();
            gtk_panel.set_layer(Layer::Overlay);
            gtk_panel.set_namespace("helix-tauri-panel");
            gtk_panel.set_keyboard_mode(KeyboardMode::Exclusive);

            // Test hook: open the panel after N seconds, so memory can be measured without clicking.
            if let Some(seconds) = std::env::var("HELIX_SPIKE_AUTO_PANEL").ok().and_then(|v| v.parse::<u64>().ok()) {
                let handle = app.handle().clone();
                std::thread::spawn(move || {
                    std::thread::sleep(std::time::Duration::from_secs(seconds));
                    if let Some(panel) = handle.get_webview_window("panel") {
                        let _ = panel.show();
                    }
                });
            }
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running the spike");
}
