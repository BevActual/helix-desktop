//! Builtin palettes, palette JSON parsing, and custom-palette naming/writing
//! (`theme/builtin_palettes.cpp`, `theme/custom_palettes.cpp`, parts of `theme_service.cpp`).
//!
//! The JSON writer is hand-rolled rather than going through `serde_json`'s map so
//! key order and `dump(2)` formatting match the C++ `ordered_json` output byte for byte
//! without needing the crate-wide `preserve_order` feature.

use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use serde_json::Value;

use crate::color::Color;
use crate::contrast::ensure_contrast;
use crate::palette::{
    apply_terminal_palette, expand_fixed_palette_mode, map_generated_palette_mode,
    synthesize_terminal_palette_tokens, terminal_palette_from_tokens, Palette, TerminalPalette,
    TokenMap, ANSI_KEYS,
};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FixedPaletteMode {
    pub palette: Palette,
    pub terminal: TerminalPalette,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FixedPalette {
    pub dark: FixedPaletteMode,
    pub light: FixedPaletteMode,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BuiltinPalette {
    pub name: String,
    pub palette: FixedPalette,
}

/// Dark/light token maps (C++ `GeneratedPalette`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct GeneratedPalette {
    pub dark: TokenMap,
    pub light: TokenMap,
}

/// The 16 editable colour roles in `ColorRole` order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ColorRole {
    Primary,
    OnPrimary,
    Secondary,
    OnSecondary,
    Tertiary,
    OnTertiary,
    Error,
    OnError,
    Surface,
    OnSurface,
    SurfaceVariant,
    OnSurfaceVariant,
    Outline,
    Shadow,
    Hover,
    OnHover,
}

impl ColorRole {
    pub fn from_index(index: u32) -> Option<Self> {
        use ColorRole::*;
        [
            Primary,
            OnPrimary,
            Secondary,
            OnSecondary,
            Tertiary,
            OnTertiary,
            Error,
            OnError,
            Surface,
            OnSurface,
            SurfaceVariant,
            OnSurfaceVariant,
            Outline,
            Shadow,
            Hover,
            OnHover,
        ]
        .get(index as usize)
        .copied()
    }
}

/// `#rgb`, `#rgba`, `#rrggbb` or `#rrggbbaa` (literal `#` required); alpha is dropped.
pub fn parse_hex(value: &str) -> Option<Color> {
    let digits = value.strip_prefix('#')?;
    if !digits.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    let byte = |i: usize| {
        u8::from_str_radix(&digits[i..i + 2], 16)
            .ok()
            .map(i32::from)
    };
    let short = |i: usize| {
        u8::from_str_radix(&digits[i..=i], 16)
            .ok()
            .map(|v| i32::from(v) * 17)
    };
    match digits.len() {
        3 | 4 => Some(Color::new(short(0)?, short(1)?, short(2)?)),
        6 | 8 => Some(Color::new(byte(0)?, byte(2)?, byte(4)?)),
        _ => None,
    }
}

/// Reads `mPrimary` first, then `primary`; missing or invalid yields black.
fn read_color_field(obj: &Value, camel: &str) -> Color {
    let mut prefixed = String::from("m");
    let mut chars = camel.chars();
    if let Some(first) = chars.next() {
        prefixed.extend(first.to_uppercase());
    }
    prefixed.push_str(chars.as_str());
    for key in [prefixed.as_str(), camel] {
        if let Some(c) = obj.get(key).and_then(Value::as_str).and_then(parse_hex) {
            return c;
        }
    }
    Color::default()
}

fn read_palette_json(obj: &Value) -> Palette {
    let c = |k: &str| read_color_field(obj, k);
    Palette {
        primary: c("primary"),
        on_primary: c("onPrimary"),
        secondary: c("secondary"),
        on_secondary: c("onSecondary"),
        tertiary: c("tertiary"),
        on_tertiary: c("onTertiary"),
        error: c("error"),
        on_error: c("onError"),
        surface: c("surface"),
        on_surface: c("onSurface"),
        surface_variant: c("surfaceVariant"),
        on_surface_variant: c("onSurfaceVariant"),
        outline: c("outline"),
        shadow: c("shadow"),
        hover: c("hover"),
        on_hover: c("onHover"),
    }
}

