//! Puts the shell's single layer surface on screen and applies [`Plan`]s to it.
//!
//! The surface covers the whole output and is anchored to all four edges. Its size is requested before layer-shell init because
//! a layer surface with no size paints nothing. The bar's space is reserved with an explicit exclusive zone (an automatic one
//! would reserve the whole output). Which part accepts the pointer is the GDK input shape, and it is re-applied on every resize,
//! because the surface only gets its real size after the compositor configures it.

use std::sync::Mutex;

use gtk::prelude::*;
use gtk_layer_shell::{Edge, LayerShell};
use helix_shell_core::shell_surface::{InputRegion, BAR_HEIGHT};
use helix_shell_core::surfaces::{KeyboardMode, Layer};
use helix_shell_core::Plan;
use tauri::{AppHandle, Manager, WebviewWindow};

pub const WINDOW: &str = "shell";

/// What the size-allocate hook should apply. Written whenever a plan is applied.
static INPUT: Mutex<InputRegion> = Mutex::new(InputRegion::BarOnly);

/// Rectangles besides the bar that take the pointer while no panel is open (the dock, or the strip that summons it).
static EXTRA: Mutex<Vec<(i32, i32, i32, i32)>> = Mutex::new(Vec::new());

fn set_input_region(window: &gtk::ApplicationWindow, region: InputRegion, width: i32, height: i32) {
    let Some(gdk_window) = window.window() else {
        return;
    };
    let shape = gtk::cairo::Region::create();
    match region {
        InputRegion::None => {
            // Empty shape: clicks pass through (idle panels Overlay must not steal the bar strip).
        }
        InputRegion::BarOnly => {
            let _ = shape.union_rectangle(&gtk::cairo::RectangleInt::new(
                0,
                0,
                width.max(1),
                BAR_HEIGHT,
            ));
            for (x, y, w, h) in EXTRA.lock().expect("extra input").iter() {
                let _ = shape.union_rectangle(&gtk::cairo::RectangleInt::new(*x, *y, *w, *h));
            }
        }
        InputRegion::Whole => {
            let _ = shape.union_rectangle(&gtk::cairo::RectangleInt::new(
                0,
                0,
                width.max(1),
                height.max(1),
            ));
        }
    }
    gdk_window.input_shape_combine_region(&shape, 0, 0);
}

thread_local! {
    static TOP_STRUT: std::cell::RefCell<Option<gtk::Window>> = const { std::cell::RefCell::new(None) };
}

