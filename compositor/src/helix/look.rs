//! Helix look settings: animations on or off, the gap between windows, the cursor size, and the colour of the focus ring (which follows the accent
//! colour). Like the input settings they are *overrides* over `config.kdl`: the person's file is never rewritten, the overrides live in
//! `$XDG_STATE_HOME/helix/look-overrides.json`, are applied every time the configuration is loaded, and take effect at once when set over IPC.

use std::path::PathBuf;

use niri_config::appearance::Color;
use niri_config::appearance::CornerRadius;
use niri_config::appearance::{BackgroundEffectRule, LiquidGlassPart};
use niri_config::input::FocusFollowsMouse;
use niri_config::layer_rule::{LayerRule, Match as LayerMatch};
use niri_config::layout::CenterFocusedColumn;
use niri_config::utils::RegexEq;
use niri_config::window_rule::WindowRule;
use niri_config::Config;
use niri_ipc::HelixLook;

use crate::niri::State;

fn file() -> PathBuf {
    let base = std::env::var_os("XDG_STATE_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/state")))
        .unwrap_or_default();
    base.join("helix/look-overrides.json")
}

fn load() -> HelixLook {
    std::fs::read_to_string(file())
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or_default()
}

fn save(look: &HelixLook) -> Result<(), String> {
    let path = file();
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let temporary = path.with_extension("json.tmp");
    let text = serde_json::to_string_pretty(look).map_err(|e| e.to_string())?;
    std::fs::write(&temporary, text).map_err(|e| e.to_string())?;
    std::fs::rename(&temporary, &path).map_err(|e| e.to_string())
}

/// `#rrggbb` as its three bytes.
fn parse_hex(text: &str) -> Option<[u8; 3]> {
    let hex = text.strip_prefix('#')?;
    (hex.len() == 6 && hex.bytes().all(|b| b.is_ascii_hexdigit())).then(|| {
        let byte = |i: usize| u8::from_str_radix(&hex[i..i + 2], 16).unwrap_or(0);
        [byte(0), byte(2), byte(4)]
    })
}

pub fn validate(look: &HelixLook) -> Result<(), String> {
    if look.window_gaps.is_some_and(|g| g > 64) {
        return Err("gaps are 0 to 64".into());
    }
    if look.cursor_size.is_some_and(|s| !(16..=64).contains(&s)) {
        return Err("the cursor is 16 to 64 pixels".into());
    }
    if let Some(color) = &look.focus_color {
        if parse_hex(color).is_none() {
            return Err("a colour is #rrggbb".into());
        }
    }
    if let Some(mode) = &look.center_focused {
        if center_mode(mode).is_none() {
            return Err("centring is never, always or on-overflow".into());
        }
    }
    if look
        .animation_speed
        .is_some_and(|s| !(25..=400).contains(&s))
    {
        return Err("animation speed is 25 to 400 percent".into());
    }
    if look.blur_strength.is_some_and(|s| s > 100) {
        return Err("blur strength is 0 to 100".into());
    }
    if look.refraction.is_some_and(|r| r > 200) {
        return Err("refraction is 0 to 200 percent".into());
    }
    if look.focus_ring_width.is_some_and(|w| w > 8) {
        return Err("the focus ring is 0 to 8 pixels".into());
    }
    if look.window_corner_radius.is_some_and(|r| r > 32) {
        return Err("window corners are 0 to 32 pixels".into());
    }
    Ok(())
}

fn center_mode(name: &str) -> Option<CenterFocusedColumn> {
    match name {
        "never" => Some(CenterFocusedColumn::Never),
        "always" => Some(CenterFocusedColumn::Always),
        "on-overflow" => Some(CenterFocusedColumn::OnOverflow),
        _ => None,
    }
}

fn center_name(mode: CenterFocusedColumn) -> &'static str {
    match mode {
        CenterFocusedColumn::Never => "never",
        CenterFocusedColumn::Always => "always",
        CenterFocusedColumn::OnOverflow => "on-overflow",
    }
}

