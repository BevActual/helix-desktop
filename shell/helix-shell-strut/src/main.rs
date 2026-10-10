//! Holds exclusive zones that must survive bar restart:
//! - top: `helix-shell-reserve` (bar strip)
//! - bottom: `helix-shell-dock-reserve` (when dock prefs reserve space)
//!
//! Painted bar / Overlay can die without flapping niri's work area (Electron/Cursor).

use std::cell::RefCell;
use std::path::PathBuf;
use std::rc::Rc;
use std::time::Duration;

use gtk::prelude::*;
use gtk_layer_shell::{Edge, LayerShell};
use helix_shell_core::dock_prefs::DockPrefs;
use helix_shell_core::BAR_HEIGHT;

fn dock_prefs_path() -> Option<PathBuf> {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")))?;
    Some(base.join("helix/dock-settings.json"))
}

fn read_dock_prefs() -> DockPrefs {
    dock_prefs_path()
        .and_then(|path| std::fs::read_to_string(path).ok())
        .map(|text| DockPrefs::parse(&text))
        .unwrap_or_default()
}

fn make_transparent(window: &gtk::Window) {
    window.set_decorated(false);
    window.set_accept_focus(false);
    window.set_app_paintable(true);
    if let Some(visual) =
        gtk::prelude::WidgetExt::screen(window).and_then(|screen| screen.rgba_visual())
    {
        window.set_visual(Some(&visual));
    }
    let css = gtk::CssProvider::new();
    if css
        .load_from_data(b"window, window * { background: transparent; }")
        .is_ok()
    {
        window
            .style_context()
            .add_provider(&css, gtk::STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
}

fn empty_input(window: &gtk::Window) {
    if let Some(gdk_window) = window.window() {
        gdk_window.input_shape_combine_region(&gtk::cairo::Region::create(), 0, 0);
    }
}

fn make_top_strut() -> gtk::Window {
    let window = gtk::Window::new(gtk::WindowType::Toplevel);
    make_transparent(&window);
    window.set_size_request(1, BAR_HEIGHT);
    window.init_layer_shell();
    window.set_namespace("helix-shell-reserve");
    window.set_layer(gtk_layer_shell::Layer::Top);
    for edge in [Edge::Top, Edge::Left, Edge::Right] {
        window.set_anchor(edge, true);
    }
    window.set_exclusive_zone(BAR_HEIGHT);
    window.set_keyboard_mode(gtk_layer_shell::KeyboardMode::None);
    window.show_all();
    empty_input(&window);
    window
}

struct DockStrut {
    window: Option<gtk::Window>,
    height: Option<i32>,
}

impl DockStrut {
    fn new() -> Self {
        Self {
            window: None,
            height: None,
        }
    }

    fn apply(&mut self, height: Option<i32>) {
        if self.height == height {
            return;
        }
        self.height = height;
        match height {
            None => {
                if let Some(window) = self.window.take() {
                    window.close();
                }
                eprintln!("STRUT dock reserve cleared");
            }
            Some(h) => {
                if let Some(window) = self.window.as_ref() {
                    window.set_size_request(1, h);
                    window.set_exclusive_zone(h);
                    eprintln!("STRUT dock reserve updated exclusive_zone={h}");
                    return;
                }
                let window = gtk::Window::new(gtk::WindowType::Toplevel);
                make_transparent(&window);
                window.set_size_request(1, h);
                window.init_layer_shell();
                window.set_namespace("helix-shell-dock-reserve");
                window.set_layer(gtk_layer_shell::Layer::Top);
                for edge in [Edge::Bottom, Edge::Left, Edge::Right] {
                    window.set_anchor(edge, true);
                }
                window.set_exclusive_zone(h);
                window.set_keyboard_mode(gtk_layer_shell::KeyboardMode::None);
                window.show_all();
                empty_input(&window);
                self.window = Some(window);
                eprintln!("STRUT dock reserve exclusive_zone={h}");
            }
        }
    }
}

fn main() {
    // Session start can race the Wayland display; retry instead of panic-looping the unit.
    let mut ok = false;
    for attempt in 1..=40 {
        if gtk::init().is_ok() {
            ok = true;
            break;
        }
        std::thread::sleep(Duration::from_millis(100 * attempt.min(10)));
    }
    if !ok {
        eprintln!("STRUT gtk init failed after retries (is WAYLAND_DISPLAY set?)");
        std::process::exit(1);
    }

    let top = make_top_strut();
    eprintln!("STRUT helix-shell-reserve exclusive_zone={BAR_HEIGHT} (survives bar restart)");

    let dock = Rc::new(RefCell::new(DockStrut::new()));
    dock.borrow_mut().apply(read_dock_prefs().reserved_height());

    // Poll dock prefs (stat); same cadence as the bar dock bridge.
    let dock_watch = dock.clone();
    let mut seen = dock_prefs_path()
        .and_then(|p| std::fs::metadata(p).ok())
        .and_then(|m| m.modified().ok());
    gtk::glib::timeout_add_local(Duration::from_secs(1), move || {
        let now = dock_prefs_path()
            .and_then(|p| std::fs::metadata(p).ok())
            .and_then(|m| m.modified().ok());
        if now != seen {
            seen = now;
            dock_watch
                .borrow_mut()
                .apply(read_dock_prefs().reserved_height());
        }
        gtk::glib::ControlFlow::Continue
    });

    std::mem::forget(top);
    std::mem::forget(dock);
    gtk::main();
}