fn make_transparent_top(window: &gtk::Window) {
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

/// A surface anchored to all four edges has no single edge for the compositor to reserve space on, so the exclusive zone set on the
/// shell surface is ignored and windows would slide under the bar. This is a second, empty, transparent surface (a plain GTK window,
/// no web view, so no extra web process) anchored to the top and sides whose only job is to reserve the bar's strip.
pub fn reserve_bar_space() {
    set_top_reserve_zone(BAR_HEIGHT);
}

/// In-process top strut when `helix-shell-strut` is not running. `zone` is `BAR_HEIGHT` or `0`.
/// Must run on the GTK main thread (agent IPC and dbus bar commands use `on_main`).
pub fn set_top_reserve_zone(zone: i32) {
    TOP_STRUT.with(|slot| {
        let zone = zone.max(0);
        if let Some(window) = slot.borrow().as_ref() {
            window.set_size_request(1, zone.max(1));
            window.set_exclusive_zone(zone);
            return;
        }
        if zone == 0 {
            return;
        }
        let window = gtk::Window::new(gtk::WindowType::Toplevel);
        make_transparent_top(&window);
        window.set_size_request(1, zone);
        window.init_layer_shell();
        window.set_namespace("helix-shell-reserve");
        window.set_layer(gtk_layer_shell::Layer::Top);
        for edge in [Edge::Top, Edge::Left, Edge::Right] {
            window.set_anchor(edge, true);
        }
        window.set_exclusive_zone(zone);
        window.set_keyboard_mode(gtk_layer_shell::KeyboardMode::None);
        window.show_all();
        if let Some(gdk_window) = window.window() {
            gdk_window.input_shape_combine_region(&gtk::cairo::Region::create(), 0, 0);
        }
        *slot.borrow_mut() = Some(window);
    });
}

thread_local! {
    static DOCK_STRUT: std::cell::RefCell<Option<gtk::Window>> = const { std::cell::RefCell::new(None) };
}

/// Keeps `height` pixels free along the bottom edge for the dock (an empty transparent surface with an exclusive zone, as for the bar), or
/// lets the space go with `None`. Must run on the GTK thread.
pub fn set_dock_strut(height: Option<i32>) {
    DOCK_STRUT.with(|slot| {
        let mut slot = slot.borrow_mut();
        let Some(height) = height else {
            if let Some(window) = slot.take() {
                window.close();
            }
            return;
        };
        if let Some(window) = slot.as_ref() {
            window.set_size_request(1, height);
            window.set_exclusive_zone(height);
            return;
        }
        let window = gtk::Window::new(gtk::WindowType::Toplevel);
        window.set_decorated(false);
        window.set_accept_focus(false);
        window.set_app_paintable(true);
        if let Some(visual) = gtk::prelude::WidgetExt::screen(&window).and_then(|screen| screen.rgba_visual()) {
            window.set_visual(Some(&visual));
        }
        let css = gtk::CssProvider::new();
        if css.load_from_data(b"window, window * { background: transparent; }").is_ok() {
            window.style_context().add_provider(&css, gtk::STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
        window.set_size_request(1, height);
        window.init_layer_shell();
        window.set_namespace("helix-shell-dock-reserve");
        window.set_layer(gtk_layer_shell::Layer::Top);
        for edge in [Edge::Bottom, Edge::Left, Edge::Right] {
            window.set_anchor(edge, true);
        }
        window.set_exclusive_zone(height);
        window.set_keyboard_mode(gtk_layer_shell::KeyboardMode::None);
        window.show_all();
        if let Some(gdk_window) = window.window() {
            gdk_window.input_shape_combine_region(&gtk::cairo::Region::create(), 0, 0);
        }
        *slot = Some(window);
    });
}

/// Extra space above the shell, in pixels: `HELIX_SHELL_TOP_OFFSET`. Zero normally. Running beside another shell that owns the top edge
/// (the trial runs next to the native bar) sets it to that bar's height so the two do not draw over each other.
pub fn top_offset() -> i32 {
    std::env::var("HELIX_SHELL_TOP_OFFSET")
        .ok()
        .and_then(|value| value.trim().parse::<i32>().ok())
        .map_or(0, |value| value.clamp(0, 400))
}

/// Creates the layer surface, hidden, sets it up in the order layer-shell requires, then shows it with the closed plan.
pub fn setup(window: &WebviewWindow, plan: Plan) -> Result<(), String> {
    let gtk_window = window.gtk_window().map_err(|e| e.to_string())?;
    gtk_window.set_size_request(1, 1);
    gtk_window.init_layer_shell();
    // Bar keeps `helix-panel` (theme blur). Split panels process uses `helix-panels`.
    let namespace = match std::env::var("HELIX_SHELL_ROLE")
        .unwrap_or_default()
        .to_ascii_lowercase()
        .as_str()
    {
        "panels" | "panel" => "helix-panels",
        _ => "helix-panel",
    };
    gtk_window.set_namespace(namespace);
    for edge in [Edge::Top, Edge::Bottom, Edge::Left, Edge::Right] {
        gtk_window.set_anchor(edge, true);
    }
    LayerShell::set_layer_shell_margin(&gtk_window, Edge::Top, top_offset());
    // -1: cover the whole output even where another surface (the one below) has reserved space; that surface holds the zone.
    gtk_window.set_exclusive_zone(-1);
    apply_layer_and_keyboard(&gtk_window, plan);
    *INPUT.lock().expect("input region") = plan.input;
    gtk_window.connect_size_allocate(|window, allocation| {
        let region = *INPUT.lock().expect("input region");
        set_input_region(window, region, allocation.width(), allocation.height());
    });
    // The web view is not always told when the pointer leaves the surface (the compositor moves it to another surface, or the input
    // region shrinks under it), so the page is told from here, where the leave is certain.
    {
        use tauri::Emitter;
        let emitter = window.clone();
        gtk_window.connect_leave_notify_event(move |_, event| {
            if event.detail() != gtk::gdk::NotifyType::Inferior {
                let _ = emitter.emit("pointer-left", ());
            }
            gtk::glib::Propagation::Proceed
        });
    }
    window.show().map_err(|e| e.to_string())
}

fn apply_layer_and_keyboard(window: &gtk::ApplicationWindow, plan: Plan) {
    window.set_layer(match plan.layer {
        Layer::Top => gtk_layer_shell::Layer::Top,
        Layer::Overlay => gtk_layer_shell::Layer::Overlay,
    });
    window.set_keyboard_mode(match plan.keyboard {
        KeyboardMode::None => gtk_layer_shell::KeyboardMode::None,
        KeyboardMode::Exclusive => gtk_layer_shell::KeyboardMode::Exclusive,
    });
}

/// Applies a plan to the running surface. Must run on the main thread.
pub fn apply(app: &AppHandle, plan: Plan) {
    let Some(window) = app.get_webview_window(WINDOW) else {
        eprintln!("shell surface has no window");
        return;
    };
    let Ok(gtk_window) = window.gtk_window() else {
        eprintln!("shell surface has no gtk window");
        return;
    };
    *INPUT.lock().expect("input region") = plan.input;
    apply_layer_and_keyboard(&gtk_window, plan);
    set_input_region(
        &gtk_window,
        plan.input,
        gtk_window.allocated_width(),
        gtk_window.allocated_height(),
    );
}

/// Replaces the extra pointer-taking rectangles and re-applies the region. Must run on the main thread.
pub fn set_extra_input(app: &AppHandle, rects: &[helix_shell_core::glass::Rect]) {
    *EXTRA.lock().expect("extra input") = rects.iter().map(|r| (r.x, r.y, r.w, r.h)).collect();
    let Some(window) = app.get_webview_window(WINDOW) else {
        return;
    };
    let Ok(gtk_window) = window.gtk_window() else {
        return;
    };
    let region = *INPUT.lock().expect("input region");
    set_input_region(
        &gtk_window,
        region,
        gtk_window.allocated_width(),
        gtk_window.allocated_height(),
    );
}
