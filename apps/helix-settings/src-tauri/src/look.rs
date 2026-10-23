//! The Appearance rows the compositor holds: animations, the space between windows, the cursor size, and the focus ring's colour (which follows the
//! accent). Like the input settings they are applied by the Helix compositor at once and remembered by it; another compositor reports nothing.

use helix_compositor::compositor_ipc::LookSettings;
use helix_config::catalog::{Choice, Control, RowView};
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

fn corner_row(id: &str, title: &str, description: &str, control: Control) -> RowView {
    RowView {
        id: id.into(),
        title: title.into(),
        group: "Corners and glass".into(),
        description: description.into(),
        overridden: false,
        control,
    }
}

/// Corner roundness of the shell's own parts and of everything inside Helix applications, each on its own: a percent, 0 (square) to 200, 100 as designed.
pub fn shape_rows(shape: &helix_config::shape_prefs::Shape) -> Vec<RowView> {
    let percent = |scale: f64| Control::Slider {
        min: 0.0,
        max: 200.0,
        integer: true,
        value: (scale * 100.0).round(),
    };
    vec![
        corner_row("shape:bar", "Top bar", "The bar and its buttons. 100% is as designed, 0 is square.", percent(shape.bar)),
        corner_row("shape:dock", "Dock", "The dock, its icons and its menu.", percent(shape.dock)),
        corner_row("shape:panels", "Panels", "The launcher, control centre, calendar and the other panels, and what is in them.", percent(shape.panels)),
        corner_row("shape:toasts", "Notifications", "Banners and the notification list.", percent(shape.toasts)),
        corner_row("shape:apps", "Inside Helix apps", "Buttons, cards, fields and sheets in Settings, Mobius and the other Helix applications.", percent(shape.apps)),
    ]
}

/// A change to a window's corners, merged with what is written down: Helix applications' windows follow the windows' radius until they were given one of their
/// own; the request sent to the compositor always carries both, so the two rules never disagree with the file.
pub fn merge_window_corners(
    shape: &mut helix_config::shape_prefs::Shape,
    request: &mut LookSettings,
) {
    if let Some(radius) = request.window_corner_radius {
        shape.windows = radius;
    }
    if let Some(radius) = request.helix_window_corner_radius {
        shape.helix_windows = Some(radius);
    }
    request.window_corner_radius = Some(shape.windows);
    request.helix_window_corner_radius = Some(shape.helix_windows_effective());
}

/// Clear to Tinted, like iOS: how see-through the bar, dock and panels are. A percent, 0 (clear) to 100 (tinted).
pub fn glass_row(glass: f64) -> RowView {
    RowView {
        id: "look:glass".into(),
        title: "Glass".into(),
        group: "Corners and glass".into(),
        description: "Clear shows more of what is behind the bar, dock and panels. Tinted is more opaque and easier to read.".into(),
        overridden: false,
        control: Control::Slider { min: 0.0, max: 100.0, integer: true, value: (glass * 100.0).round() },
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
        corner_row(
            "look:corner-windows",
            "Windows",
            "How round the corners of every window are, in pixels.",
            slider(0.0, 32.0, f64::from(look.window_corner_radius.unwrap_or(12))),
        ),
        corner_row(
            "look:corner-helix",
            "Helix app windows",
            "Settings, Mobius and the other Helix applications. They follow Windows until you change this.",
            slider(
                0.0,
                32.0,
                f64::from(look.helix_window_corner_radius.or(look.window_corner_radius).unwrap_or(12)),
            ),
        ),
        row(
            "look:cursor",
            "Cursor size",
            "In pixels. Applies as soon as you let go.",
            slider(16.0, 64.0, f64::from(look.cursor_size.unwrap_or(24))),
        ),
    ]
}

/// Multitasking: how windows and workspaces behave. The same compositor overrides as the look, on their own page.
pub fn multitask_rows(look: &LookSettings) -> Vec<RowView> {
    let row = |id: &str, title: &str, group: &str, description: &str, control: Control| RowView {
        id: id.into(),
        title: title.into(),
        group: group.into(),
        description: description.into(),
        overridden: false,
        control,
    };
    let on = |v: Option<bool>, default: bool| Control::Switch {
        on: v.unwrap_or(default),
    };
    vec![
        row(
            "mt:focus",
            "Focus follows the pointer",
            "Windows",
            "The window under the pointer gets the keyboard without a click.",
            on(look.focus_follows_mouse, false),
        ),
        row(
            "mt:center",
            "Centre the focused column",
            "Windows",
            "Never leaves the row where it is. Always keeps what you are working on in the middle. When needed centres only when the next column would be cut off.",
            Control::Choice {
                options: [("never", "Never"), ("always", "Always"), ("on-overflow", "When needed")]
                    .iter()
                    .map(|(value, label)| Choice { value: (*value).into(), label: (*label).into() })
                    .collect(),
                current: look.center_focused.clone().unwrap_or_else(|| "never".into()),
            },
        ),
        row(
            "mt:corner",
            "Hot corner",
            "Workspaces",
            "Push the pointer into the top-left corner to see all your workspaces and windows.",
            on(look.hot_corner, true),
        ),
        row(
            "mt:back",
            "Pressing the current workspace goes back",
            "Workspaces",
            "Switching to the workspace you are already on returns to the one before it.",
            on(look.workspace_back_and_forth, false),
        ),
    ]
}