fn read_terminal_json(obj: &Value) -> TerminalPalette {
    let mut t = TerminalPalette::default();
    for (group, slots) in [("normal", &mut t.normal), ("bright", &mut t.bright)] {
        if let Some(g) = obj.get(group).filter(|v| v.is_object()) {
            for (slot, key) in slots.iter_mut().zip(ANSI_KEYS) {
                *slot = read_color_field(g, key);
            }
        }
    }
    t.foreground = read_color_field(obj, "foreground");
    t.background = read_color_field(obj, "background");
    t.selection_fg = read_color_field(obj, "selectionFg");
    t.selection_bg = read_color_field(obj, "selectionBg");
    t.cursor_text = read_color_field(obj, "cursorText");
    t.cursor = read_color_field(obj, "cursor");
    t
}

fn read_mode(obj: &Value) -> Option<FixedPaletteMode> {
    let terminal = obj.get("terminal").filter(|v| v.is_object())?;
    Some(FixedPaletteMode {
        palette: read_palette_json(obj),
        terminal: read_terminal_json(terminal),
    })
}

/// C++ `parseCommunityPaletteJson`: a dark mode with a terminal block is required;
/// a missing light mode mirrors dark; a present light mode without terminal is invalid.
pub fn parse_palette_json(text: &str) -> Option<FixedPalette> {
    let root: Value = serde_json::from_str(text).ok()?;
    if !root.is_object() {
        return None;
    }
    let dark = read_mode(root.get("dark").filter(|v| v.is_object())?)?;
    let light = match root.get("light").filter(|v| v.is_object()) {
        Some(l) => read_mode(l)?,
        None => dark,
    };
    Some(FixedPalette { dark, light })
}

pub fn builtin_palettes() -> &'static [BuiltinPalette] {
    static PALETTES: OnceLock<Vec<BuiltinPalette>> = OnceLock::new();
    PALETTES.get_or_init(|| {
        let root: Value = serde_json::from_str(include_str!("../data/builtin_palettes.json"))
            .expect("builtin_palettes.json is valid");
        root.as_array()
            .expect("builtin_palettes.json is an array")
            .iter()
            .map(|entry| BuiltinPalette {
                name: entry["name"].as_str().expect("palette name").to_string(),
                palette: FixedPalette {
                    dark: read_mode(&entry["dark"]).expect("dark mode"),
                    light: read_mode(&entry["light"]).expect("light mode"),
                },
            })
            .collect()
    })
}

pub fn find_builtin_palette(name: &str) -> Option<&'static BuiltinPalette> {
    builtin_palettes().iter().find(|p| p.name == name)
}

/// C++ `expandBuiltinPalette`.
pub fn expand_fixed_palette(palette: &FixedPalette) -> GeneratedPalette {
    let mut dark = expand_fixed_palette_mode(&palette.dark.palette, true);
    let mut light = expand_fixed_palette_mode(&palette.light.palette, false);
    apply_terminal_palette(&mut dark, &palette.dark.terminal);
    apply_terminal_palette(&mut light, &palette.light.terminal);
    GeneratedPalette { dark, light }
}

// ---- naming -------------------------------------------------------------------------

fn sanitize_component(raw: &str) -> String {
    let mut out = String::with_capacity(raw.len());
    let mut pending_dash = false;
    for ch in raw.chars() {
        if ch.is_ascii_alphanumeric() {
            if pending_dash && !out.is_empty() {
                out.push('-');
                pending_dash = false;
            }
            out.push(ch.to_ascii_lowercase());
        } else if !out.is_empty() {
            pending_dash = true;
        }
    }
    while out.ends_with('-') {
        out.pop();
    }
    out
}

pub fn custom_palette_path(dir: &Path, name: &str) -> PathBuf {
    dir.join(format!("{name}.json"))
}

/// C++ `shortSchemeTag`.
fn short_scheme_tag(scheme: &str) -> String {
    const TAGS: [(&str, &str); 10] = [
        ("m3-tonal-spot", "ts"),
        ("m3-content", "mc"),
        ("m3-fruit-salad", "fs"),
        ("m3-rainbow", "rb"),
        ("m3-monochrome", "mo"),
        ("vibrant", "vib"),
        ("faithful", "fth"),
        ("soft", "sft"),
        ("dysfunctional", "dys"),
        ("muted", "mut"),
    ];
    if let Some((_, tag)) = TAGS.iter().find(|(full, _)| *full == scheme) {
        return (*tag).to_string();
    }
    let mut fallback = sanitize_component(scheme);
    if fallback.len() > 4 {
        fallback.truncate(4);
        while fallback.ends_with('-') {
            fallback.pop();
        }
    }
    fallback
}