fn merge(base: &mut HelixLook, change: &HelixLook) {
    if change.animations.is_some() {
        base.animations = change.animations;
    }
    if change.window_gaps.is_some() {
        base.window_gaps = change.window_gaps;
    }
    if change.cursor_size.is_some() {
        base.cursor_size = change.cursor_size;
    }
    if change.focus_color.is_some() {
        base.focus_color = change.focus_color.clone();
    }
    if change.focus_follows_mouse.is_some() {
        base.focus_follows_mouse = change.focus_follows_mouse;
    }
    if change.hot_corner.is_some() {
        base.hot_corner = change.hot_corner;
    }
    if change.center_focused.is_some() {
        base.center_focused = change.center_focused.clone();
    }
    if change.workspace_back_and_forth.is_some() {
        base.workspace_back_and_forth = change.workspace_back_and_forth;
    }
    if change.window_corner_radius.is_some() {
        base.window_corner_radius = change.window_corner_radius;
    }
    if change.animation_speed.is_some() {
        base.animation_speed = change.animation_speed;
    }
    if change.blur_strength.is_some() {
        base.blur_strength = change.blur_strength;
    }
    if change.refraction.is_some() {
        base.refraction = change.refraction;
    }
    if change.window_shadows.is_some() {
        base.window_shadows = change.window_shadows;
    }
    if change.focus_ring_width.is_some() {
        base.focus_ring_width = change.focus_ring_width;
    }
}

/// A rule for every window that sets only the corner radius (and clips the window to it), which is what the override is. Later rules win, so it is
/// put last; the one from an earlier overlay is taken out first so rules do not pile up.
/// A rule for every window that sets only the corner radius (and clips the window to it), which is what the override is. Later rules win, so it is put last;
/// the one from an earlier overlay is taken out first so rules do not pile up.
fn corner_rule(radius: u16) -> WindowRule {
    WindowRule {
        geometry_corner_radius: Some(CornerRadius {
            top_left: f32::from(radius),
            top_right: f32::from(radius),
            bottom_right: f32::from(radius),
            bottom_left: f32::from(radius),
        }),
        clip_to_geometry: Some(true),
        ..WindowRule::default()
    }
}

fn is_corner_rule(rule: &WindowRule) -> bool {
    let mut bare = rule.clone();
    bare.geometry_corner_radius = None;
    bare.clip_to_geometry = None;
    bare == WindowRule::default()
        && rule.geometry_corner_radius.is_some()
        && rule.clip_to_geometry == Some(true)
}

fn radius_of(config: &Config) -> Option<u16> {
    config
        .window_rules
        .iter()
        .rev()
        .find(|rule| is_corner_rule(rule))
        .and_then(|rule| rule.geometry_corner_radius)
        .map(|r| r.top_left.round().clamp(0.0, 32.0) as u16)
}

const GLASS_NAMESPACE: &str = "^helix-panels?$";

/// The blur passes and offset for a strength: light is one pass and a small offset, heavy is five passes and a wide one.
fn blur_for(strength: u8) -> (u8, f64) {
    let t = f64::from(strength.min(100)) / 100.0;
    (1 + (t * 4.0).round() as u8, 1.5 + t * 4.5)
}

fn strength_of(passes: u8) -> u8 {
    ((f64::from(passes.clamp(1, 5)) - 1.0) / 4.0 * 100.0).round() as u8
}

/// The shell's own glass rule, if it is there (see [`ensure_glass_rule`]).
fn glass_rule(config: &mut Config) -> Option<&mut LayerRule> {
    config.layer_rules.iter_mut().find(|rule| {
        rule.matches.len() == 1
            && rule.matches[0]
                .namespace
                .as_ref()
                .is_some_and(|ns| ns.0.as_str() == GLASS_NAMESPACE)
    })
}

