//! The Magic Pointer's half in the compositor: `helix-shell "point"` notes where the pointer is and the window under it (only the
//! compositor knows both at the instant of the key press), then the shell opens its bubble there and asks for the note once
//! (`HelixPointContext`). Nothing is read from the window here; the shell captures it only when the person picks something to do with
//! it. The note is forgotten after a minute.

use std::time::{Duration, Instant};

use niri_ipc::{HelixPointContext, HelixPointWindow};
use smithay::output::Output;

use crate::layout::HitType;
use crate::niri::State;
use crate::utils::with_toplevel_role;

const KEEP: Duration = Duration::from_secs(60);

impl State {
    pub fn helix_note_point(&mut self) {
        let pos = self.niri.seat.get_pointer().unwrap().current_location();
        let Some((output, within)) = self.niri.output_under(pos) else {
            self.niri.helix_point = None;
            return;
        };
        let output: Output = output.clone();
        let window = if self.niri.window_under(pos).is_some() {
            self.niri
                .layout
                .window_under(&output, within)
                .and_then(|(mapped, hit)| {
                    let HitType::Input { win_pos } = hit else {
                        return None;
                    };
                    // A window screenshot holds the whole surface (shadows included) from the corner of its bounding box.
                    let bbox = mapped.window.bbox();
                    let at = within - win_pos - bbox.loc.to_f64();
                    with_toplevel_role(mapped.toplevel(), |role| HelixPointWindow {
                        id: mapped.id().get(),
                        title: role.title.clone(),
                        app_id: role.app_id.clone(),
                        pid: mapped.credentials().map(|c| c.pid),
                        x: at.x,
                        y: at.y,
                        width: bbox.size.w,
                        height: bbox.size.h,
                    })
                    .into()
                })
        } else {
            None
        };
        self.niri.helix_point = Some((
            Instant::now(),
            HelixPointContext {
                output: output.name(),
                x: within.x,
                y: within.y,
                window,
                age_ms: 0,
            },
        ));
    }

    /// The note, once: taking it clears it.
    pub fn helix_take_point_context(&mut self) -> Option<HelixPointContext> {
        let (at, mut context) = self.niri.helix_point.take()?;
        let age = at.elapsed();
        if age > KEEP {
            return None;
        }
        context.age_ms = age.as_millis() as u64;
        Some(context)
    }
}
