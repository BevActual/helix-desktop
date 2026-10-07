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
    gdk_window.input_shape_combine_region(&shape, 0, 0);
}

/// Creates the layer surface, hidden, sets it up in the order layer-shell requires, then shows it with the closed plan.
pub fn setup(window: &WebviewWindow, plan: Plan) -> Result<(), String> {
    let gtk_window = window.gtk_window().map_err(|e| e.to_string())?;
    gtk_window.set_size_request(1, 1);
    gtk_window.init_layer_shell();
    gtk_window.set_namespace("helix-shell");
    for edge in [Edge::Top, Edge::Bottom, Edge::Left, Edge::Right] {
        gtk_window.set_anchor(edge, true);
    }
    gtk_window.set_exclusive_zone(BAR_HEIGHT);
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