/// Puts the overrides over a configuration.
pub fn overlay(config: &mut Config, look: &HelixLook) {
    if let Some(on) = look.animations {
        config.animations.off = !on;
    }
    if let Some(gaps) = look.window_gaps {
        config.layout.gaps = f64::from(gaps);
    }
    if let Some(size) = look.cursor_size {
        config.cursor.xcursor_size = size;
    }
    if let Some(on) = look.focus_follows_mouse {
        config.input.focus_follows_mouse = on.then_some(FocusFollowsMouse {
            max_scroll_amount: None,
        });
    }
    if let Some(on) = look.hot_corner {
        config.gestures.hot_corners.off = !on;
    }
    if let Some(mode) = look.center_focused.as_deref().and_then(center_mode) {
        config.layout.center_focused_column = mode;
    }
    if let Some(on) = look.workspace_back_and_forth {
        config.input.workspace_auto_back_and_forth = on;
    }
    if let Some(speed) = look.animation_speed {
        config.animations.slowdown = 100.0 / f64::from(speed.clamp(25, 400));
    }
    if let Some(strength) = look.blur_strength {
        let (passes, offset) = blur_for(strength);
        config.blur.passes = passes;
        config.blur.offset = offset;
    }
    if let Some(percent) = look.refraction {
        if let Some(rule) = glass_rule(config) {
            let part = rule
                .background_effect
                .liquid_glass
                .get_or_insert_with(LiquidGlassPart::default);
            part.refraction_strength = Some(niri_config::utils::FloatOrInt(
                f64::from(percent.min(200)) / 100.0,
            ));
        }
    }
    if let Some(on) = look.window_shadows {
        config.layout.shadow.on = on;
    }
    if let Some(width) = look.focus_ring_width {
        config.layout.focus_ring.off = width == 0;
        config.layout.focus_ring.width = f64::from(width.min(8));
    }
    if let Some(radius) = look.window_corner_radius {
        config.window_rules.retain(|rule| !is_corner_rule(rule));
        config.window_rules.push(corner_rule(radius));
    }
    if let Some([r, g, b]) = look.focus_color.as_deref().and_then(parse_hex) {
        let color = Color::from_rgba8_unpremul(r, g, b, 255);
        config.layout.focus_ring.active_color = color;
        config.layout.focus_ring.active_gradient = None;
        config.layout.border.active_color = color;
        config.layout.border.active_gradient = None;
    }
}

/// What is in effect, every field set.
pub fn effective(config: &Config) -> HelixLook {
    let c = config.layout.focus_ring.active_color;
    let byte = |v: f32| (v.clamp(0.0, 1.0) * 255.0).round() as u8;
    HelixLook {
        animations: Some(!config.animations.off),
        window_gaps: Some(config.layout.gaps.clamp(0.0, 64.0).round() as u16),
        cursor_size: Some(config.cursor.xcursor_size),
        focus_color: Some(format!(
            "#{:02x}{:02x}{:02x}",
            byte(c.r),
            byte(c.g),
            byte(c.b)
        )),
        focus_follows_mouse: Some(config.input.focus_follows_mouse.is_some()),
        hot_corner: Some(!config.gestures.hot_corners.off),
        center_focused: Some(center_name(config.layout.center_focused_column).to_string()),
        workspace_back_and_forth: Some(config.input.workspace_auto_back_and_forth),
        window_corner_radius: Some(radius_of(config).unwrap_or(0)),
        animation_speed: Some(
            (100.0 / config.animations.slowdown.max(0.001))
                .round()
                .clamp(25.0, 400.0) as u16,
        ),
        blur_strength: Some(strength_of(config.blur.passes)),
        refraction: config
            .layer_rules
            .iter()
            .find(|rule| {
                rule.matches.len() == 1
                    && rule.matches[0]
                        .namespace
                        .as_ref()
                        .is_some_and(|ns| ns.0.as_str() == GLASS_NAMESPACE)
            })
            .and_then(|rule| rule.background_effect.liquid_glass)
            .map(|lg| {
                (lg.refraction_strength.map_or(0.35, |v| v.0) * 100.0)
                    .round()
                    .clamp(0.0, 200.0) as u16
            }),
        window_shadows: Some(config.layout.shadow.on),
        focus_ring_width: Some(if config.layout.focus_ring.off {
            0
        } else {
            config.layout.focus_ring.width.round().clamp(0.0, 8.0) as u8
        }),
    }
}

