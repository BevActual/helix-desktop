//! The light at the screen edges while an agent is acting (`docs/design/north-star.md`, "one presence, drawn by the system"). The shell says
//! when (`HelixSetGlow`); the compositor draws it, below trusted surfaces and their rim and above everything else, so no application can
//! fake it or hide it. Each edge glows inward in one colour of Apple Intelligence's light (pink along the top, blue on the right, purple
//! along the bottom, orange on the left), fading over [`DEPTH`] logical pixels. It is still: no animation, so no frame is drawn for it
//! while nothing else changes.

use smithay::utils::{Logical, Point, Size};

use crate::niri::State;
use crate::render_helpers::solid_color::SolidColorBuffer;

/// How many strips each edge is drawn with, and how deep the light reaches.
pub const STRIPS: usize = 12;
pub const DEPTH: f64 = 30.;

/// Top, right, bottom, left.
const COLORS: [[f32; 3]; 4] = [
    [1.0, 0.416, 0.757],
    [0.353, 0.784, 0.98],
    [0.635, 0.349, 1.0],
    [1.0, 0.71, 0.278],
];

/// The opacity of strip `i` (0 at the edge): bright at the edge, gone by the inner side.
pub fn alpha(i: usize) -> f32 {
    let t = i as f32 / STRIPS as f32;
    0.6 * (1.0 - t) * (1.0 - t)
}

/// Where each strip goes and how big it is, on an output of `size`: the same order as the buffers ([edge * STRIPS + strip]).
pub fn places(size: Size<f64, Logical>) -> Vec<(Point<f64, Logical>, Size<f64, Logical>)> {
    let step = DEPTH / STRIPS as f64;
    let (w, h) = (size.w, size.h);
    let mut out = Vec::with_capacity(4 * STRIPS);
    for edge in 0..4 {
        for i in 0..STRIPS {
            let d = i as f64 * step;
            out.push(match edge {
                0 => (Point::from((0., d)), Size::from((w, step))),
                1 => (Point::from((w - d - step, 0.)), Size::from((step, h))),
                2 => (Point::from((0., h - d - step)), Size::from((w, step))),
                _ => (Point::from((d, 0.)), Size::from((step, h))),
            });
        }
    }
    out
}

/// The strips' buffers for an output of `size`, coloured and sized.
pub fn buffers(size: Size<f64, Logical>) -> Vec<SolidColorBuffer> {
    places(size)
        .into_iter()
        .enumerate()
        .map(|(n, (_, strip))| {
            let (edge, i) = (n / STRIPS, n % STRIPS);
            let [r, g, b] = COLORS[edge];
            let a = alpha(i);
            // Solid colours are premultiplied: the light is its colour times how much of it there is.
            SolidColorBuffer::new(strip, [r * a, g * a, b * a, a])
        })
        .collect()
}

impl State {
    /// `HelixSetGlow`: light the screen edges, or put the light out.
    pub fn helix_set_glow(&mut self, on: bool) {
        if self.niri.helix_agent_glow == on {
            return;
        }
        self.niri.helix_agent_glow = on;
        tracing::info!("agent glow {}", if on { "on" } else { "off" });
        self.niri.queue_redraw_all();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_light_is_brightest_at_the_edge_and_gone_inside() {
        assert!(alpha(0) > 0.4);
        assert!((1..STRIPS).all(|i| alpha(i) < alpha(i - 1)));
        assert!(alpha(STRIPS - 1) < 0.02);
    }

    #[test]
    fn the_strips_cover_each_edge_to_the_depth_and_no_further() {
        let size = Size::from((1280., 720.));
        let p = places(size);
        assert_eq!(p.len(), 4 * STRIPS);
        // The top edge starts at the top and its innermost strip ends at DEPTH.
        assert_eq!(p[0].0, Point::from((0., 0.)));
        let last_top = p[STRIPS - 1];
        assert!((last_top.0.y + last_top.1.h - DEPTH).abs() < 1e-9);
        // The right edge starts flush with the right side.
        assert!((p[STRIPS].0.x + p[STRIPS].1.w - 1280.).abs() < 1e-9);
        // Nothing is outside the output.
        assert!(p.iter().all(|(at, s)| at.x >= 0.
            && at.y >= 0.
            && at.x + s.w <= 1280. + 1e-9
            && at.y + s.h <= 720. + 1e-9));
    }
}
