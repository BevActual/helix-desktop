//! Mouse & touchpad, and the typing half of Keyboard: pointer speed, scrolling direction, tap to click, the repeat rate, the layout. The Helix
//! compositor holds these (as overrides over `config.kdl`, applied at once and remembered); this builds the rows from what it reports and turns the
//! page's changes into the one request it takes. Another compositor reports nothing, and the pages say so.

use helix_compositor::compositor_ipc::InputSettings;
use helix_config::catalog::{Choice, Control, RowView};
use serde_json::Value;

/// The keyboard layouts the system's xkb rules know: `(code, description)`, from the `! layout` section of `evdev.lst`.
pub fn read_layouts() -> Vec<(String, String)> {
    let text = std::fs::read_to_string("/usr/share/X11/xkb/rules/evdev.lst").unwrap_or_default();
    parse_layouts(&text)
}

pub fn parse_layouts(text: &str) -> Vec<(String, String)> {
    let mut out = Vec::new();
    let mut inside = false;
    for line in text.lines() {
        if let Some(heading) = line.strip_prefix('!') {
            inside = heading.trim() == "layout";
            continue;
        }
        if !inside {
            continue;
        }
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        let (code, description) = line.split_once(char::is_whitespace).unwrap_or((line, ""));
        let ok = !code.is_empty()
            && code
                .chars()
                .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-');
        if ok {
            out.push((
                code.to_string(),
                description.trim().chars().take(60).collect(),
            ));
        }
    }
    out
}

fn row(group: &str, id: &str, title: &str, description: &str, control: Control) -> RowView {
    RowView {
        id: id.into(),
        title: title.into(),
        group: group.into(),
        description: description.into(),
        overridden: false,
        control,
    }
}

fn speed(value: Option<f64>) -> Control {
    Control::Slider {
        min: -1.0,
        max: 1.0,
        integer: false,
        value: value.unwrap_or(0.0),
    }
}

fn choice(options: &[(&str, &str)], current: &str) -> Control {
    Control::Choice {
        options: options
            .iter()
            .map(|(value, label)| Choice {
                value: (*value).into(),
                label: (*label).into(),
            })
            .collect(),
        current: current.into(),
    }
}

/// Touchpad and mouse.
pub fn pointer_rows(input: &InputSettings) -> Vec<RowView> {
    let on = |v: Option<bool>| Control::Switch {
        on: v.unwrap_or(false),
    };
    vec![
        row("Touchpad", "touchpad:tap", "Tap to click", "A light tap counts as a click.", on(input.touchpad_tap)),
        row(
            "Touchpad",
            "touchpad:natural",
            "Natural scrolling",
            "The page follows your fingers, as on a phone.",
            on(input.touchpad_natural_scroll),
        ),
        row("Touchpad", "touchpad:speed", "Pointer speed", "Slower on the left, faster on the right.", speed(input.touchpad_speed)),
        row(
            "Touchpad",
            "touchpad:dwt",
            "Ignore it while typing",
            "Stops a palm resting on the touchpad from moving the pointer.",
            on(input.touchpad_disable_while_typing),
        ),
        row(
            "Touchpad",
            "touchpad:click",
            "Clicking",
            "Fingers: one finger is the main button, two the other. Areas: the bottom corners are the buttons.",
            choice(
                &[("clickfinger", "Fingers"), ("button-areas", "Areas")],
                input.touchpad_click_method.as_deref().unwrap_or("clickfinger"),
            ),
        ),
        row(
            "Touchpad",
            "touchpad:scroll",
            "Scrolling",
            "Two fingers anywhere, or one finger along the right edge.",
            choice(
                &[("two-finger", "Two fingers"), ("edge", "Edge")],
                input.touchpad_scroll_method.as_deref().unwrap_or("two-finger"),
            ),
        ),
        row("Touchpad", "touchpad:drag", "Tap and drag", "Tap, then keep your finger down and move to drag.", on(input.touchpad_tap_and_drag)),
        row("Touchpad", "touchpad:drag-lock", "Drag lock", "A drag carries on when you lift your finger, until you tap again.", on(input.touchpad_drag_lock)),
        row("Touchpad", "touchpad:off-with-mouse", "Off while a mouse is plugged in", "", on(input.touchpad_off_with_mouse)),
        row("Mouse", "mouse:natural", "Natural scrolling", "The wheel scrolls the other way.", on(input.mouse_natural_scroll)),
        row("Mouse", "mouse:speed", "Pointer speed", "", speed(input.mouse_speed)),
        row(
            "Mouse",
            "mouse:accel",
            "Acceleration",
            "Adaptive: the pointer goes further when you move it faster. Flat: it goes exactly as far as the mouse does.",
            choice(
                &[("adaptive", "Adaptive"), ("flat", "Flat")],
                input.mouse_accel_profile.as_deref().unwrap_or("adaptive"),
            ),
        ),
        row("Mouse", "mouse:left", "Primary button on the right", "For left-handed use.", on(input.mouse_left_handed)),
    ]
}

