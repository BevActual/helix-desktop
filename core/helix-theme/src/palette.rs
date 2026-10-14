//! Fixed 16-role palettes, token expansion and terminal synthesis (`theme/fixed_palette.cpp`).
//!
//! The C++ side stores roles as float RGB, but every value originates from a byte
//! (`byte / 255.0F`) and is converted back with round-to-nearest, so the integer
//! model here is exact.

use std::collections::HashMap;

use crate::color::Color;
use crate::contrast::ensure_contrast;

/// Token name → packed ARGB (`0xffRRGGBB`).
pub type TokenMap = HashMap<String, u32>;

const OPAQUE_BLACK: u32 = 0xff00_0000;
const OPAQUE_WHITE: u32 = 0xffff_ffff;

/// The 16 editable roles. Field order matches `ColorRole`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Palette {
    pub primary: Color,
    pub on_primary: Color,
    pub secondary: Color,
    pub on_secondary: Color,
    pub tertiary: Color,
    pub on_tertiary: Color,
    pub error: Color,
    pub on_error: Color,
    pub surface: Color,
    pub on_surface: Color,
    pub surface_variant: Color,
    pub on_surface_variant: Color,
    pub outline: Color,
    pub shadow: Color,
    pub hover: Color,
    pub on_hover: Color,
}

pub const ANSI_KEYS: [&str; 8] = ["black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"];
pub const ANSI_GROUPS: [(&str, &str); 2] = [("normal", "terminal_normal"), ("bright", "terminal_bright")];
/// `(json key, token key)`
pub const DIRECT_KEYS: [(&str, &str); 6] = [
    ("foreground", "terminal_foreground"),
    ("background", "terminal_background"),
    ("cursor", "terminal_cursor"),
    ("cursorText", "terminal_cursor_text"),
    ("selectionFg", "terminal_selection_fg"),
    ("selectionBg", "terminal_selection_bg"),
];

/// Indexed by [`ANSI_KEYS`].
pub type AnsiColors = [Color; 8];

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TerminalPalette {
    pub normal: AnsiColors,
    pub bright: AnsiColors,
    pub foreground: Color,
    pub background: Color,
    pub selection_fg: Color,
    pub selection_bg: Color,
    pub cursor_text: Color,
    pub cursor: Color,
}

impl TerminalPalette {
    pub fn direct(&self, json_key: &str) -> Color {
        match json_key {
            "foreground" => self.foreground,
            "background" => self.background,
            "cursor" => self.cursor,
            "cursorText" => self.cursor_text,
            "selectionFg" => self.selection_fg,
            _ => self.selection_bg,
        }
    }

    pub fn direct_mut(&mut self, json_key: &str) -> &mut Color {
        match json_key {
            "foreground" => &mut self.foreground,
            "background" => &mut self.background,
            "cursor" => &mut self.cursor,
            "cursorText" => &mut self.cursor_text,
            "selectionFg" => &mut self.selection_fg,
            _ => &mut self.selection_bg,
        }
    }
}

fn token_to_color(tokens: &TokenMap, key: &str) -> Color {
    match tokens.get(key) {
        Some(v) => Color::from_argb(*v & 0x00ff_ffff),
        None => Color::new(0xff, 0x00, 0xff),
    }
}

fn set_token(dst: &mut TokenMap, key: &str, color: Color) {
    dst.insert(key.to_string(), color.to_argb());
}

fn token_or(tokens: &TokenMap, key: &str, fallback: u32) -> u32 {
    tokens.get(key).copied().unwrap_or(fallback)
}

fn set_missing_token(tokens: &mut TokenMap, key: &str, value: u32) {
    tokens.entry(key.to_string()).or_insert(value);
}

fn interpolate(a: Color, b: Color, t: f64) -> Color {
    let mix = |l: i32, r: i32| (f64::from(l) + f64::from(r - l) * t) as i32;
    Color::new(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b))
}

fn ensure_terminal_text_contrast(foreground: u32, background: u32) -> u32 {
    ensure_contrast(Color::from_argb(foreground), Color::from_argb(background), 4.5, 0).to_argb()
}