pub fn parse_multitask_change(id: &str, change: &Value) -> Result<LookSettings, String> {
    let kind = change.get("kind").and_then(Value::as_str);
    let value = change.get("value");
    let switch = || {
        value
            .and_then(Value::as_bool)
            .ok_or("not a boolean".to_string())
    };
    let mut out = LookSettings::default();
    match (id, kind) {
        ("mt:focus", Some("switch")) => out.focus_follows_mouse = Some(switch()?),
        ("mt:corner", Some("switch")) => out.hot_corner = Some(switch()?),
        ("mt:back", Some("switch")) => out.workspace_back_and_forth = Some(switch()?),
        ("mt:center", Some("choice")) => {
            let mode = value.and_then(Value::as_str).ok_or("not text")?;
            if !matches!(mode, "never" | "always" | "on-overflow") {
                return Err("pick when to centre".into());
            }
            out.center_focused = Some(mode.to_string());
        }
        _ => return Err("unknown change".into()),
    }
    Ok(out)
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
        ("look:corner-windows", Some("number")) => {
            out.window_corner_radius = Some(number()?.round().clamp(0.0, 32.0) as u16)
        }
        ("look:corner-helix", Some("number")) => {
            out.helix_window_corner_radius = Some(number()?.round().clamp(0.0, 32.0) as u16)
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
            window_corner_radius: Some(18),
            focus_color: None,
            ..LookSettings::default()
        });
        assert!(matches!(rows[0].control, Control::Switch { on: false }));
        assert!(matches!(rows[1].control, Control::Slider { value, .. } if value == 4.0));
        assert!(
            matches!(rows[2].control, Control::Slider { value, .. } if value == 18.0),
            "windows"
        );
        assert!(
            matches!(rows[3].control, Control::Slider { value, .. } if value == 18.0),
            "Helix windows follow until told otherwise"
        );
        assert!(
            matches!(rows[4].control, Control::Slider { value, .. } if value == 40.0),
            "cursor"
        );
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
        assert_eq!(
            set("look:corner-windows", "number", json!(6))
                .unwrap()
                .window_corner_radius,
            Some(6)
        );
        assert_eq!(
            set("look:corner-helix", "number", json!(99))
                .unwrap()
                .helix_window_corner_radius,
            Some(32)
        );
        assert!(set("look:corner-windows", "number", json!("x")).is_err());
        let shape_rows = shape_rows(&helix_config::shape_prefs::Shape::default());
        assert_eq!(shape_rows.len(), 5);
        assert!(shape_rows
            .iter()
            .all(|r| matches!(r.control, Control::Slider { value, .. } if value == 100.0)));
        assert!(set("look:gaps", "number", json!("x")).is_err());
        assert!(set("look:nope", "switch", json!(true)).is_err());
    }

    #[test]
    fn helix_windows_follow_the_windows_until_they_have_a_radius_of_their_own() {
        use helix_config::shape_prefs::Shape;
        let change = |all: Option<u16>, helix: Option<u16>| LookSettings {
            window_corner_radius: all,
            helix_window_corner_radius: helix,
            ..LookSettings::default()
        };
        let mut shape = Shape::default();
        let mut request = change(Some(20), None);
        merge_window_corners(&mut shape, &mut request);
        assert_eq!(
            (
                request.window_corner_radius,
                request.helix_window_corner_radius,
                shape.helix_windows
            ),
            (Some(20), Some(20), None)
        );
        let mut request = change(None, Some(2));
        merge_window_corners(&mut shape, &mut request);
        assert_eq!(
            (
                request.window_corner_radius,
                request.helix_window_corner_radius,
                shape.helix_windows
            ),
            (Some(20), Some(2), Some(2))
        );
        let mut request = change(Some(8), None);
        merge_window_corners(&mut shape, &mut request);
        assert_eq!(
            (
                request.window_corner_radius,
                request.helix_window_corner_radius,
                shape.windows
            ),
            (Some(8), Some(2), 8),
            "their own choice stays"
        );
    }

    #[test]
    fn multitasking_rows_and_changes() {
        let rows = multitask_rows(&LookSettings {
            focus_follows_mouse: Some(true),
            center_focused: Some("always".into()),
            hot_corner: Some(false),
            ..LookSettings::default()
        });
        assert!(matches!(rows[0].control, Control::Switch { on: true }));
        assert!(matches!(&rows[1].control, Control::Choice { current, .. } if current == "always"));
        assert!(matches!(rows[2].control, Control::Switch { on: false }));
        let set = |id: &str, kind: &str, v: Value| {
            parse_multitask_change(id, &json!({"kind": kind, "value": v}))
        };
        assert_eq!(
            set("mt:corner", "switch", json!(false)).unwrap().hot_corner,
            Some(false)
        );
        assert_eq!(
            set("mt:center", "choice", json!("on-overflow"))
                .unwrap()
                .center_focused
                .as_deref(),
            Some("on-overflow")
        );
        assert!(set("mt:center", "choice", json!("sometimes")).is_err());
        assert!(set("mt:focus", "switch", json!("yes")).is_err());
        assert!(set("look:gaps", "number", json!(3)).is_err());
    }

    #[test]
    fn every_accent_has_a_ring_colour() {
        for name in helix_config::settings::ACCENTS {
            assert!(accent_ring(name).is_some(), "{name}");
        }
        assert!(accent_ring("teal").is_none());
    }
}