/// The layout, and how fast a held key repeats.
pub fn typing_rows(input: &InputSettings, layouts: &[(String, String)]) -> Vec<RowView> {
    let current = input.keyboard_layout.clone().unwrap_or_else(|| "us".into());
    let mut options: Vec<Choice> = layouts
        .iter()
        .map(|(code, description)| Choice {
            value: code.clone(),
            label: if description.is_empty() {
                code.clone()
            } else {
                format!("{description} ({code})")
            },
        })
        .collect();
    if !options.iter().any(|o| o.value == current) {
        options.push(Choice {
            value: current.clone(),
            label: current.clone(),
        });
    }
    vec![
        row(
            "Typing",
            "typing:layout",
            "Layout",
            "Which letters the keys make. A setup with several layouts in config.kdl shows here as it is written.",
            Control::Choice { options, current },
        ),
        row(
            "Typing",
            "typing:delay",
            "Repeat delay (ms)",
            "How long a key is held before it starts repeating.",
            Control::Slider { min: 150.0, max: 1000.0, integer: true, value: f64::from(input.keyboard_repeat_delay.unwrap_or(600)) },
        ),
        row(
            "Typing",
            "typing:rate",
            "Repeat rate (per second)",
            "How fast it repeats once it has started.",
            Control::Slider { min: 2.0, max: 60.0, integer: true, value: f64::from(input.keyboard_repeat_rate.unwrap_or(25)) },
        ),
    ]
}

