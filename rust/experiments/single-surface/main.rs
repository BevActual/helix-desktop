// One layer-shell surface covering the whole output, hosting the bar AND the panel in a single page (one web process).
// The input region shrinks the surface to just what is visible, so clicks elsewhere pass through to windows beneath.
use gtk::prelude::*;
use gtk_layer_shell::{Edge, KeyboardMode, Layer, LayerShell};
use std::sync::OnceLock;
use std::time::Instant;
use tauri::{Manager, WebviewUrl, WebviewWindowBuilder};

static STARTED: OnceLock<Instant> = OnceLock::new();
const BAR_HEIGHT: i32 = 42;

fn set_input_region(window: &gtk::ApplicationWindow, rects: &[(i32, i32, i32, i32)]) {
    if let Some(gdk_window) = window.window() {
        let region = gtk::cairo::Region::create();
        for (x, y, w, h) in rects {
            let _ = region.union_rectangle(&gtk::cairo::RectangleInt::new(*x, *y, *w, *h));
        }
        gdk_window.input_shape_combine_region(&region, 0, 0);
    }
}

/// open = a panel is showing: take the keyboard, sit above fullscreen windows, catch clicks anywhere (to dismiss).
/// closed = only the bar is clickable, no keyboard, normal bar layer.
#[tauri::command]
fn set_open(app: tauri::AppHandle, open: bool) {
    let Some(w) = app.get_webview_window("shell") else { return };
    let Ok(gtk_win) = w.gtk_window() else { return };
    let width = gtk_win.allocated_width().max(1);
    let height = gtk_win.allocated_height().max(1);
    if open {
        gtk_win.set_layer(Layer::Overlay);
        gtk_win.set_keyboard_mode(KeyboardMode::Exclusive);
        set_input_region(&gtk_win, &[(0, 0, width, height)]);
    } else {
        gtk_win.set_layer(Layer::Top);
        gtk_win.set_keyboard_mode(KeyboardMode::None);
        set_input_region(&gtk_win, &[(0, 0, width, BAR_HEIGHT)]);
    }
}

#[tauri::command]
fn ready(window: tauri::Window) {
    if let Some(started) = STARTED.get() {
        eprintln!("READY {} after {} ms", window.label(), started.elapsed().as_millis());
    }
}

fn main() {
    let _ = STARTED.set(Instant::now());
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![set_open, ready])
        .setup(|app| {
            let win = WebviewWindowBuilder::new(app, "shell", WebviewUrl::App("index.html".into()))
                .decorations(false)
                .transparent(true)
                .visible(false)
                .inner_size(1280.0, 800.0)
                .build()?;
            let g = win.gtk_window()?;
            g.set_size_request(1, 1);
            g.init_layer_shell();
            g.set_layer(Layer::Top);
            g.set_namespace("helix-tauri-single");
            for edge in [Edge::Top, Edge::Left, Edge::Right, Edge::Bottom] {
                g.set_anchor(edge, true);
            }
            g.set_exclusive_zone(BAR_HEIGHT);
            g.set_keyboard_mode(KeyboardMode::None);
            win.show()?;
            let g2 = g.clone();
            gtk::glib::timeout_add_local_once(std::time::Duration::from_millis(400), move || {
                let w = g2.allocated_width().max(1);
                set_input_region(&g2, &[(0, 0, w, BAR_HEIGHT)]);
            });
            if let Some(secs) = std::env::var("HELIX_SPIKE_AUTO_PANEL").ok().and_then(|v| v.parse::<u64>().ok()) {
                let handle = app.handle().clone();
                std::thread::spawn(move || {
                    std::thread::sleep(std::time::Duration::from_secs(secs));
                    if let Some(w) = handle.get_webview_window("shell") {
                        let _ = w.eval("openPanel()");
                    }
                });
            }
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running");
}
