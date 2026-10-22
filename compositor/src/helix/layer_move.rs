//! Helix: a panel opens on the screen the person is working on. The shell's panels are one layer surface, made when the shell starts, so it sits on
//! whichever output had the focus then. When a panel is about to open the shell asks for the surface to be moved to the output that has the focus now.
//! The surface is not re-made (a client re-making a layer surface is what goes wrong): it is taken out of one output's layer map and put in the
//! other's, which sends it the new size.

use smithay::desktop::{layer_map_for_output, LayerSurface};
use smithay::wayland::shell::wlr_layer::KeyboardInteractivity;

use crate::niri::State;

impl State {
    pub fn helix_move_layer_to_focused_output(&mut self, namespace: &str) {
        let Some(target) = self.niri.layout.active_output().cloned() else {
            return;
        };
        let others: Vec<_> = self
            .niri
            .global_space
            .outputs()
            .filter(|o| **o != target)
            .cloned()
            .collect();
        let mut moved = false;
        for output in others {
            let layers: Vec<LayerSurface> = layer_map_for_output(&output)
                .layers()
                .filter(|layer| layer.namespace() == namespace)
                // One that holds the keyboard is an open panel: it stays where the person opened it.
                .filter(|layer| {
                    layer.cached_state().keyboard_interactivity != KeyboardInteractivity::Exclusive
                })
                .cloned()
                .collect();
            for layer in layers {
                layer_map_for_output(&output).unmap_layer(&layer);
                if layer_map_for_output(&target).map_layer(&layer).is_ok() {
                    moved = true;
                }
            }
        }
        if moved {
            self.niri.queue_redraw_all();
        }
    }
}
