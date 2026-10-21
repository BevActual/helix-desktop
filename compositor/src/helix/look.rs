//! Helix look settings: animations on or off, the gap between windows, the cursor size, and the colour of the focus ring (which follows the accent
//! colour). Like the input settings they are *overrides* over `config.kdl`: the person's file is never rewritten, the overrides live in
//! `$XDG_STATE_HOME/helix/look-overrides.json`, are applied every time the configuration is loaded, and take effect at once when set over IPC.

use std::path::PathBuf;

use niri_config::appearance::Color;
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
    Ok(())
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
        focus_color: Some(format!("#{:02x}{:02x}{:02x}", byte(c.r), byte(c.g), byte(c.b))),
    }
}

/// Applies the remembered overrides to a configuration that was just loaded (start-up and every reload).
pub fn apply_saved(config: &mut Config) {
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
        if change.cursor_size.is_some() {
            self.niri.cursor_manager.reload(&theme, size);
            self.niri.cursor_texture_cache.clear();
        }
        self.niri.queue_redraw_all();
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn overrides_replace_only_what_they_name() {
        let mut config = Config::default();
        let gaps = config.layout.gaps;
        overlay(&mut config, &HelixLook { animations: Some(false), cursor_size: Some(32), focus_color: Some("#bf5af2".into()), ..HelixLook::default() });
        assert!(config.animations.off);
        assert_eq!(config.cursor.xcursor_size, 32);
        assert_eq!(config.layout.gaps, gaps, "untouched");
        let seen = effective(&config);
        assert_eq!(seen.focus_color.as_deref(), Some("#bf5af2"));
        assert_eq!(seen.animations, Some(false));
        overlay(&mut config, &HelixLook { window_gaps: Some(8), animations: Some(true), ..HelixLook::default() });
        assert_eq!((config.layout.gaps, config.animations.off), (8.0, false));
    }

    #[test]
    fn only_sane_values_are_stored() {
        assert!(validate(&HelixLook { window_gaps: Some(64), cursor_size: Some(16), focus_color: Some("#0a84ff".into()), ..HelixLook::default() }).is_ok());
        for bad in [
            HelixLook { window_gaps: Some(65), ..HelixLook::default() },
            HelixLook { cursor_size: Some(8), ..HelixLook::default() },
            HelixLook { cursor_size: Some(200), ..HelixLook::default() },
            HelixLook { focus_color: Some("blue".into()), ..HelixLook::default() },
            HelixLook { focus_color: Some("#12345".into()), ..HelixLook::default() },
            HelixLook { focus_color: Some("#12345g".into()), ..HelixLook::default() },
        ] {
            assert!(validate(&bad).is_err(), "{bad:?}");
        }
        assert_eq!(parse_hex("#0a84ff"), Some([10, 132, 255]));
    }
}
