//! Helix pointer and keyboard settings: what Settings → Mouse & touchpad and Keyboard change. They are *overrides* over `config.kdl`: the person's
//! file is never rewritten, the overrides live in `$XDG_STATE_HOME/helix/input-overrides.json`, they are applied to the configuration every time it is
//! loaded (at start and at each reload), and they take effect at once when set over IPC. A field that was never set is whatever `config.kdl` says.

use std::path::PathBuf;

use niri_config::input::{AccelProfile, ClickMethod, ScrollMethod};
use niri_config::{FloatOrInt, Input};
use niri_ipc::HelixInput;

use crate::input::apply_libinput_settings;
use crate::niri::State;

fn file() -> PathBuf {
    let base = std::env::var_os("XDG_STATE_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/state")))
        .unwrap_or_default();
    base.join("helix/input-overrides.json")
}

fn load() -> HelixInput {
    std::fs::read_to_string(file())
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or_default()
}

fn save(overrides: &HelixInput) -> Result<(), String> {
    let path = file();
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let temporary = path.with_extension("json.tmp");
    let text = serde_json::to_string_pretty(overrides).map_err(|e| e.to_string())?;
    std::fs::write(&temporary, text).map_err(|e| e.to_string())?;
    std::fs::rename(&temporary, &path).map_err(|e| e.to_string())
}

/// Refuses anything outside what niri itself accepts, before it is stored.
pub fn validate(input: &HelixInput) -> Result<(), String> {
    let speed = |name: &str, value: Option<f64>| match value {
        Some(v) if !v.is_finite() || !(-1.0..=1.0).contains(&v) => {
            Err(format!("{name} is -1 to 1"))
        }
        _ => Ok(()),
    };
    speed("touchpad speed", input.touchpad_speed)?;
    speed("mouse speed", input.mouse_speed)?;
    if let Some(method) = &input.touchpad_click_method {
        if !matches!(method.as_str(), "clickfinger" | "button-areas") {
            return Err("click method is clickfinger or button-areas".into());
        }
    }
    if let Some(method) = &input.touchpad_scroll_method {
        if !matches!(method.as_str(), "two-finger" | "edge") {
            return Err("scrolling is two-finger or edge".into());
        }
    }
    if let Some(profile) = &input.mouse_accel_profile {
        if !matches!(profile.as_str(), "adaptive" | "flat") {
            return Err("acceleration is adaptive or flat".into());
        }
    }
    if input
        .keyboard_repeat_delay
        .is_some_and(|d| !(100..=2000).contains(&d))
    {
        return Err("repeat delay is 100 to 2000 ms".into());
    }
    if input
        .keyboard_repeat_rate
        .is_some_and(|r| !(1..=100).contains(&r))
    {
        return Err("repeat rate is 1 to 100 a second".into());
    }
    if let Some(layout) = &input.keyboard_layout {
        let ok = !layout.is_empty()
            && layout.chars().count() <= 64
            && layout
                .chars()
                .all(|c| c.is_ascii_alphanumeric() || matches!(c, ',' | '_' | '-'));
        if !ok {
            return Err("a layout is letters, digits and commas".into());
        }
    }
    Ok(())
}

/// The fields of `change` that are set replace those of `base`.
fn merge(base: &mut HelixInput, change: &HelixInput) {
    macro_rules! take {
        ($($field:ident),*) => {$( if change.$field.is_some() { base.$field = change.$field.clone(); } )*};
    }
    take!(
        touchpad_tap,
        touchpad_natural_scroll,
        touchpad_speed,
        touchpad_disable_while_typing,
        touchpad_click_method,
        touchpad_tap_and_drag,
        touchpad_drag_lock,
        touchpad_scroll_method,
        touchpad_off_with_mouse,
        mouse_natural_scroll,
        mouse_speed,
        mouse_accel_profile,
        mouse_left_handed,
        keyboard_repeat_delay,
        keyboard_repeat_rate,
        keyboard_layout
    );
}

