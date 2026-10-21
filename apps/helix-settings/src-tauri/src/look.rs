//! The Appearance rows the compositor holds: animations, the space between windows, the cursor size, and the focus ring's colour (which follows the
//! accent). Like the input settings they are applied by the Helix compositor at once and remembered by it; another compositor reports nothing.

use helix_compositor::compositor_ipc::LookSettings;
use helix_config::catalog::{Control, RowView};
use serde_json::Value;

/// The accent colours (name, `#rrggbb`) as the dark theme draws them; the ring is drawn on dark and light windows alike.
const ACCENT_COLORS: &[(&str, &str)] = &[
    ("blue", "#0a84ff"),
    ("purple", "#bf5af2"),
    ("pink", "#ff375f"),
    ("red", "#ff453a"),
    ("orange", "#ff9f0a"),
    ("yellow", "#ffd60a"),
    ("green", "#30d158"),
    ("graphite", "#98989d"),
];

/// What the focus ring becomes when the accent colour is `name`.
pub fn accent_ring(name: &str) -> Option<LookSettings> {
    ACCENT_COLORS
        .iter()
        .find(|(accent, _)| *accent == name)
        .map(|(_, hex)| LookSettings {
            focus_color: Some((*hex).into()),
            ..LookSettings::default()
        })
}

fn row(id: &str, title: &str, description: &str, control: Control) -> RowView {
    RowView {
        id: id.into(),
        title: title.into(),
        group: "Windows and cursor".into(),
        description: description.into(),
        overridden: false,
        control,
    }
}

pub fn look_rows(look: &LookSettings) -> Vec<RowView> {
    let slider = |min: f64, max: f64, value: f64| Control::Slider {
        min,
        max,
        integer: true,
        value,
    };
    vec![
        row(
            "look:animations",
            "Animations",
            "Windows and workspaces glide into place. Turn off for a plainer, snappier desktop.",
            Control::Switch {
                on: look.animations.unwrap_or(true),
            },
        ),
        row(
            "look:gaps",
            "Space between windows",
            "In pixels. Zero makes windows touch.",
            slider(0.0, 64.0, f64::from(look.window_gaps.unwrap_or(16))),
        ),
        row(
            "look:cursor",
            "Cursor size",
            "In pixels. Applies as soon as you let go.",
            slider(16.0, 64.0, f64::from(look.cursor_size.unwrap_or(24))),
        ),
    ]
}

pub fn parse_change(id: &str, change: &Value) -> Result<LookSettings, String> {
    let kind = change.get("kind").and_then(Value::as_str);
    let value = change.get("value");
    let number = || {
        value
            .and_then(Value::as_f64)
            .filter(|n| n.is_finite())
            .ok_or("not a number".to_string())
    };
    let mut out = LookSettings::default();
    match (id, kind) {
        ("look:animations", Some("switch")) => {
            out.animations = Some(value.and_then(Value::as_bool).ok_or("not a boolean")?)
        }
        ("look:gaps", Some("number")) => {
            out.window_gaps = Some(number()?.round().clamp(0.0, 64.0) as u16)
        }
        ("look:cursor", Some("number")) => {
            out.cursor_size = Some(number()?.round().clamp(16.0, 64.0) as u8)
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
    fn rows_show_what_the_compositor_reports() {
        let rows = look_rows(&LookSettings {
            animations: Some(false),
            window_gaps: Some(4),
            cursor_size: Some(40),
            focus_color: None,
        });
        assert!(matches!(rows[0].control, Control::Switch { on: false }));
        assert!(matches!(rows[1].control, Control::Slider { value, .. } if value == 4.0));
        assert!(matches!(rows[2].control, Control::Slider { value, .. } if value == 40.0));
    }

    #[test]
    fn numbers_are_kept_in_range_and_unknown_rows_refused() {
        let set =
            |id: &str, kind: &str, v: Value| parse_change(id, &json!({"kind": kind, "value": v}));
        assert_eq!(
            set("look:gaps", "number", json!(500)).unwrap().window_gaps,
            Some(64)
        );
        assert_eq!(
            set("look:cursor", "number", json!(3)).unwrap().cursor_size,
            Some(16)
        );
        assert_eq!(
            set("look:animations", "switch", json!(false))
                .unwrap()
                .animations,
            Some(false)
        );
        assert!(set("look:gaps", "number", json!("x")).is_err());
        assert!(set("look:nope", "switch", json!(true)).is_err());
    }

    #[test]
    fn every_accent_has_a_ring_colour() {
        for name in helix_config::settings::ACCENTS {
            assert!(accent_ring(name).is_some(), "{name}");
        }
        assert!(accent_ring("teal").is_none());
    }
}