pub fn apply_terminal_palette(tokens: &mut TokenMap, terminal: &TerminalPalette) {
    for (json, prefix) in ANSI_GROUPS {
        let colors = if json == "bright" { &terminal.bright } else { &terminal.normal };
        for (key, color) in ANSI_KEYS.iter().zip(colors) {
            set_token(tokens, &format!("{prefix}_{key}"), *color);
        }
    }
    for (json, token) in DIRECT_KEYS {
        set_token(tokens, token, terminal.direct(json));
    }
}

pub fn synthesize_terminal_palette_tokens(tokens: &mut TokenMap) {
    let background = token_or(tokens, "background", token_or(tokens, "surface", OPAQUE_BLACK));
    let foreground = token_or(tokens, "on_surface", OPAQUE_WHITE);
    let surface = token_or(tokens, "surface", background);
    let surface_variant = token_or(tokens, "surface_variant", surface);
    let on_surface_variant = token_or(tokens, "on_surface_variant", foreground);
    let outline = token_or(tokens, "outline", on_surface_variant);
    let error = token_or(tokens, "error", foreground);
    let primary = token_or(tokens, "primary", foreground);
    let secondary = token_or(tokens, "secondary", primary);
    let tertiary = token_or(tokens, "tertiary", secondary);
    let primary_fixed_dim = token_or(tokens, "primary_fixed_dim", primary);
    let secondary_fixed_dim = token_or(tokens, "secondary_fixed_dim", secondary);

    set_missing_token(tokens, "terminal_foreground", foreground);
    set_missing_token(tokens, "terminal_background", background);
    set_missing_token(tokens, "terminal_cursor", foreground);
    set_missing_token(tokens, "terminal_cursor_text", background);
    set_missing_token(tokens, "terminal_selection_fg", on_surface_variant);
    set_missing_token(tokens, "terminal_selection_bg", surface_variant);

    let terminal_background = token_or(tokens, "terminal_background", background);
    let red = ensure_terminal_text_contrast(error, terminal_background);
    let green = ensure_terminal_text_contrast(primary, terminal_background);
    let yellow = ensure_terminal_text_contrast(secondary, terminal_background);
    let blue = ensure_terminal_text_contrast(tertiary, terminal_background);
    let magenta = ensure_terminal_text_contrast(primary_fixed_dim, terminal_background);
    let cyan = ensure_terminal_text_contrast(secondary_fixed_dim, terminal_background);

    let normal = [
        ("black", surface_variant),
        ("red", red),
        ("green", green),
        ("yellow", yellow),
        ("blue", blue),
        ("magenta", magenta),
        ("cyan", cyan),
        ("white", foreground),
    ];
    for (name, value) in normal {
        set_missing_token(tokens, &format!("terminal_normal_{name}"), value);
    }
    let bright = [
        ("black", outline),
        ("red", red),
        ("green", green),
        ("yellow", yellow),
        ("blue", blue),
        ("magenta", magenta),
        ("cyan", cyan),
        ("white", foreground),
    ];
    for (name, value) in bright {
        set_missing_token(tokens, &format!("terminal_bright_{name}"), value);
    }
}