/// Puts the overrides over a configuration's input section.
pub fn overlay(input: &mut Input, o: &HelixInput) {
    if let Some(v) = o.touchpad_tap {
        input.touchpad.tap = v;
    }
    if let Some(v) = o.touchpad_natural_scroll {
        input.touchpad.natural_scroll = v;
    }
    if let Some(v) = o.touchpad_speed {
        input.touchpad.accel_speed = FloatOrInt(v);
    }
    if let Some(v) = o.touchpad_disable_while_typing {
        input.touchpad.dwt = v;
    }
    if let Some(v) = &o.touchpad_click_method {
        input.touchpad.click_method = match v.as_str() {
            "clickfinger" => Some(ClickMethod::Clickfinger),
            "button-areas" => Some(ClickMethod::ButtonAreas),
            _ => input.touchpad.click_method,
        };
    }
    if let Some(v) = o.touchpad_tap_and_drag {
        input.touchpad.drag = Some(v);
    }
    if let Some(v) = o.touchpad_drag_lock {
        input.touchpad.drag_lock = v;
    }
    if let Some(v) = &o.touchpad_scroll_method {
        input.touchpad.scroll_method = match v.as_str() {
            "two-finger" => Some(ScrollMethod::TwoFinger),
            "edge" => Some(ScrollMethod::Edge),
            _ => input.touchpad.scroll_method,
        };
    }
    if let Some(v) = o.touchpad_off_with_mouse {
        input.touchpad.disabled_on_external_mouse = v;
    }
    if let Some(v) = o.mouse_natural_scroll {
        input.mouse.natural_scroll = v;
    }
    if let Some(v) = o.mouse_speed {
        input.mouse.accel_speed = FloatOrInt(v);
    }
    if let Some(v) = &o.mouse_accel_profile {
        input.mouse.accel_profile = match v.as_str() {
            "adaptive" => Some(AccelProfile::Adaptive),
            "flat" => Some(AccelProfile::Flat),
            _ => input.mouse.accel_profile,
        };
    }
    if let Some(v) = o.mouse_left_handed {
        input.mouse.left_handed = v;
    }
    if let Some(v) = o.keyboard_repeat_delay {
        input.keyboard.repeat_delay = v;
    }
    if let Some(v) = o.keyboard_repeat_rate {
        input.keyboard.repeat_rate = v;
    }
    if let Some(v) = &o.keyboard_layout {
        input.keyboard.xkb.layout = v.clone();
        // A layout chosen here must not be shadowed by a keymap file in config.kdl.
        input.keyboard.xkb.file = None;
    }
}

/// What is in effect, every field set.
pub fn effective(input: &Input) -> HelixInput {
    let click = |m: Option<ClickMethod>| {
        Some(
            match m {
                Some(ClickMethod::ButtonAreas) => "button-areas",
                _ => "clickfinger",
            }
            .to_string(),
        )
    };
    HelixInput {
        touchpad_tap: Some(input.touchpad.tap),
        touchpad_natural_scroll: Some(input.touchpad.natural_scroll),
        touchpad_speed: Some(input.touchpad.accel_speed.0),
        touchpad_disable_while_typing: Some(input.touchpad.dwt),
        touchpad_click_method: click(input.touchpad.click_method),
        touchpad_tap_and_drag: Some(input.touchpad.drag.unwrap_or(input.touchpad.tap)),
        touchpad_drag_lock: Some(input.touchpad.drag_lock),
        touchpad_scroll_method: Some(
            match input.touchpad.scroll_method {
                Some(ScrollMethod::Edge) => "edge",
                _ => "two-finger",
            }
            .to_string(),
        ),
        touchpad_off_with_mouse: Some(input.touchpad.disabled_on_external_mouse),
        mouse_natural_scroll: Some(input.mouse.natural_scroll),
        mouse_speed: Some(input.mouse.accel_speed.0),
        mouse_accel_profile: Some(
            match input.mouse.accel_profile {
                Some(AccelProfile::Flat) => "flat",
                _ => "adaptive",
            }
            .to_string(),
        ),
        mouse_left_handed: Some(input.mouse.left_handed),
        keyboard_repeat_delay: Some(input.keyboard.repeat_delay),
        keyboard_repeat_rate: Some(input.keyboard.repeat_rate),
        keyboard_layout: Some(if input.keyboard.xkb.layout.is_empty() {
            "us".to_string()
        } else {
            input.keyboard.xkb.layout.clone()
        }),
    }
}

/// Applies the remembered overrides to a configuration that was just loaded (start-up and every reload).
pub fn apply_saved(input: &mut Input) {
    overlay(input, &load());
}

impl State {
    pub fn helix_input(&self) -> HelixInput {
        effective(&self.niri.config.borrow().input)
    }