/// C++ `suggestCustomPaletteName(wallpaperPath, scheme)`.
pub fn suggest_custom_palette_name(wallpaper_path: &str, scheme: &str) -> String {
    let mut base = String::new();
    if let Some(color) = wallpaper_path.strip_prefix("color:") {
        let component = sanitize_component(color);
        base = if component.is_empty() {
            "solid".to_string()
        } else {
            format!("color-{component}")
        };
    } else if !wallpaper_path.is_empty() {
        let stem = Path::new(wallpaper_path)
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
        base = sanitize_component(&stem);
    }
    if base.is_empty() {
        base = "wallpaper".into();
    }
    let tag = short_scheme_tag(scheme);
    if !tag.is_empty() {
        base.push('-');
        base.push_str(&tag);
    }
    base
}

/// C++ `saveWallpaperPaletteAsCustom` (the file; the caller selects the palette).
pub fn save_wallpaper_as_custom(dir: &Path, sel: &ThemeSelection) -> Result<String, SaveError> {
    if sel.source != PaletteSource::Wallpaper {
        return Err("palette source is not wallpaper".to_string().into());
    }
    let Some(wallpaper) = &sel.wallpaper else {
        return Err(SaveError::WallpaperSource);
    };
    let name = allocate_custom_palette_name(
        dir,
        &suggest_custom_palette_name(&wallpaper.path, &wallpaper.scheme),
    );
    if name.is_empty() {
        return Err("failed to allocate custom palette name".to_string().into());
    }
    save_custom_palette(dir, &name, &wallpaper.generated)?;
    Ok(name)
}

/// C++ `allocateCustomPaletteName(base)`: sanitized, suffixed `-2`..`-999` on collision.
/// Empty when nothing usable is left.
pub fn allocate_custom_palette_name(dir: &Path, base: &str) -> String {
    let sanitized = sanitize_component(base);
    if sanitized.is_empty() {
        return String::new();
    }
    if !custom_palette_path(dir, &sanitized).exists() {
        return sanitized;
    }
    for suffix in 2..1000 {
        let candidate = format!("{sanitized}-{suffix}");
        if !custom_palette_path(dir, &candidate).exists() {
            return candidate;
        }
    }
    String::new()
}

// ---- writing ------------------------------------------------------------------------

fn write_ansi(out: &mut String, key: &str, colors: &[Color; 8], last: bool) {
    out.push_str(&format!("      \"{key}\": {{\n"));
    for (i, (name, c)) in ANSI_KEYS.iter().zip(colors).enumerate() {
        let comma = if i + 1 == ANSI_KEYS.len() { "" } else { "," };
        out.push_str(&format!("        \"{name}\": \"{}\"{comma}\n", c.to_hex()));
    }
    out.push_str(&format!("      }}{}\n", if last { "" } else { "," }));
}

fn write_mode(out: &mut String, key: &str, tokens: &TokenMap, last: bool) {
    let p = map_generated_palette_mode(tokens);
    let t = terminal_palette_from_tokens(tokens);
    out.push_str(&format!("  \"{key}\": {{\n"));
    let fields: [(&str, Color); 16] = [
        ("mPrimary", p.primary),
        ("mOnPrimary", p.on_primary),
        ("mSecondary", p.secondary),
        ("mOnSecondary", p.on_secondary),
        ("mTertiary", p.tertiary),
        ("mOnTertiary", p.on_tertiary),
        ("mError", p.error),
        ("mOnError", p.on_error),
        ("mSurface", p.surface),
        ("mOnSurface", p.on_surface),
        ("mSurfaceVariant", p.surface_variant),
        ("mOnSurfaceVariant", p.on_surface_variant),
        ("mOutline", p.outline),
        ("mShadow", p.shadow),
        ("mHover", p.hover),
        ("mOnHover", p.on_hover),
    ];
    for (name, c) in fields {
        out.push_str(&format!("    \"{name}\": \"{}\",\n", c.to_hex()));
    }
    out.push_str("    \"terminal\": {\n");
    // `write_ansi` indents for the mode nesting used by builtin export; re-indent here.
    let mut block = String::new();
    write_ansi(&mut block, "normal", &t.normal, false);
    write_ansi(&mut block, "bright", &t.bright, false);
    out.push_str(&block);
    let direct = [
        ("foreground", t.foreground),
        ("background", t.background),
        ("cursor", t.cursor),
        ("cursorText", t.cursor_text),
        ("selectionFg", t.selection_fg),
        ("selectionBg", t.selection_bg),
    ];
    for (i, (name, c)) in direct.iter().enumerate() {
        let comma = if i + 1 == direct.len() { "" } else { "," };
        out.push_str(&format!("      \"{name}\": \"{}\"{comma}\n", c.to_hex()));
    }
    out.push_str("    }\n");
    out.push_str(&format!("  }}{}\n", if last { "" } else { "," }));
}

