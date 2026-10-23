//! The Appearance rows the compositor holds: animations, the space between windows, the cursor size, and the focus ring's colour (which follows the
//! accent). Like the input settings they are applied by the Helix compositor at once and remembered by it; another compositor reports nothing.

use helix_compositor::compositor_ipc::{AppRule, LookSettings};
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

fn effect_row(id: &str, title: &str, description: &str, control: Control) -> RowView {
    RowView {
        id: id.into(),
        title: title.into(),
        group: "Effects".into(),
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

/// A change to every window's corners, written down with the rest so everything agrees.
pub fn merge_window_corners(shape: &mut helix_config::shape_prefs::Shape, request: &LookSettings) {
    if let Some(radius) = request.window_corner_radius {
        shape.windows = radius;
    }
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
            "How round the corners of every window are, in pixels: Helix applications and all the others.",
            slider(0.0, 32.0, f64::from(look.window_corner_radius.unwrap_or(12))),
        ),
        row(
            "look:cursor",
            "Cursor size",
            "In pixels. Applies as soon as you let go.",
            slider(16.0, 64.0, f64::from(look.cursor_size.unwrap_or(24))),
        ),
        effect_row(
            "look:anim-speed",
            "Animation speed",
            "How fast windows and workspaces move, in percent. 100 is as designed, 200 twice as fast, 50 half as fast.",
            slider(25.0, 400.0, f64::from(look.animation_speed.unwrap_or(100))),
        ),
        effect_row(
            "look:blur",
            "Blur strength",
            "How strongly the glass blurs what is behind it. Higher is softer and costs a little more power.",
            slider(0.0, 100.0, f64::from(look.blur_strength.unwrap_or(50))),
        ),
        effect_row(
            "look:shadows",
            "Window shadows",
            "A soft shadow under each window.",
            Control::Switch { on: look.window_shadows.unwrap_or(false) },
        ),
        effect_row(
            "look:ring",
            "Focus ring width",
            "The ring around the window you are working in, in pixels. 0 hides it.",
            slider(0.0, 8.0, f64::from(look.focus_ring_width.unwrap_or(2))),
        ),
    ]
    // Refraction is only offered when the shell's glass rule is the compositor's own (a rule of the person's is theirs to edit).
    .into_iter()
    .chain(look.refraction.map(|percent| {
        effect_row(
            "look:refraction",
            "Glass refraction",
            "How much the bar, dock and panels bend what is behind their edges, in percent. 0 is flat glass.",
            slider(0.0, 200.0, f64::from(percent)),
        )
    }))
    .collect()
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
            "mt:warp",
            "Pointer follows the keyboard focus",
            "Windows",
            "When a shortcut moves focus to another window, the pointer jumps onto it.",
            on(look.warp_mouse_to_focus, false),
        ),
        row(
            "mt:mod",
            "Shortcut key",
            "Windows",
            "The key the window shortcuts are built on. Super is the default; Alt or Ctrl suit keyboards without one but can clash with apps.",
            Control::Choice {
                options: [("super", "Super"), ("alt", "Alt"), ("ctrl", "Ctrl")]
                    .iter()
                    .map(|(value, label)| Choice { value: (*value).into(), label: (*label).into() })
                    .collect(),
                current: look.mod_key.clone().unwrap_or_else(|| "super".into()),
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

/// One group of rows for each application that has a rule or has a window open now: how its windows open, whether it is hidden from screen sharing,
/// and how solid it is.
pub fn app_rows(look: &LookSettings, running: &[String]) -> Vec<RowView> {
    let rules = look.app_rules.clone().unwrap_or_default();
    let mut apps: Vec<String> = rules.iter().map(|r| r.app_id.clone()).collect();
    for id in running {
        if !apps.contains(id) && valid_app_id(id) {
            apps.push(id.clone());
        }
    }
    apps.sort_by_key(|a| a.to_lowercase());
    apps.into_iter()
        .flat_map(|app| {
            let rule = rules.iter().find(|r| r.app_id == app).cloned().unwrap_or_default();
            let group = format!("Application: {app}");
            let row = |kind: &str, title: &str, description: &str, control: Control| RowView {
                id: format!("app:{app}:{kind}"),
                title: title.into(),
                group: group.clone(),
                description: description.into(),
                overridden: false,
                control,
            };
            vec![
                row(
                    "open",
                    "Opens",
                    "How new windows of this application start. Windows already open stay as they are.",
                    Control::Choice {
                        options: [("default", "As the layout does"), ("floating", "Floating"), ("maximized", "Maximized"), ("fullscreen", "Fullscreen")]
                            .iter()
                            .map(|(value, label)| Choice { value: (*value).into(), label: (*label).into() })
                            .collect(),
                        current: rule.open.clone().unwrap_or_else(|| "default".into()),
                    },
                ),
                row(
                    "private",
                    "Hide from screen sharing",
                    "Its windows show as black boxes in recordings, screenshots and shared screens.",
                    Control::Switch { on: rule.private },
                ),
                row(
                    "opacity",
                    "Opacity",
                    "How solid its windows are, in percent.",
                    Control::Slider { min: 20.0, max: 100.0, integer: true, value: f64::from(rule.opacity.unwrap_or(100)) },
                ),
            ]
        })
        .collect()
}

fn valid_app_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 128
        && id
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_'))
}

/// A change to one application's rule, as the whole new list (the compositor replaces it). `None` when `id` is not an application row.
pub fn parse_app_change(
    id: &str,
    change: &Value,
    current: &[AppRule],
) -> Option<Result<LookSettings, String>> {
    let rest = id.strip_prefix("app:")?;
    let (app, kind) = rest.rsplit_once(':')?;
    Some((|| {
        if !valid_app_id(app) {
            return Err("unknown application".to_string());
        }
        let mut rules = current.to_vec();
        let at = match rules.iter().position(|r| r.app_id == app) {
            Some(at) => at,
            None => {
                rules.push(AppRule {
                    app_id: app.into(),
                    ..AppRule::default()
                });
                rules.len() - 1
            }
        };
        let value = change.get("value");
        match kind {
            "open" => {
                let open = value.and_then(Value::as_str).ok_or("not text")?;
                rules[at].open = match open {
                    "" | "default" => None,
                    "floating" | "maximized" | "fullscreen" => Some(open.to_string()),
                    _ => return Err("pick how it opens".into()),
                };
            }
            "private" => {
                rules[at].private = value.and_then(Value::as_bool).ok_or("not a boolean")?
            }
            "opacity" => {
                let percent = value
                    .and_then(Value::as_f64)
                    .filter(|n| n.is_finite())
                    .ok_or("not a number")?;
                let percent = percent.round().clamp(20.0, 100.0) as u8;
                rules[at].opacity = (percent < 100).then_some(percent);
            }
            _ => return Err("unknown change".into()),
        }
        // A rule that says nothing is no rule.
        rules.retain(|r| r.open.is_some() || r.private || r.opacity.is_some());
        Ok(LookSettings {
            app_rules: Some(rules),
            ..LookSettings::default()
        })
    })())
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
        ("mt:warp", Some("switch")) => out.warp_mouse_to_focus = Some(switch()?),
        ("mt:mod", Some("choice")) => {
            let key = value.and_then(Value::as_str).ok_or("not text")?;
            if !matches!(key, "super" | "alt" | "ctrl") {
                return Err("pick a key".into());
            }
            out.mod_key = Some(key.to_string());
        }
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
        ("look:anim-speed", Some("number")) => {
            out.animation_speed = Some(number()?.round().clamp(25.0, 400.0) as u16)
        }
        ("look:blur", Some("number")) => {
            out.blur_strength = Some(((number()? / 25.0).round() * 25.0).clamp(0.0, 100.0) as u8)
        }
        ("look:refraction", Some("number")) => {
            out.refraction = Some(number()?.round().clamp(0.0, 200.0) as u16)
        }
        ("look:shadows", Some("switch")) => {
            out.window_shadows = Some(value.and_then(Value::as_bool).ok_or("not a boolean")?)
        }
        ("look:ring", Some("number")) => {
            out.focus_ring_width = Some(number()?.round().clamp(0.0, 8.0) as u8)
        }
        ("look:corner-windows", Some("number")) => {
            out.window_corner_radius = Some(number()?.round().clamp(0.0, 32.0) as u16)
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
            matches!(rows[3].control, Control::Slider { value, .. } if value == 40.0),
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
    fn application_rows_and_changes() {
        let look = LookSettings {
            app_rules: Some(vec![AppRule {
                app_id: "signal".into(),
                private: true,
                ..AppRule::default()
            }]),
            ..LookSettings::default()
        };
        let rows = app_rows(
            &look,
            &[
                "org.gnome.Nautilus".into(),
                "signal".into(),
                "bad id".into(),
            ],
        );
        let ids: Vec<&str> = rows.iter().map(|r| r.id.as_str()).collect();
        assert_eq!(
            ids,
            [
                "app:org.gnome.Nautilus:open",
                "app:org.gnome.Nautilus:private",
                "app:org.gnome.Nautilus:opacity",
                "app:signal:open",
                "app:signal:private",
                "app:signal:opacity"
            ]
        );
        assert!(matches!(rows[4].control, Control::Switch { on: true }));
        let current = look.app_rules.clone().unwrap();
        let set = |id: &str, kind: &str, v: Value| {
            parse_app_change(id, &json!({"kind": kind, "value": v}), &current).unwrap()
        };
        let rules = set("app:org.gnome.Nautilus:open", "choice", json!("floating"))
            .unwrap()
            .app_rules
            .unwrap();
        assert_eq!(rules.len(), 2);
        assert_eq!(rules[1].open.as_deref(), Some("floating"));
        // Turning the last thing off removes the rule.
        let rules = set("app:signal:private", "switch", json!(false))
            .unwrap()
            .app_rules
            .unwrap();
        assert!(rules.is_empty());
        let rules = set("app:signal:opacity", "number", json!(5))
            .unwrap()
            .app_rules
            .unwrap();
        assert_eq!(rules[0].opacity, Some(20));
        assert!(set("app:x y:open", "choice", json!("floating")).is_err());
        assert!(set("app:signal:open", "choice", json!("tiny")).is_err());
        assert!(parse_app_change("mt:focus", &json!({}), &current).is_none());
    }

    #[test]
    fn effects_rows_and_changes() {
        let rows = look_rows(&LookSettings {
            animation_speed: Some(150),
            blur_strength: Some(80),
            refraction: Some(35),
            window_shadows: Some(true),
            focus_ring_width: Some(4),
            ..LookSettings::default()
        });
        let effects: Vec<&RowView> = rows.iter().filter(|r| r.group == "Effects").collect();
        assert_eq!(
            effects.iter().map(|r| r.id.as_str()).collect::<Vec<_>>(),
            [
                "look:anim-speed",
                "look:blur",
                "look:shadows",
                "look:ring",
                "look:refraction"
            ]
        );
        assert!(
            matches!(effects[0].control, Control::Slider { value, min, max, .. } if value == 150.0 && min == 25.0 && max == 400.0)
        );
        assert!(matches!(effects[2].control, Control::Switch { on: true }));
        // No refraction row when the compositor does not report one (the person has their own glass rule).
        let none = look_rows(&LookSettings::default());
        assert!(!none.iter().any(|r| r.id == "look:refraction"));
        let set =
            |id: &str, kind: &str, v: Value| parse_change(id, &json!({"kind": kind, "value": v}));
        assert_eq!(
            set("look:anim-speed", "number", json!(1000))
                .unwrap()
                .animation_speed,
            Some(400)
        );
        assert_eq!(
            set("look:anim-speed", "number", json!(1))
                .unwrap()
                .animation_speed,
            Some(25)
        );
        assert_eq!(
            set("look:blur", "number", json!(70)).unwrap().blur_strength,
            Some(75)
        );
        assert_eq!(
            set("look:refraction", "number", json!(250))
                .unwrap()
                .refraction,
            Some(200)
        );
        assert_eq!(
            set("look:shadows", "switch", json!(true))
                .unwrap()
                .window_shadows,
            Some(true)
        );
        assert_eq!(
            set("look:ring", "number", json!(20))
                .unwrap()
                .focus_ring_width,
            Some(8)
        );
        assert!(set("look:ring", "switch", json!(true)).is_err());
    }

    #[test]
    fn the_windows_radius_is_written_down_with_the_rest() {
        use helix_config::shape_prefs::Shape;
        let mut shape = Shape::default();
        merge_window_corners(
            &mut shape,
            &LookSettings {
                window_corner_radius: Some(20),
                ..LookSettings::default()
            },
        );
        assert_eq!(shape.windows, 20);
        merge_window_corners(&mut shape, &LookSettings::default());
        assert_eq!(shape.windows, 20, "a change to something else leaves it");
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