pub fn expand_fixed_palette_mode(palette: &Palette, is_dark: bool) -> TokenMap {
    let Palette {
        primary,
        on_primary,
        secondary,
        on_secondary,
        tertiary,
        on_tertiary,
        error,
        on_error,
        surface,
        on_surface,
        surface_variant,
        on_surface_variant,
        outline: outline_raw,
        shadow,
        ..
    } = *palette;

    let container_dark = |base: Color| {
        let (h, s, l) = base.to_hsl();
        // Scale the saturation step by remaining headroom instead of a flat +0.15.
        Color::from_hsl(h, s + 0.15 * (1.0 - s), (l - 0.35).max(0.15))
    };
    let container_light = |base: Color| {
        let (h, s, l) = base.to_hsl();
        Color::from_hsl(h, (s - 0.20).max(0.30), (l + 0.35).min(0.85))
    };
    let fixed_dark = |base: Color| {
        let (h, s, _) = base.to_hsl();
        (Color::from_hsl(h, s.max(0.70), 0.85), Color::from_hsl(h, s.max(0.65), 0.75))
    };
    let fixed_light = |base: Color| {
        let (h, s, _) = base.to_hsl();
        (Color::from_hsl(h, s.max(0.70), 0.40), Color::from_hsl(h, s.max(0.65), 0.30))
    };
    let container = |c: Color| if is_dark { container_dark(c) } else { container_light(c) };
    let fixed = |c: Color| if is_dark { fixed_dark(c) } else { fixed_light(c) };
    let ec = |fg: Color, bg: Color| ensure_contrast(fg, bg, 4.5, 0);

    let primary_container = container(primary);
    let secondary_container = container(secondary);
    let tertiary_container = container(tertiary);
    let error_container = container(error);

    let (primary_h, primary_s, _) = primary.to_hsl();
    let (secondary_h, secondary_s, _) = secondary.to_hsl();
    let (tertiary_h, tertiary_s, _) = tertiary.to_hsl();
    let (error_h, error_s, _) = error.to_hsl();

    let on_container_l = if is_dark { 0.90 } else { 0.15 };
    let on_primary_container = ec(Color::from_hsl(primary_h, primary_s, on_container_l), primary_container);
    let on_secondary_container = ec(Color::from_hsl(secondary_h, secondary_s, on_container_l), secondary_container);
    let on_tertiary_container = ec(Color::from_hsl(tertiary_h, tertiary_s, on_container_l), tertiary_container);
    let on_error_container = ec(Color::from_hsl(error_h, error_s, on_container_l), error_container);

    let (primary_fixed, primary_fixed_dim) = fixed(primary);
    let (secondary_fixed, secondary_fixed_dim) = fixed(secondary);
    let (tertiary_fixed, tertiary_fixed_dim) = fixed(tertiary);

    let (on_fixed_l, on_fixed_variant_l) = if is_dark { (0.15, 0.20) } else { (0.90, 0.85) };
    let on_fixed = |h: f64, base: Color| ec(Color::from_hsl(h, 0.15, on_fixed_l), base);
    let on_fixed_variant = |h: f64, base: Color| ec(Color::from_hsl(h, 0.15, on_fixed_variant_l), base);
    let on_primary_fixed = on_fixed(primary_h, primary_fixed);
    let on_primary_fixed_variant = on_fixed_variant(primary_h, primary_fixed_dim);
    let on_secondary_fixed = on_fixed(secondary_h, secondary_fixed);
    let on_secondary_fixed_variant = on_fixed_variant(secondary_h, secondary_fixed_dim);
    let on_tertiary_fixed = on_fixed(tertiary_h, tertiary_fixed);
    let on_tertiary_fixed_variant = on_fixed_variant(tertiary_h, tertiary_fixed_dim);

    let (surface_h, surface_s, surface_l) = surface.to_hsl();
    let (sv_h, sv_s, sv_l) = surface_variant.to_hsl();
    let surface_container = surface_variant;
    let surface_container_lowest = interpolate(surface, surface_variant, 0.2);
    let surface_container_low = interpolate(surface, surface_variant, 0.5);
    let (surface_container_high, surface_container_highest, surface_dim, surface_bright) = if is_dark {
        (
            Color::from_hsl(sv_h, sv_s, (sv_l + 0.04).min(0.40)),
            Color::from_hsl(sv_h, sv_s, (sv_l + 0.08).min(0.45)),
            Color::from_hsl(surface_h, surface_s, (surface_l - 0.04).max(0.02)),
            Color::from_hsl(sv_h, sv_s, (sv_l + 0.12).min(0.50)),
        )
    } else {
        (
            Color::from_hsl(sv_h, sv_s, (sv_l - 0.04).max(0.60)),
            Color::from_hsl(sv_h, sv_s, (sv_l - 0.08).max(0.55)),
            Color::from_hsl(sv_h, sv_s, (sv_l - 0.12).max(0.50)),
            Color::from_hsl(surface_h, surface_s, (surface_l + 0.03).min(0.98)),
        )
    };

    let outline = ensure_contrast(outline_raw, surface, 3.0, 0);
    let (outline_h, outline_s, outline_l) = outline.to_hsl();
    let outline_variant = if is_dark {
        Color::from_hsl(outline_h, outline_s, (outline_l - 0.15).max(0.1))
    } else {
        Color::from_hsl(outline_h, outline_s, (outline_l + 0.15).min(0.9))
    };
    let scrim = Color::new(0, 0, 0);
    let inverse_surface = if is_dark {
        Color::from_hsl(surface_h, 0.08, 0.90)
    } else {
        Color::from_hsl(surface_h, 0.08, 0.15)
    };
    let inverse_on_surface = if is_dark {
        Color::from_hsl(surface_h, 0.05, 0.15)
    } else {
        Color::from_hsl(surface_h, 0.05, 0.90)
    };
    let inverse_primary = if is_dark {
        Color::from_hsl(primary_h, (primary_s * 0.8).max(0.5), 0.40)
    } else {
        Color::from_hsl(primary_h, (primary_s * 0.8).max(0.5), 0.70)
    };

    let entries = [
        ("source_color", primary),
        ("primary", primary),
        ("on_primary", on_primary),
        ("primary_container", primary_container),
        ("on_primary_container", on_primary_container),
        ("primary_fixed", primary_fixed),
        ("primary_fixed_dim", primary_fixed_dim),
        ("on_primary_fixed", on_primary_fixed),
        ("on_primary_fixed_variant", on_primary_fixed_variant),
        ("secondary", secondary),
        ("on_secondary", on_secondary),
        ("secondary_container", secondary_container),
        ("on_secondary_container", on_secondary_container),
        ("secondary_fixed", secondary_fixed),
        ("secondary_fixed_dim", secondary_fixed_dim),
        ("on_secondary_fixed", on_secondary_fixed),
        ("on_secondary_fixed_variant", on_secondary_fixed_variant),
        ("tertiary", tertiary),
        ("on_tertiary", on_tertiary),
        ("tertiary_container", tertiary_container),
        ("on_tertiary_container", on_tertiary_container),
        ("tertiary_fixed", tertiary_fixed),
        ("tertiary_fixed_dim", tertiary_fixed_dim),
        ("on_tertiary_fixed", on_tertiary_fixed),
        ("on_tertiary_fixed_variant", on_tertiary_fixed_variant),
        ("error", error),
        ("on_error", on_error),
        ("error_container", error_container),
        ("on_error_container", on_error_container),
        ("surface", surface),
        ("on_surface", on_surface),
        ("surface_variant", surface_variant),
        ("on_surface_variant", on_surface_variant),
        ("surface_dim", surface_dim),
        ("surface_bright", surface_bright),
        ("surface_container_lowest", surface_container_lowest),
        ("surface_container_low", surface_container_low),
        ("surface_container", surface_container),
        ("surface_container_high", surface_container_high),
        ("surface_container_highest", surface_container_highest),
        ("surface_tint", primary),
        ("outline", outline),
        ("outline_variant", outline_variant),
        ("shadow", shadow),
        ("scrim", scrim),
        ("inverse_surface", inverse_surface),
        ("inverse_on_surface", inverse_on_surface),
        ("inverse_primary", inverse_primary),
        ("background", surface),
        ("on_background", on_surface),
    ];
    let mut result = TokenMap::new();
    for (key, color) in entries {
        set_token(&mut result, key, color);
    }
    result
}