/// Serializes like `nlohmann::ordered_json::dump(2)` (no trailing newline).
pub fn palette_json(generated: &GeneratedPalette) -> Option<String> {
    let mut dark = generated.dark.clone();
    let mut light = generated.light.clone();
    if !dark.is_empty() {
        synthesize_terminal_palette_tokens(&mut dark);
    }
    if !light.is_empty() {
        synthesize_terminal_palette_tokens(&mut light);
    }
    if dark.is_empty() && light.is_empty() {
        return None;
    }
    let mut out = String::from("{\n");
    if !dark.is_empty() {
        write_mode(&mut out, "dark", &dark, light.is_empty());
    }
    if !light.is_empty() {
        write_mode(&mut out, "light", &light, true);
    }
    out.push('}');
    Some(out)
}

/// C++ `saveCustomPaletteFromGenerated`. Returns the sanitized name actually used.
pub fn save_custom_palette(
    dir: &Path,
    name: &str,
    generated: &GeneratedPalette,
) -> Result<String, String> {
    let trimmed = sanitize_component(name);
    if trimmed.is_empty() {
        return Err("invalid custom palette name".into());
    }
    let json = palette_json(generated).ok_or_else(|| "generated palette is empty".to_string())?;
    std::fs::create_dir_all(dir).map_err(|_| "failed to create palettes directory".to_string())?;
    let path = custom_palette_path(dir, &trimmed);
    std::fs::write(&path, &json).map_err(|_| "failed to write custom palette file".to_string())?;
    if parse_palette_json(&json).is_none() {
        let _ = std::fs::remove_file(&path);
        return Err("saved custom palette failed validation".into());
    }
    Ok(trimmed)
}

/// C++ `applyEditablePaletteRole`. `false` for roles that are not editable.
pub fn apply_editable_palette_role(palette: &mut Palette, role: ColorRole, color: Color) -> bool {
    let seed = if crate::contrast::is_dark(color) {
        Color::new(255, 255, 255)
    } else {
        Color::new(0, 0, 0)
    };
    let on = Color::from_argb(ensure_contrast(seed, color, 4.5, 0).to_argb() & 0x00ff_ffff);
    match role {
        ColorRole::Primary => {
            palette.primary = color;
            palette.on_primary = on;
        }
        ColorRole::Secondary => {
            palette.secondary = color;
            palette.on_secondary = on;
        }
        ColorRole::Tertiary => {
            palette.tertiary = color;
            palette.on_tertiary = on;
            palette.hover = color;
            palette.on_hover = on;
        }
        ColorRole::Error => {
            palette.error = color;
            palette.on_error = on;
        }
        ColorRole::Surface => {
            palette.surface = color;
            palette.on_surface = on;
        }
        ColorRole::Outline => palette.outline = color,
        _ => return false,
    }
    true
}

// ---- ThemeService save paths --------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PaletteSource {
    Builtin,
    Wallpaper,
    Community,
    Custom,
}

/// The slice of `[theme]` config the save paths read, plus resolved shell mode.
#[derive(Clone, Debug)]
pub struct ThemeSelection {
    pub source: PaletteSource,
    pub builtin: String,
    pub custom_palette: String,
    /// Cached community palette file for the selected community palette, if any.
    pub community_cache: Option<PathBuf>,
    /// Resolved shell mode (`resolvedShellMode() == "light"`).
    pub shell_light: bool,
    /// The palette generated from the wallpaper, when the source is `wallpaper` and it could be generated.
    pub wallpaper: Option<WallpaperPalette>,
}

