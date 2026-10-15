//! Wallpaper palette generation: a 112x112 RGB buffer plus a scheme in, dark and light token maps out. Port of C++
//! `palette_generator.cpp` / `m3_schemes.cpp` (the Material Design 3 path; the C++ matched matugen's `material_colors`
//! quantize and score one-for-one, so this uses that crate). The custom HSL schemes are in [`crate::custom_schemes`].

use material_colors::color::Rgb;
use material_colors::dynamic_color::{Platform, SpecVersion};
use material_colors::hct::{Cam16, Hct};
use material_colors::quantize::{Quantizer, QuantizerCelebi};
use material_colors::scheme::variant::{
    SchemeContent, SchemeFruitSalad, SchemeMonochrome, SchemeRainbow, SchemeTonalSpot,
};
use material_colors::scheme::Scheme as MaterialScheme;
use material_colors::score::Score;

use crate::custom::GeneratedPalette;
use crate::palette::{synthesize_terminal_palette_tokens, TokenMap};

pub const SIDE: usize = 112;

/// Color generation strategies. The first five are Material Design 3; the rest are custom HSL generators.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Scheme {
    TonalSpot,
    Content,
    FruitSalad,
    Rainbow,
    Monochrome,
    Vibrant,
    Faithful,
    Soft,
    Dysfunctional,
    Muted,
}

pub const SCHEME_NAMES: [(&str, Scheme); 10] = [
    ("m3-tonal-spot", Scheme::TonalSpot),
    ("m3-content", Scheme::Content),
    ("m3-fruit-salad", Scheme::FruitSalad),
    ("m3-rainbow", Scheme::Rainbow),
    ("m3-monochrome", Scheme::Monochrome),
    ("vibrant", Scheme::Vibrant),
    ("faithful", Scheme::Faithful),
    ("soft", Scheme::Soft),
    ("dysfunctional", Scheme::Dysfunctional),
    ("muted", Scheme::Muted),
];

impl Scheme {
    pub fn parse(name: &str) -> Option<Self> {
        SCHEME_NAMES.iter().find(|(n, _)| *n == name).map(|(_, s)| *s)
    }

    pub fn is_material(self) -> bool {
        matches!(
            self,
            Self::TonalSpot | Self::Content | Self::FruitSalad | Self::Rainbow | Self::Monochrome
        )
    }
}

pub fn generate(rgb112: &[u8], scheme: Scheme) -> Result<GeneratedPalette, String> {
    if rgb112.len() != SIDE * SIDE * 3 {
        return Err("expected 112x112x3 pixel buffer".into());
    }
    let palette = if scheme.is_material() {
        generate_material(rgb112, scheme)
    } else {
        crate::custom_schemes::generate_custom(rgb112, scheme)
    };
    if palette.dark.is_empty() && palette.light.is_empty() {
        return Err("palette generation produced no token maps".into());
    }
    if palette.dark.is_empty() {
        return Err("palette generation produced an empty dark token map".into());
    }
    if palette.light.is_empty() {
        return Err("palette generation produced an empty light token map".into());
    }
    Ok(palette)
}

fn argb(color: Rgb) -> u32 {
    0xff00_0000 | (u32::from(color.red) << 16) | (u32::from(color.green) << 8) | u32::from(color.blue)
}

/// Quantize, drop low-chroma clusters (as matugen does), score, take the first ranked colour.
fn extract_seed(rgb112: &[u8]) -> Rgb {
    let pixels: Vec<Rgb> = rgb112
        .chunks_exact(3)
        .map(|p| Rgb::new(p[0], p[1], p[2]))
        .collect();
    let mut result = QuantizerCelebi::quantize(&pixels, 128);
    result
        .color_to_count
        .retain(|color, _| Cam16::from(*color).chroma >= 5.0);
    let ranked = Score::score(&result.color_to_count, Some(4), None, Some(true));
    ranked
        .first()
        .copied()
        .unwrap_or_else(|| Rgb::new(66, 133, 244))
}

