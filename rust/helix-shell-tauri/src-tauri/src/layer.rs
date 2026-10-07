//! Apply surface-manager steps to GTK layer-shell windows, in the order the steps are given.
//! Size is requested before init_layer_shell because a layer surface with no size paints nothing.

use gtk::prelude::*;
use gtk_layer_shell::LayerShell;
use helix_shell_core::surfaces::{Edge, ExclusiveZone, KeyboardMode, Layer, Step};
use tauri::{AppHandle, Manager};

pub fn apply(app: &AppHandle, steps: &[Step]) {
    for step in steps {
        // Destroy must not hold a gtk::Window across the call: destroy() only queues work on
        // the event loop, and a live GTK handle can keep the WebKit process pinned.
        if let Step::Destroy { id } = step {
            let Some(webview) = app.get_webview_window(id) else {
                eprintln!("surface {id} has no window");
                continue;
            };
            let _ = webview.destroy();
            if id == "panel" {
                eprintln!("PANEL destroyed");
            }
            continue;
        }

        let Some(webview) = app.get_webview_window(step.id()) else {
            eprintln!("surface {} has no window", step.id());
            continue;
        };
        let Ok(window) = webview.gtk_window() else {
            eprintln!("surface {} has no gtk window", step.id());
            continue;
        };
        match step {
            Step::CreateHidden { .. } => {
                let _ = webview.hide();
            }
            Step::SetSizeRequest { width, height, .. } => window.set_size_request(*width, *height),
            Step::InitLayerShell { .. } => window.init_layer_shell(),
            Step::SetLayer { layer, .. } => window.set_layer(match layer {
                Layer::Top => gtk_layer_shell::Layer::Top,
                Layer::Overlay => gtk_layer_shell::Layer::Overlay,
            }),
            Step::SetAnchors { edges, .. } => {
                for edge in [Edge::Top, Edge::Bottom, Edge::Left, Edge::Right] {
                    window.set_anchor(gtk_edge(edge), edges.contains(&edge));
                }
            }
            Step::SetExclusiveZone { zone, .. } => match zone {
                ExclusiveZone::Auto => window.auto_exclusive_zone_enable(),
                ExclusiveZone::None => window.set_exclusive_zone(0),
            },
            Step::SetKeyboardMode { mode, .. } => window.set_keyboard_mode(match mode {
                KeyboardMode::None => gtk_layer_shell::KeyboardMode::None,
                KeyboardMode::Exclusive => gtk_layer_shell::KeyboardMode::Exclusive,
            }),
            Step::SetNamespace { namespace, .. } => window.set_namespace(namespace),
            Step::Show { id } => {
                let _ = webview.show();
                if id == "panel" {
                    eprintln!("PANEL open");
                }
            }
            Step::Hide { id } => {
                let _ = webview.hide();
                if id == "panel" {
                    eprintln!("PANEL closed");
                }
            }
            Step::Destroy { .. } => unreachable!("handled above"),
        }
    }
}

fn gtk_edge(edge: Edge) -> gtk_layer_shell::Edge {
    match edge {
        Edge::Top => gtk_layer_shell::Edge::Top,
        Edge::Bottom => gtk_layer_shell::Edge::Bottom,
        Edge::Left => gtk_layer_shell::Edge::Left,
        Edge::Right => gtk_layer_shell::Edge::Right,
    }
}

/// Fires after the panel has been active and then loses that, which is a click outside it.
pub fn when_deactivated(window: &impl IsA<gtk::Window>, callback: impl Fn() + 'static) {
    let was_active = std::rc::Rc::new(std::cell::Cell::new(false));
    let flag = was_active.clone();
    window.connect_notify_local(Some("is-active"), move |window, _| {
        if window.is_active() {
            flag.set(true);
        } else if flag.replace(false) {
            callback();
        }
    });
}