/// A palette generated from the wallpaper plus what its saved name is derived from.
#[derive(Clone, Debug)]
pub struct WallpaperPalette {
    pub generated: GeneratedPalette,
    /// The wallpaper path (or `color:#rrggbb`), as `suggestCustomPaletteName` reads it.
    pub path: String,
    /// The scheme name as configured (`m3-content`, `vibrant`, ...).
    pub scheme: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SaveError {
    /// The source is the wallpaper and no palette could be generated for it; callers fall back.
    WallpaperSource,
    Message(String),
}

impl From<String> for SaveError {
    fn from(message: String) -> Self {
        Self::Message(message)
    }
}

fn resolve_builtin(name: &str) -> GeneratedPalette {
    let palette = find_builtin_palette(name)
        .or_else(|| find_builtin_palette("Helix"))
        .expect("Helix builtin palette exists");
    expand_fixed_palette(&palette.palette)
}

fn read_parsed(path: &Path) -> Option<FixedPalette> {
    parse_palette_json(&std::fs::read_to_string(path).ok()?)
}

/// C++ `resolveFromSource` for non-wallpaper sources; `None` means "use builtin".
fn resolve_from_source(dir: &Path, sel: &ThemeSelection) -> Option<GeneratedPalette> {
    match sel.source {
        PaletteSource::Wallpaper => sel.wallpaper.as_ref().map(|w| w.generated.clone()),
        PaletteSource::Custom if !sel.custom_palette.is_empty() => {
            let path = custom_palette_path(dir, &sel.custom_palette);
            path.exists()
                .then(|| read_parsed(&path))
                .flatten()
                .map(|p| expand_fixed_palette(&p))
        }
        PaletteSource::Community => {
            let path = sel.community_cache.as_deref().filter(|p| p.exists())?;
            match read_parsed(path) {
                Some(parsed) => Some(expand_fixed_palette(&parsed)),
                None => {
                    // C++ drops an unreadable cache entry so it gets fetched again.
                    let _ = std::fs::remove_file(path);
                    None
                }
            }
        }
        PaletteSource::Builtin => Some(resolve_builtin(&sel.builtin)),
        _ => None,
    }
}

/// Palette for template apply. `None` when the wallpaper source has no decodable wallpaper.
pub fn resolve_palette(dir: &Path, sel: &ThemeSelection) -> Option<GeneratedPalette> {
    match sel.source {
        PaletteSource::Wallpaper => sel.wallpaper.as_ref().map(|w| w.generated.clone()),
        _ => Some(resolve(dir, sel)),
    }
}

fn resolve(dir: &Path, sel: &ThemeSelection) -> GeneratedPalette {
    resolve_from_source(dir, sel).unwrap_or_else(|| resolve_builtin(&sel.builtin))
}

/// `StringUtils::urlEncode`: everything but `[A-Za-z0-9-_.~]` becomes `%XX`.
pub fn url_encode(text: &str) -> String {
    let mut out = String::with_capacity(text.len() * 3);
    for b in text.bytes() {
        if b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b'.' | b'~') {
            out.push(char::from(b));
        } else {
            out.push_str(&format!("%{b:02X}"));
        }
    }
    out
}

/// `communityPaletteCachePath(name)` under the state dir (`<state>/community-palettes/`).
pub fn community_palette_cache_path(state_dir: &Path, name: &str) -> PathBuf {
    state_dir
        .join("community-palettes")
        .join(format!("{}.json", url_encode(name)))
}

/// C++ `saveRoleColorAsCustom` (local path). Returns the palette name; the caller must
/// then select it (`[theme] source = "custom"`, `custom_palette = name`).
pub fn save_role_color_as_custom(
    dir: &Path,
    sel: &ThemeSelection,
    role: ColorRole,
    color: Color,
) -> Result<String, SaveError> {
    if sel.source == PaletteSource::Wallpaper && sel.wallpaper.is_none() {
        return Err(SaveError::WallpaperSource);
    }
    let mut generated = resolve(dir, sel);
    let tokens = if sel.shell_light {
        &mut generated.light
    } else {
        &mut generated.dark
    };
    let mut palette = map_generated_palette_mode(tokens);
    if !apply_editable_palette_role(&mut palette, role, color) {
        return Err("unsupported palette role".to_string().into());
    }
    *tokens = expand_fixed_palette_mode(&palette, !sel.shell_light);

    let name = if sel.source == PaletteSource::Custom && !sel.custom_palette.is_empty() {
        sel.custom_palette.clone()
    } else {
        allocate_custom_palette_name(dir, "custom")
    };
    if name.is_empty() {
        return Err("failed to allocate custom palette name".to_string().into());
    }
    save_custom_palette(dir, &name, &generated)?;
    Ok(name)
}

/// C++ `saveCurrentPaletteAsCustom` (local path, non-wallpaper sources).
pub fn save_current_as_custom(dir: &Path, sel: &ThemeSelection) -> Result<String, SaveError> {
    if sel.source == PaletteSource::Wallpaper {
        return save_wallpaper_as_custom(dir, sel);
    }
    let generated = resolve(dir, sel);
    let name = allocate_custom_palette_name(dir, "custom");
    if name.is_empty() {
        return Err("failed to allocate custom palette name".to_string().into());
    }
    save_custom_palette(dir, &name, &generated)?;
    Ok(name)
}
