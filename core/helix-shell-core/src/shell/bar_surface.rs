//! The bar process's layer surface: always Top, no keyboard, bar-strip input only.
//! Opening a panel never changes this plan — panels live in `helix-shell-panels`.

use crate::surfaces::{KeyboardMode, Layer};

use super::shell_surface::{InputRegion, Plan, BAR_HEIGHT};

/// Always-on bar chrome plan (never Overlay, never exclusive keyboard).
pub const BAR_PLAN: Plan = Plan {
    layer: Layer::Top,
    keyboard: KeyboardMode::None,
    input: InputRegion::BarOnly,
};

/// Pure model for the bar surface. Exists so the bar binary cannot accidentally
/// promote itself to Overlay when a panel opens.
#[derive(Debug, Default)]
pub struct BarSurface;

impl BarSurface {
    pub fn new() -> Self {
        Self
    }

    pub fn plan(&self) -> Plan {
        BAR_PLAN
    }

    pub fn bar_height(&self) -> i32 {
        BAR_HEIGHT
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bar_plan_never_takes_overlay_or_keyboard() {
        let bar = BarSurface::new();
        let plan = bar.plan();
        assert_eq!(plan.layer, Layer::Top);
        assert_eq!(plan.keyboard, KeyboardMode::None);
        assert_eq!(plan.input, InputRegion::BarOnly);
        // Opening nothing changes it — there is no open API by design.
        assert_eq!(bar.plan(), BAR_PLAN);
    }
}
