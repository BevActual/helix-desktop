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
    let rect = match region {
        InputRegion::BarOnly => (0, 0, width.max(1), BAR_HEIGHT),
        InputRegion::Whole => (0, 0, width.max(1), height.max(1)),
    };
    let shape = gtk::cairo::Region::create();
    let _ = shape.union_rectangle(&gtk::cairo::RectangleInt::new(
        rect.0, rect.1, rect.2, rect.3,
    ));
    // The extras only matter while the pointer is otherwise limited to the bar; a panel's region already covers everything.
    if region == InputRegion::BarOnly {
        for (x, y, w, h) in EXTRA.lock().expect("extra input").iter() {
            let _ = shape.union_rectangle(&gtk::cairo::RectangleInt::new(*x, *y, *w, *h));
        }
    }
    gdk_window.input_shape_combine_region(&shape, 0, 0);
}

/// A surface anchored to all four edges has no single edge for the compositor to reserve space on, so the exclusive zone set on the
/// shell surface is ignored and windows would slide under the bar. This is a second, empty, transparent surface (a plain GTK window,
/// no web view, so no extra web process) anchored to the top and sides whose only job is to reserve the bar's strip.
pub fn reserve_bar_space() {
    let window = gtk::Window::new(gtk::WindowType::Toplevel);
    window.set_decorated(false);
    window.set_accept_focus(false);
    window.set_app_paintable(true);
    if let Some(visual) =
        gtk::prelude::WidgetExt::screen(&window).and_then(|screen| screen.rgba_visual())
    {
        window.set_visual(Some(&visual));
    }
    // Nothing of this window is ever drawn: it is fully transparent and takes no pointer input.
    let css = gtk::CssProvider::new();
    if css
        .load_from_data(b"window, window * { background: transparent; }")
        .is_ok()
    {
        window
            .style_context()
            .add_provider(&css, gtk::STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
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
    if let Some(gdk_window) = window.window() {
        gdk_window.input_shape_combine_region(&gtk::cairo::Region::create(), 0, 0);
    }
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
    // "noctalia-panel" is what the person's niri blur and liquid-glass rule (generated from their Theme settings) matches for panels.
    gtk_window.set_namespace("noctalia-panel");
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
    let Some(window) = app.get_webview_window(WINDOW) else { return };
    let Ok(gtk_window) = window.gtk_window() else { return };
    let region = *INPUT.lock().expect("input region");
    set_input_region(&gtk_window, region, gtk_window.allocated_width(), gtk_window.allocated_height());
}