/// The shell's glass sees what is really behind it. A layer surface that asks for blur is, unless a rule says otherwise, blurred against the wallpaper
/// alone ("x-ray"), which is why a clear panel showed the wallpaper and not the windows under it. Helix's own surfaces (`helix-panel`, `helix-panels`) get
/// a rule that turns that off and adds a little refraction at the edges, unless the person has written a rule for them. It goes first, so any rule of theirs
/// that matches later still wins.
pub fn ensure_glass_rule(config: &mut Config) {
    let covered = config.layer_rules.iter().any(|rule| {
        rule.matches.iter().any(|m| {
            m.namespace
                .as_ref()
                .is_some_and(|ns| ns.0.is_match("helix-panel") || ns.0.is_match("helix-panels"))
        })
    });
    if covered {
        return;
    }
    let Ok(namespace) = GLASS_NAMESPACE.parse::<RegexEq>() else {
        return;
    };
    config.layer_rules.insert(
        0,
        LayerRule {
            matches: vec![LayerMatch {
                namespace: Some(namespace),
                ..LayerMatch::default()
            }],
            background_effect: BackgroundEffectRule {
                xray: Some(false),
                liquid_glass: Some(LiquidGlassPart {
                    refraction_strength: Some(niri_config::utils::FloatOrInt(0.35)),
                    ..LiquidGlassPart::default()
                }),
                ..BackgroundEffectRule::default()
            },
            ..LayerRule::default()
        },
    );
}

/// Applies the remembered overrides to a configuration that was just loaded (start-up and every reload).
pub fn apply_saved(config: &mut Config) {
    ensure_glass_rule(config);
    overlay(config, &load());
}

impl State {
    pub fn helix_look(&self) -> HelixLook {
        effective(&self.niri.config.borrow())
    }

