//! How round the corners are: one choice from Settings → Appearance that sets the corners of every window (the compositor's override) and of the shell's
//! own bar, dock and panels (a scale the shell reads from `~/.config/helix/shape-settings.json`). The steps are fixed so the two always agree.

use serde_json::{json, Value};

/// A step: the name Settings sends, what the page calls it, the radius of a window in pixels and the scale of the shell's own corners.
pub struct Step {
    pub value: &'static str,
    pub label: &'static str,
    pub window_radius: u16,
    pub scale: f64,
}

pub const STEPS: [Step; 4] = [
    Step {
        value: "square",
        label: "Square",
        window_radius: 0,
        scale: 0.0,
    },
    Step {
        value: "soft",
        label: "Soft",
        window_radius: 6,
        scale: 0.5,
    },
    Step {
        value: "default",
        label: "Default",
        window_radius: 12,
        scale: 1.0,
    },
    Step {
        value: "round",
        label: "Round",
        window_radius: 20,
        scale: 1.6,
    },
];

pub fn step(value: &str) -> Option<&'static Step> {
    STEPS.iter().find(|s| s.value == value)
}

/// The step whose window radius is nearest to `radius` (what the compositor reports).
pub fn nearest(radius: u16) -> &'static Step {
    STEPS
        .iter()
        .min_by_key(|s| s.window_radius.abs_diff(radius))
        .expect("steps")
}

/// The scale in a settings file's text: a number from 0 to 2, else 1 (the default look).
pub fn parse_scale(text: &str) -> f64 {
    serde_json::from_str::<Value>(text)
        .ok()
        .and_then(|v| v.get("scale").and_then(Value::as_f64))
        .filter(|s| s.is_finite())
        .map_or(1.0, |s| s.clamp(0.0, 2.0))
}

pub fn to_json(step: &Step) -> String {
    json!({"version": 1, "step": step.value, "scale": step.scale}).to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_steps_agree_in_both_places_and_are_found_by_name_or_nearness() {
        assert_eq!(step("soft").unwrap().window_radius, 6);
        assert!(step("pointy").is_none());
        assert_eq!(nearest(0).value, "square");
        assert_eq!(nearest(5).value, "soft");
        assert_eq!(nearest(12).value, "default");
        assert_eq!(nearest(32).value, "round");
        for s in &STEPS {
            assert_eq!(parse_scale(&to_json(s)), s.scale);
        }
    }

    #[test]
    fn a_missing_or_odd_file_means_the_default_look() {
        for text in ["", "{", "[]", r#"{"scale":"big"}"#, r#"{"scale":null}"#] {
            assert_eq!(parse_scale(text), 1.0, "{text}");
        }
        assert_eq!(parse_scale(r#"{"scale": 9}"#), 2.0);
        assert_eq!(parse_scale(r#"{"scale": -1}"#), 0.0);
    }
}
