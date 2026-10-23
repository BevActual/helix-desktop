//! Helix summon: one key that brings up Mobius *about what the person was just doing*. The compositor is the only place that knows what had the
//! keyboard at the instant of the key press (by the time Mobius has a window, it has the keyboard itself). So `helix-summon` writes down the
//! focused window's title, application and process, then raises the Mobius window if there is one and starts Mobius if there is not. Mobius asks
//! for the note once (`HelixSummonContext`), shows it as a chip the person can remove, and sends it with their next message only if they leave it.
//! Nothing is read from the window beyond what every client can already list (its title, app id and pid), and the note is forgotten after a minute.

use std::time::{Duration, Instant};

use niri_ipc::HelixSummonContext;

use crate::niri::State;
use crate::utils::spawning::spawn;
use crate::utils::with_toplevel_role;

/// How long a note is kept for Mobius to pick up.
const KEEP: Duration = Duration::from_secs(60);

impl State {
    pub fn helix_summon(&mut self) {
        let (app_id, command) = {
            let config = self.niri.config.borrow();
            (
                config.helix.summon_app_id.clone(),
                config.helix.summon_command.clone(),
            )
        };
        // Note what has the keyboard, unless that is already Mobius (summoning it again should not make it the subject).
        let focused = self.niri.layout.focus().map(|mapped| {
            with_toplevel_role(mapped.toplevel(), |role| {
                (
                    role.title.clone(),
                    role.app_id.clone(),
                    mapped.credentials().map(|c| c.pid),
                )
            })
        });
        if let Some((title, focused_app, pid)) = focused {
            if focused_app.as_deref() != Some(app_id.as_str()) {
                self.niri.helix_summon = Some((
                    Instant::now(),
                    HelixSummonContext {
                        title,
                        app_id: focused_app,
                        pid,
                        age_ms: 0,
                    },
                ));
            }
        }
        // Raise it if it is open.
        let existing = self
            .niri
            .layout
            .windows()
            .find(|(_, mapped)| {
                with_toplevel_role(mapped.toplevel(), |role| {
                    role.app_id.as_deref() == Some(app_id.as_str())
                })
            })
            .map(|(_, mapped)| mapped.window.clone());
        if let Some(window) = existing {
            self.focus_window(&window);
        } else if !command.is_empty() {
            let (token, _) = self.niri.activation_state.create_external_token(None);
            spawn(command, Some(token.clone()));
        }
    }

    /// The note, once: taking it clears it.
    pub fn helix_take_summon_context(&mut self) -> Option<HelixSummonContext> {
        let (at, mut context) = self.niri.helix_summon.take()?;
        let age = at.elapsed();
        if age > KEEP {
            return None;
        }
        context.age_ms = age.as_millis() as u64;
        Some(context)
    }
}