    /// Changes the fields that are set, remembers them, and makes them take effect now.
    pub fn helix_set_input(&mut self, change: HelixInput) -> Result<(), String> {
        validate(&change)?;
        let mut stored = load();
        merge(&mut stored, &change);
        save(&stored)?;
        let (old_touchpad, old_mouse, old_xkb, old_delay, old_rate) = {
            let config = self.niri.config.borrow();
            let k = &config.input.keyboard;
            (
                config.input.touchpad.clone(),
                config.input.mouse.clone(),
                k.xkb.clone(),
                k.repeat_delay,
                k.repeat_rate,
            )
        };
        let (xkb, delay, rate, devices_changed) = {
            let mut config = self.niri.config.borrow_mut();
            overlay(&mut config.input, &stored);
            let k = &config.input.keyboard;
            (
                (k.xkb != old_xkb).then(|| k.xkb.clone()),
                k.repeat_delay,
                k.repeat_rate,
                config.input.touchpad != old_touchpad || config.input.mouse != old_mouse,
            )
        };
        if (delay, rate) != (old_delay, old_rate) {
            if let Some(keyboard) = self.niri.seat.get_keyboard() {
                keyboard.change_repeat_info(rate.into(), delay.into());
            }
        }
        if let Some(mut xkb) = xkb {
            if xkb == niri_config::input::Xkb::default() {
                xkb = self.niri.xkb_from_locale1.clone().unwrap_or_default();
            }
            self.set_xkb_config(xkb.to_xkb_config());
            self.ipc_keyboard_layouts_changed();
        }
        if devices_changed {
            let config = self.niri.config.borrow();
            for mut device in self.niri.devices.iter().cloned() {
                apply_libinput_settings(&config.input, &mut device);
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn touchpad_extras_are_set_and_read_back() {
        let mut input = Input::default();
        overlay(
            &mut input,
            &HelixInput {
                touchpad_tap_and_drag: Some(false),
                touchpad_drag_lock: Some(true),
                touchpad_scroll_method: Some("edge".into()),
                touchpad_off_with_mouse: Some(true),
                ..HelixInput::default()
            },
        );
        let now = effective(&input);
        assert_eq!(now.touchpad_tap_and_drag, Some(false));
        assert_eq!(now.touchpad_drag_lock, Some(true));
        assert_eq!(now.touchpad_scroll_method.as_deref(), Some("edge"));
        assert_eq!(now.touchpad_off_with_mouse, Some(true));
        assert_eq!(
            effective(&Input::default())
                .touchpad_scroll_method
                .as_deref(),
            Some("two-finger")
        );
    }

    #[test]
    fn overrides_replace_only_what_they_name() {
        let mut input = Input::default();
        input.touchpad.natural_scroll = true;
        input.mouse.left_handed = true;
        overlay(
            &mut input,
            &HelixInput {
                touchpad_tap: Some(true),
                touchpad_speed: Some(0.5),
                mouse_accel_profile: Some("flat".into()),
                keyboard_repeat_rate: Some(40),
                keyboard_layout: Some("us,de".into()),
                ..HelixInput::default()
            },
        );
        assert!(
            input.touchpad.tap && input.touchpad.natural_scroll,
            "untouched fields stay"
        );
        assert_eq!(input.touchpad.accel_speed.0, 0.5);
        assert!(input.mouse.left_handed);
        assert_eq!(input.mouse.accel_profile, Some(AccelProfile::Flat));
        assert_eq!(
            (input.keyboard.repeat_rate, input.keyboard.repeat_delay),
            (40, 600)
        );
        assert_eq!(input.keyboard.xkb.layout, "us,de");
        let seen = effective(&input);
        assert_eq!(seen.touchpad_tap, Some(true));
        assert_eq!(seen.mouse_accel_profile.as_deref(), Some("flat"));
        assert_eq!(seen.keyboard_layout.as_deref(), Some("us,de"));
    }

    #[test]
    fn later_changes_win_and_the_rest_are_kept() {
        let mut stored = HelixInput {
            touchpad_tap: Some(true),
            mouse_speed: Some(0.2),
            ..HelixInput::default()
        };
        merge(
            &mut stored,
            &HelixInput {
                mouse_speed: Some(-0.4),
                keyboard_repeat_delay: Some(300),
                ..HelixInput::default()
            },
        );
        assert_eq!(
            (
                stored.touchpad_tap,
                stored.mouse_speed,
                stored.keyboard_repeat_delay
            ),
            (Some(true), Some(-0.4), Some(300))
        );
    }

    #[test]
    fn only_what_niri_accepts_is_stored() {
        let ok = HelixInput {
            touchpad_speed: Some(1.0),
            keyboard_repeat_delay: Some(250),
            keyboard_layout: Some("us,de".into()),
            ..HelixInput::default()
        };
        assert!(validate(&ok).is_ok());
        for bad in [
            HelixInput {
                touchpad_speed: Some(1.5),
                ..HelixInput::default()
            },
            HelixInput {
                mouse_speed: Some(f64::NAN),
                ..HelixInput::default()
            },
            HelixInput {
                touchpad_click_method: Some("tripleclick".into()),
                ..HelixInput::default()
            },
            HelixInput {
                touchpad_scroll_method: Some("circular".into()),
                ..HelixInput::default()
            },
            HelixInput {
                mouse_accel_profile: Some("turbo".into()),
                ..HelixInput::default()
            },
            HelixInput {
                keyboard_repeat_delay: Some(5),
                ..HelixInput::default()
            },
            HelixInput {
                keyboard_repeat_rate: Some(0),
                ..HelixInput::default()
            },
            HelixInput {
                keyboard_layout: Some("us; rm -rf".into()),
                ..HelixInput::default()
            },
            HelixInput {
                keyboard_layout: Some(String::new()),
                ..HelixInput::default()
            },
        ] {
            assert!(validate(&bad).is_err(), "{bad:?}");
        }
    }
}
