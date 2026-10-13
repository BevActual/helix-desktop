//! Screen brightness as a pure model over what the backlight reports (a current value and a maximum). The model decides what may be
//! written: never zero (a black screen with no way to see the control that brings it back), never more than the maximum.

use serde::Serialize;

/// The dimmest the shell will set. A backlight at its very lowest can be unreadable; the keys and the slider stop here.
pub const MIN_PERCENT: f64 = 5.0;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Brightness {
    current: u32,
    max: u32,
}

/// What to write to the backlight.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Request {
    pub value: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
pub struct BrightnessView {
    pub available: bool,
    pub percent: u8,
}

impl Brightness {
    /// From the text of `brightness` and `max_brightness`. A maximum of zero, or anything that is not a number, is no backlight.
    pub fn parse(current: &str, max: &str) -> Option<Self> {
        let current: u32 = current.trim().parse().ok()?;
        let max: u32 = max.trim().parse().ok()?;
        (max > 0).then_some(Self {
            current: current.min(max),
            max,
        })
    }

    pub fn percent(&self) -> f64 {
        f64::from(self.current) * 100.0 / f64::from(self.max)
    }

    pub fn view(&self) -> BrightnessView {
        BrightnessView {
            available: true,
            percent: self.percent().round().clamp(0.0, 100.0) as u8,
        }
    }

    pub fn unavailable() -> BrightnessView {
        BrightnessView {
            available: false,
            percent: 0,
        }
    }

    /// Sets an absolute level, clamped to [`MIN_PERCENT`]..=100.
    pub fn set_percent(&self, percent: f64) -> Request {
        let percent = if percent.is_finite() {
            percent.clamp(MIN_PERCENT, 100.0)
        } else {
            self.percent()
        };
        let value = (percent * f64::from(self.max) / 100.0).round() as u32;
        Request {
            value: value.clamp(1, self.max),
        }
    }

    /// Moves by `delta` percentage points from where the backlight is now.
    pub fn adjust(&self, delta: i32) -> Request {
        self.set_percent(self.percent() + f64::from(delta))
    }

    /// The model after the backlight accepted `request`.
    pub fn with(&self, request: Request) -> Self {
        Self {
            current: request.value,
            max: self.max,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn at(current: u32, max: u32) -> Brightness {
        Brightness::parse(&current.to_string(), &max.to_string()).unwrap()
    }

    #[test]
    fn it_reads_what_sysfs_reports_and_refuses_nonsense() {
        assert_eq!(
            Brightness::parse("95\n", "100\n").unwrap().view(),
            BrightnessView {
                available: true,
                percent: 95
            }
        );
        assert!(
            Brightness::parse("5", "0").is_none(),
            "no maximum, no backlight"
        );
        assert!(Brightness::parse("abc", "100").is_none());
        assert!(Brightness::parse("5", "-1").is_none());
        assert!(Brightness::parse("", "").is_none());
    }

    #[test]
    fn a_current_value_above_the_maximum_is_clamped() {
        assert_eq!(Brightness::parse("500", "100").unwrap().percent(), 100.0);
    }

    #[test]
    fn the_percentage_is_exact_on_a_fine_scale_and_rounded_on_a_coarse_one() {
        assert_eq!(at(120, 255).view().percent, 47);
        assert_eq!(at(1, 3).view().percent, 33);
        assert_eq!(at(2, 3).view().percent, 67, "rounded, not cut off");
        assert_eq!(at(0, 255).set_percent(50.0).value, 128, "127.5 rounds up");
    }

    #[test]
    fn setting_is_clamped_between_the_dimmest_allowed_and_full() {
        let b = at(50, 100);
        assert_eq!(b.set_percent(70.0).value, 70);
        assert_eq!(b.set_percent(0.0).value, 5, "never black");
        assert_eq!(b.set_percent(-20.0).value, 5);
        assert_eq!(b.set_percent(500.0).value, 100);
    }

    #[test]
    fn a_value_is_never_zero_even_on_a_coarse_backlight() {
        let b = at(2, 3);
        assert_eq!(b.set_percent(5.0).value, 1);
        assert_eq!(b.set_percent(100.0).value, 3);
        assert!(b.set_percent(0.0).value >= 1);
    }

    #[test]
    fn nan_and_infinity_change_nothing() {
        let b = at(40, 100);
        assert_eq!(b.set_percent(f64::NAN).value, 40);
        assert_eq!(b.set_percent(f64::INFINITY).value, 40);
        assert_eq!(b.set_percent(f64::NEG_INFINITY).value, 40);
    }

    #[test]
    fn keys_move_by_steps_and_stop_at_both_ends() {
        let b = at(50, 100);
        assert_eq!(b.adjust(5).value, 55);
        assert_eq!(b.adjust(-5).value, 45);
        assert_eq!(at(98, 100).adjust(5).value, 100);
        assert_eq!(at(7, 100).adjust(-5).value, 5);
        assert_eq!(at(5, 100).adjust(-5).value, 5);
    }

    #[test]
    fn repeated_presses_walk_the_scale() {
        let mut b = at(50, 255);
        let start = b.view().percent;
        for _ in 0..4 {
            b = b.with(b.adjust(5));
        }
        assert!(
            b.view().percent >= start + 18 && b.view().percent <= start + 22,
            "{} -> {}",
            start,
            b.view().percent
        );
    }

    #[test]
    fn no_backlight_is_reported_as_unavailable() {
        assert_eq!(
            Brightness::unavailable(),
            BrightnessView {
                available: false,
                percent: 0
            }
        );
    }
}
