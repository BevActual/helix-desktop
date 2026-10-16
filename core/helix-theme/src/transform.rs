//! Palette transforms applied after resolution (`theme/palette_transform.cpp`).

use crate::hct::Hct;
use crate::palette::TokenMap;

/// The dark elevation ladder plus the two terminal tokens that are the background itself.
const SURFACE_RAMP: [&str; 12] = [
    "background",
    "surface",
    "surface_variant",
    "surface_dim",
    "surface_bright",
    "surface_container_lowest",
    "surface_container_low",
    "surface_container",
    "surface_container_high",
    "surface_container_highest",
    "terminal_background",
    "terminal_cursor_text",
];

fn lower_tone(argb: u32, shift: f64) -> u32 {
    let mut hct = Hct::from_argb(argb);
    hct.set_tone((hct.tone() - shift).max(0.0));
    hct.to_argb()
}

/// `applyPureBlackDark(TokenMap&)`: shifts the elevation ramp so `surface` lands on tone 0.
pub fn apply_pure_black_dark(dark: &mut TokenMap) {
    let Some(base) = dark.get("surface").copied() else {
        return;
    };
    let shift = Hct::from_argb(base).tone();
    if shift <= 0.0 {
        return;
    }
    for key in SURFACE_RAMP {
        if let Some(value) = dark.get_mut(key) {
            *value = lower_tone(*value, shift);
        }
    }
}

/// `applyHighContrast(TokenMap&, bool)`.
pub fn apply_high_contrast(tokens: &mut TokenMap, is_dark: bool) {
    for (key, value) in tokens.iter_mut() {
        let mut hct = Hct::from_argb(*value);
        let mut tone = hct.tone();
        if key == "outline" || key == "outline_variant" {
            tone = if is_dark {
                tone.max(80.0)
            } else {
                tone.min(20.0)
            };
        } else if tone < 50.0 {
            tone = (tone - 20.0).max(0.0);
        } else {
            tone = (tone + 20.0).min(100.0);
        }
        hct.set_tone(tone);
        *value = hct.to_argb();
    }
}

/// Both transforms for a generated palette, in the order `ThemeService` applies them.
pub fn apply_resolved_transforms(
    dark: &mut TokenMap,
    light: &mut TokenMap,
    pure_black_dark: bool,
    high_contrast: bool,
) {
    if pure_black_dark && !dark.is_empty() {
        apply_pure_black_dark(dark);
    }
    if high_contrast {
        if !dark.is_empty() {
            apply_high_contrast(dark, true);
        }
        if !light.is_empty() {
            apply_high_contrast(light, false);
        }
    }
}