/// The request a row's change makes, or why it must not be made. Only the ids above, only values in their ranges.
pub fn parse_change(id: &str, change: &Value) -> Result<InputSettings, String> {
    let kind = change.get("kind").and_then(Value::as_str);
    let value = change.get("value");
    let switch = || {
        value
            .and_then(Value::as_bool)
            .ok_or("not a boolean".to_string())
    };
    let number = || {
        value
            .and_then(Value::as_f64)
            .filter(|n| n.is_finite())
            .ok_or("not a number".to_string())
    };
    let text = || value.and_then(Value::as_str).ok_or("not text".to_string());
    let mut out = InputSettings::default();
    match (id, kind) {
        ("touchpad:tap", Some("switch")) => out.touchpad_tap = Some(switch()?),
        ("touchpad:natural", Some("switch")) => out.touchpad_natural_scroll = Some(switch()?),
        ("touchpad:speed", Some("number")) => out.touchpad_speed = Some(number()?.clamp(-1.0, 1.0)),
        ("touchpad:dwt", Some("switch")) => out.touchpad_disable_while_typing = Some(switch()?),
        ("touchpad:click", Some("choice")) => out.touchpad_click_method = Some(text()?.to_string()),
        ("touchpad:scroll", Some("choice")) => {
            out.touchpad_scroll_method = Some(text()?.to_string())
        }
        ("touchpad:drag", Some("switch")) => out.touchpad_tap_and_drag = Some(switch()?),
        ("touchpad:drag-lock", Some("switch")) => out.touchpad_drag_lock = Some(switch()?),
        ("touchpad:off-with-mouse", Some("switch")) => {
            out.touchpad_off_with_mouse = Some(switch()?)
        }
        ("mouse:natural", Some("switch")) => out.mouse_natural_scroll = Some(switch()?),
        ("mouse:speed", Some("number")) => out.mouse_speed = Some(number()?.clamp(-1.0, 1.0)),
        ("mouse:accel", Some("choice")) => out.mouse_accel_profile = Some(text()?.to_string()),
        ("mouse:left", Some("switch")) => out.mouse_left_handed = Some(switch()?),
        ("typing:layout", Some("choice")) => out.keyboard_layout = Some(text()?.to_string()),
        ("typing:delay", Some("number")) => {
            out.keyboard_repeat_delay = Some(number()?.round().clamp(150.0, 1000.0) as u16)
        }
        ("typing:rate", Some("number")) => {
            out.keyboard_repeat_rate = Some(number()?.round().clamp(2.0, 60.0) as u8)
        }
        _ => return Err("unknown change".into()),
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn layouts_come_from_the_layout_section_only() {
        let text = "! model\n  pc105  Generic 105-key\n\n! layout\n  us              English (US)\n  de              German\n  ara             Arabic\n\n! variant\n  euro   us: English (US, euro on 5)\n";
        assert_eq!(
            parse_layouts(text),
            [
                ("us".into(), "English (US)".into()),
                ("de".into(), "German".into()),
                ("ara".into(), "Arabic".into())
            ]
        );
        assert!(parse_layouts("").is_empty());
    }

    #[test]
    fn the_rows_show_what_the_compositor_reports() {
        let input = InputSettings {
            touchpad_tap: Some(true),
            touchpad_speed: Some(0.3),
            mouse_accel_profile: Some("flat".into()),
            keyboard_layout: Some("us,de".into()),
            keyboard_repeat_rate: Some(30),
            ..InputSettings::default()
        };
        let pointer = pointer_rows(&input);
        assert!(matches!(pointer[0].control, Control::Switch { on: true }));
        assert!(
            matches!(&pointer[11].control, Control::Choice { current, .. } if current == "flat")
        );
        let typing = typing_rows(&input, &[("us".into(), "English (US)".into())]);
        match &typing[0].control {
            Control::Choice { current, options } => {
                assert_eq!(current, "us,de");
                assert!(
                    options.iter().any(|o| o.value == "us,de"),
                    "a setup written by hand still shows"
                );
            }
            _ => panic!("layout is a choice"),
        }
        assert!(matches!(typing[2].control, Control::Slider { value, .. } if value == 30.0));
    }

    #[test]
    fn a_change_is_one_field_and_the_numbers_are_kept_in_range() {
        let set = |id: &str, kind: &str, value: Value| {
            parse_change(id, &json!({"kind": kind, "value": value}))
        };
        assert_eq!(
            set("touchpad:tap", "switch", json!(true))
                .unwrap()
                .touchpad_tap,
            Some(true)
        );
        assert_eq!(
            set("mouse:speed", "number", json!(5.0))
                .unwrap()
                .mouse_speed,
            Some(1.0)
        );
        assert_eq!(
            set("typing:delay", "number", json!(10))
                .unwrap()
                .keyboard_repeat_delay,
            Some(150)
        );
        assert_eq!(
            set("typing:rate", "number", json!(30.4))
                .unwrap()
                .keyboard_repeat_rate,
            Some(30)
        );
        assert!(set("touchpad:tap", "choice", json!("x")).is_err());
        assert!(set("mouse:speed", "number", json!("fast")).is_err());
        assert!(set("hostname", "text", json!("x")).is_err());
        let one = set("typing:layout", "choice", json!("de")).unwrap();
        assert_eq!(
            one,
            InputSettings {
                keyboard_layout: Some("de".into()),
                ..InputSettings::default()
            }
        );
    }
}