    /// Changes the fields that are set, remembers them, and makes them take effect now.
    pub fn helix_set_look(&mut self, change: HelixLook) -> Result<(), String> {
        validate(&change)?;
        let mut stored = load();
        merge(&mut stored, &change);
        save(&stored)?;
        {
            let mut config = self.niri.config.borrow_mut();
            overlay(&mut config, &stored);
        }
        let (off, theme, size) = {
            let config = self.niri.config.borrow();
            self.niri.layout.update_config(&config);
            (
                config.animations.off,
                config.cursor.xcursor_theme.clone(),
                config.cursor.xcursor_size,
            )
        };
        self.niri.clock.set_complete_instantly(off);
        if change.animation_speed.is_some() {
            let slowdown = self.niri.config.borrow().animations.slowdown;
            self.niri.clock.set_rate(1.0 / slowdown.max(0.001));
        }
        if change.refraction.is_some() {
            self.niri.recompute_layer_rules();
        }
        if change.cursor_size.is_some() {
            self.niri.cursor_manager.reload(&theme, size);
            self.niri.cursor_texture_cache.clear();
        }
        if change.window_corner_radius.is_some() {
            // Windows that are already open take the new corners now.
            self.niri.recompute_window_rules();
        }
        self.niri.queue_redraw_all();
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_shells_glass_is_not_x_ray_unless_the_person_wrote_their_own_rule() {
        let mut config = Config::default();
        let before = config.layer_rules.len();
        ensure_glass_rule(&mut config);
        ensure_glass_rule(&mut config);
        assert_eq!(
            config.layer_rules.len(),
            before + 1,
            "once, however often the configuration is applied"
        );
        let rule = &config.layer_rules[0];
        assert_eq!(rule.background_effect.xray, Some(false));
        assert!(rule.matches[0]
            .namespace
            .as_ref()
            .unwrap()
            .0
            .is_match("helix-panel"));
        assert!(rule.matches[0]
            .namespace
            .as_ref()
            .unwrap()
            .0
            .is_match("helix-panels"));
        assert!(!rule.matches[0]
            .namespace
            .as_ref()
            .unwrap()
            .0
            .is_match("helix-wallpaper"));
        // Their own rule for these surfaces is respected.
        let mut own = Config::default();
        own.layer_rules.push(LayerRule {
            matches: vec![LayerMatch {
                namespace: Some("helix-panel".parse::<RegexEq>().unwrap()),
                ..LayerMatch::default()
            }],
            background_effect: BackgroundEffectRule {
                xray: Some(true),
                ..BackgroundEffectRule::default()
            },
            ..LayerRule::default()
        });
        let count = own.layer_rules.len();
        ensure_glass_rule(&mut own);
        assert_eq!(own.layer_rules.len(), count);
    }

    #[test]
    fn effects_are_set_and_read_back_in_the_units_the_page_uses() {
        let mut config = Config::default();
        ensure_glass_rule(&mut config);
        overlay(
            &mut config,
            &HelixLook {
                animation_speed: Some(200),
                blur_strength: Some(100),
                refraction: Some(150),
                window_shadows: Some(true),
                focus_ring_width: Some(0),
                ..HelixLook::default()
            },
        );
        let seen = effective(&config);
        assert_eq!(seen.animation_speed, Some(200));
        assert_eq!(
            config.animations.slowdown, 0.5,
            "twice as fast is half the slowdown"
        );
        assert_eq!((seen.blur_strength, config.blur.passes), (Some(100), 5));
        assert_eq!(seen.refraction, Some(150));
        assert_eq!(seen.window_shadows, Some(true));
        assert_eq!(seen.focus_ring_width, Some(0));
        assert!(config.layout.focus_ring.off);
        overlay(
            &mut config,
            &HelixLook {
                focus_ring_width: Some(3),
                blur_strength: Some(0),
                animation_speed: Some(25),
                ..HelixLook::default()
            },
        );
        let seen = effective(&config);
        assert_eq!(
            (
                seen.focus_ring_width,
                seen.blur_strength,
                seen.animation_speed
            ),
            (Some(3), Some(0), Some(25))
        );
        assert!(!config.layout.focus_ring.off && config.layout.focus_ring.width == 3.0);
        assert_eq!(config.animations.slowdown, 4.0);
        // Blur gets heavier with the strength, never lighter.
        let mut last = (0u8, 0.0f64);
        for strength in (0..=100).step_by(10) {
            let (passes, offset) = blur_for(strength);
            assert!(passes >= last.0 && offset >= last.1, "{strength}");
            last = (passes, offset);
        }
    }

    #[test]
    fn refraction_is_not_reported_when_the_person_has_their_own_glass_rule() {
        let mut own = Config::default();
        own.layer_rules.push(LayerRule {
            matches: vec![LayerMatch {
                namespace: Some("helix-panel".parse::<RegexEq>().unwrap()),
                ..LayerMatch::default()
            }],
            ..LayerRule::default()
        });
        ensure_glass_rule(&mut own);
        overlay(
            &mut own,
            &HelixLook {
                refraction: Some(120),
                ..HelixLook::default()
            },
        );
        assert_eq!(effective(&own).refraction, None, "not ours to change");
    }

    #[test]
    fn impossible_effects_are_refused() {
        let bad = |look: HelixLook| validate(&look).is_err();
        assert!(bad(HelixLook {
            animation_speed: Some(10),
            ..HelixLook::default()
        }));
        assert!(bad(HelixLook {
            animation_speed: Some(401),
            ..HelixLook::default()
        }));
        assert!(bad(HelixLook {
            blur_strength: Some(101),
            ..HelixLook::default()
        }));
        assert!(bad(HelixLook {
            refraction: Some(201),
            ..HelixLook::default()
        }));
        assert!(bad(HelixLook {
            focus_ring_width: Some(9),
            ..HelixLook::default()
        }));
        assert!(validate(&HelixLook {
            animation_speed: Some(25),
            blur_strength: Some(100),
            refraction: Some(200),
            focus_ring_width: Some(8),
            ..HelixLook::default()
        })
        .is_ok());
    }

    #[test]
    fn the_window_corner_override_is_one_rule_that_is_replaced_not_added() {
        let mut config = Config::default();
        let before = config.window_rules.len();
        assert_eq!(effective(&config).window_corner_radius, Some(0));
        let set = |config: &mut Config, r: u16| {
            overlay(
                config,
                &HelixLook {
                    window_corner_radius: Some(r),
                    ..HelixLook::default()
                },
            )
        };
        set(&mut config, 18);
        set(&mut config, 6);
        assert_eq!(
            config.window_rules.len(),
            before + 1,
            "one rule, not one per change"
        );
        assert_eq!(effective(&config).window_corner_radius, Some(6));
        let rule = config.window_rules.last().unwrap();
        assert_eq!(rule.clip_to_geometry, Some(true));
        assert!(rule.matches.is_empty(), "it is for every window");
        assert!(validate(&HelixLook {
            window_corner_radius: Some(33),
            ..HelixLook::default()
        })
        .is_err());
        assert!(validate(&HelixLook {
            window_corner_radius: Some(32),
            ..HelixLook::default()
        })
        .is_ok());
        // A person's own rule is left alone.
        let mut own = Config::default();
        own.window_rules.push(WindowRule {
            open_focused: Some(true),
            ..WindowRule::default()
        });
        set(&mut own, 12);
        assert_eq!(own.window_rules.len(), 2);
    }

    #[test]
    fn overrides_replace_only_what_they_name() {
        let mut config = Config::default();
        let gaps = config.layout.gaps;
        overlay(
            &mut config,
            &HelixLook {
                animations: Some(false),
                cursor_size: Some(32),
                focus_color: Some("#bf5af2".into()),
                ..HelixLook::default()
            },
        );
        assert!(config.animations.off);
        assert_eq!(config.cursor.xcursor_size, 32);
        assert_eq!(config.layout.gaps, gaps, "untouched");
        let seen = effective(&config);
        assert_eq!(seen.focus_color.as_deref(), Some("#bf5af2"));
        assert_eq!(seen.animations, Some(false));
        overlay(
            &mut config,
            &HelixLook {
                window_gaps: Some(8),
                animations: Some(true),
                ..HelixLook::default()
            },
        );
        assert_eq!((config.layout.gaps, config.animations.off), (8.0, false));
        overlay(
            &mut config,
            &HelixLook {
                focus_follows_mouse: Some(true),
                hot_corner: Some(false),
                center_focused: Some("always".into()),
                workspace_back_and_forth: Some(false),
                ..HelixLook::default()
            },
        );
        let seen = effective(&config);
        assert_eq!(
            (
                seen.focus_follows_mouse,
                seen.hot_corner,
                seen.workspace_back_and_forth
            ),
            (Some(true), Some(false), Some(false))
        );
        assert_eq!(seen.center_focused.as_deref(), Some("always"));
        overlay(
            &mut config,
            &HelixLook {
                focus_follows_mouse: Some(false),
                ..HelixLook::default()
            },
        );
        assert_eq!(effective(&config).focus_follows_mouse, Some(false));
    }

    #[test]
    fn only_sane_values_are_stored() {
        assert!(validate(&HelixLook {
            window_gaps: Some(64),
            cursor_size: Some(16),
            focus_color: Some("#0a84ff".into()),
            ..HelixLook::default()
        })
        .is_ok());
        for bad in [
            HelixLook {
                window_gaps: Some(65),
                ..HelixLook::default()
            },
            HelixLook {
                cursor_size: Some(8),
                ..HelixLook::default()
            },
            HelixLook {
                cursor_size: Some(200),
                ..HelixLook::default()
            },
            HelixLook {
                focus_color: Some("blue".into()),
                ..HelixLook::default()
            },
            HelixLook {
                focus_color: Some("#12345".into()),
                ..HelixLook::default()
            },
            HelixLook {
                focus_color: Some("#12345g".into()),
                ..HelixLook::default()
            },
            HelixLook {
                center_focused: Some("sometimes".into()),
                ..HelixLook::default()
            },
        ] {
            assert!(validate(&bad).is_err(), "{bad:?}");
        }
        assert_eq!(parse_hex("#0a84ff"), Some([10, 132, 255]));
    }
}