fn token_map(scheme: &MaterialScheme) -> TokenMap {
    let mut m = TokenMap::new();
    let mut set = |key: &str, color: Rgb| {
        m.insert(key.to_string(), argb(color));
    };
    set("primary", scheme.primary);
    set("on_primary", scheme.on_primary);
    set("primary_container", scheme.primary_container);
    set("on_primary_container", scheme.on_primary_container);
    set("inverse_primary", scheme.inverse_primary);
    set("surface_tint", scheme.surface_tint);
    set("secondary", scheme.secondary);
    set("on_secondary", scheme.on_secondary);
    set("secondary_container", scheme.secondary_container);
    set("on_secondary_container", scheme.on_secondary_container);
    set("tertiary", scheme.tertiary);
    set("on_tertiary", scheme.on_tertiary);
    set("tertiary_container", scheme.tertiary_container);
    set("on_tertiary_container", scheme.on_tertiary_container);
    set("error", scheme.error);
    set("on_error", scheme.on_error);
    set("error_container", scheme.error_container);
    set("on_error_container", scheme.on_error_container);
    set("surface", scheme.surface);
    set("on_surface", scheme.on_surface);
    set("surface_variant", scheme.surface_variant);
    set("on_surface_variant", scheme.on_surface_variant);
    set("surface_dim", scheme.surface_dim);
    set("surface_bright", scheme.surface_bright);
    set("surface_container_lowest", scheme.surface_container_lowest);
    set("surface_container_low", scheme.surface_container_low);
    set("surface_container", scheme.surface_container);
    set("surface_container_high", scheme.surface_container_high);
    set("surface_container_highest", scheme.surface_container_highest);
    set("outline", scheme.outline);
    set("outline_variant", scheme.outline_variant);
    set("shadow", scheme.shadow);
    set("scrim", scheme.scrim);
    set("inverse_surface", scheme.inverse_surface);
    set("inverse_on_surface", scheme.inverse_on_surface);
    set("background", scheme.background);
    set("on_background", scheme.on_background);
    set("primary_fixed", scheme.primary_fixed);
    set("primary_fixed_dim", scheme.primary_fixed_dim);
    set("on_primary_fixed", scheme.on_primary_fixed);
    set("on_primary_fixed_variant", scheme.on_primary_fixed_variant);
    set("secondary_fixed", scheme.secondary_fixed);
    set("secondary_fixed_dim", scheme.secondary_fixed_dim);
    set("on_secondary_fixed", scheme.on_secondary_fixed);
    set("on_secondary_fixed_variant", scheme.on_secondary_fixed_variant);
    set("tertiary_fixed", scheme.tertiary_fixed);
    set("tertiary_fixed_dim", scheme.tertiary_fixed_dim);
    set("on_tertiary_fixed", scheme.on_tertiary_fixed);
    set("on_tertiary_fixed_variant", scheme.on_tertiary_fixed_variant);
    m
}

fn make_scheme(source: Hct, scheme: Scheme, is_dark: bool) -> MaterialScheme {
    // The C++ built MCU's schemes, which are the 2021 spec on phone.
    let (spec, platform) = (SpecVersion::Spec2021, Platform::Phone);
    let dynamic = match scheme {
        Scheme::Content => SchemeContent::with_spec(source, is_dark, Some(0.0), spec, platform).scheme,
        Scheme::FruitSalad => SchemeFruitSalad::with_spec(source, is_dark, Some(0.0), spec, platform).scheme,
        Scheme::Rainbow => SchemeRainbow::with_spec(source, is_dark, Some(0.0), spec, platform).scheme,
        Scheme::Monochrome => SchemeMonochrome::with_spec(source, is_dark, Some(0.0), spec, platform).scheme,
        _ => SchemeTonalSpot::with_spec(source, is_dark, Some(0.0), spec, platform).scheme,
    };
    MaterialScheme::from(dynamic)
}

fn generate_material(rgb112: &[u8], scheme: Scheme) -> GeneratedPalette {
    let seed = extract_seed(rgb112);
    let source = <Hct as From<Rgb>>::from(seed);
    let mut dark = token_map(&make_scheme(source, scheme, true));
    let mut light = token_map(&make_scheme(source, scheme, false));
    dark.insert("source_color".into(), argb(seed));
    light.insert("source_color".into(), argb(seed));
    synthesize_terminal_palette_tokens(&mut dark);
    synthesize_terminal_palette_tokens(&mut light);
    GeneratedPalette { dark, light }
}