pub fn map_generated_palette_mode(t: &TokenMap) -> Palette {
    Palette {
        primary: token_to_color(t, "primary"),
        on_primary: token_to_color(t, "on_primary"),
        secondary: token_to_color(t, "secondary"),
        on_secondary: token_to_color(t, "on_secondary"),
        tertiary: token_to_color(t, "tertiary"),
        on_tertiary: token_to_color(t, "on_tertiary"),
        error: token_to_color(t, "error"),
        on_error: token_to_color(t, "on_error"),
        surface: token_to_color(t, "surface"),
        on_surface: token_to_color(t, "on_surface"),
        surface_variant: token_to_color(t, "surface_container"),
        on_surface_variant: token_to_color(t, "on_surface_variant"),
        outline: token_to_color(t, "outline_variant"),
        shadow: token_to_color(t, "shadow"),
        hover: token_to_color(t, "tertiary"),
        on_hover: token_to_color(t, "on_tertiary"),
    }
}

pub fn terminal_palette_from_tokens(tokens: &TokenMap) -> TerminalPalette {
    let mut terminal = TerminalPalette::default();
    for (json, prefix) in ANSI_GROUPS {
        let colors = if json == "bright" { &mut terminal.bright } else { &mut terminal.normal };
        for (slot, key) in colors.iter_mut().zip(ANSI_KEYS) {
            *slot = token_to_color(tokens, &format!("{prefix}_{key}"));
        }
    }
    for (json, token) in DIRECT_KEYS {
        *terminal.direct_mut(json) = token_to_color(tokens, token);
    }
    terminal
}
