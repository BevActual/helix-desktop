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
        SCHEME_NAMES
            .iter()
            .find(|(n, _)| *n == name)
            .map(|(_, s)| *s)
    }

    pub fn as_str(self) -> &'static str {
        SCHEME_NAMES
            .iter()
            .find(|(_, scheme)| *scheme == self)
            .map(|(name, _)| *name)
            .expect("every Scheme has a canonical name")
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
    0xff00_0000
        | (u32::from(color.red) << 16)
        | (u32::from(color.green) << 8)
        | u32::from(color.blue)
}

/// Quantize, drop low-chroma clusters (as matugen does), score, take the first ranked colour.
fn extract_seed(rgb112: &[u8]) -> Rgb {
    let pixels: Vec<Rgb> = rgb112
        .as_chunks::<3>()
        .0
        .iter()
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
    set(
        "surface_container_highest",
        scheme.surface_container_highest,
    );
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
    set(
        "on_secondary_fixed_variant",
        scheme.on_secondary_fixed_variant,
    );
    set("tertiary_fixed", scheme.tertiary_fixed);
    set("tertiary_fixed_dim", scheme.tertiary_fixed_dim);
    set("on_tertiary_fixed", scheme.on_tertiary_fixed);
    set(
        "on_tertiary_fixed_variant",
        scheme.on_tertiary_fixed_variant,
    );
    m
}

fn make_scheme(source: Hct, scheme: Scheme, is_dark: bool) -> MaterialScheme {
    // The C++ built MCU's schemes, which are the 2021 spec on phone.
    let (spec, platform) = (SpecVersion::Spec2021, Platform::Phone);
    let dynamic = match scheme {
        Scheme::Content => {
            SchemeContent::with_spec(source, is_dark, Some(0.0), spec, platform).scheme
        }
        Scheme::FruitSalad => {
            SchemeFruitSalad::with_spec(source, is_dark, Some(0.0), spec, platform).scheme
        }
        Scheme::Rainbow => {
            SchemeRainbow::with_spec(source, is_dark, Some(0.0), spec, platform).scheme
        }
        Scheme::Monochrome => {
            SchemeMonochrome::with_spec(source, is_dark, Some(0.0), spec, platform).scheme
        }
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

// ---- loading a wallpaper ---------------------------------------------------------------------------------------------------

/// Decode `path` and force-resize it to 112x112 RGB (aspect ratio ignored): a triangle filter for the Material schemes (a
/// port of the `image` crate's resize, which matugen uses) and a box filter for the custom ones. Both are hand-ported from
/// C++ `image_loader.cpp` because a few LSB of drift in the buffer can move the seed to another cluster.
pub fn load_and_resize(path: &std::path::Path, scheme: Scheme) -> Result<Vec<u8>, String> {
    use image::ImageDecoder;
    let bytes = std::fs::read(path).map_err(|_| "cannot open file".to_string())?;
    if bytes.is_empty() {
        return Err("empty image file".into());
    }
    let mut decoder = image::ImageReader::new(std::io::Cursor::new(&bytes))
        .with_guessed_format()
        .map_err(|e| e.to_string())?
        .into_decoder()
        .map_err(|e| e.to_string())?;
    let orientation = decoder
        .orientation()
        .unwrap_or(image::metadata::Orientation::NoTransforms);
    let mut picture = image::DynamicImage::from_decoder(decoder).map_err(|e| e.to_string())?;
    picture.apply_orientation(orientation);
    let rgba = picture.to_rgba8();
    let (width, height) = (rgba.width() as usize, rgba.height() as usize);
    if width == 0 || height == 0 {
        return Err("invalid image dimensions".into());
    }
    let resized = if scheme.is_material() {
        triangle_resize(rgba.as_raw(), width, height, SIDE, SIDE)
    } else {
        box_resize(rgba.as_raw(), width, height, SIDE, SIDE)
    };
    let mut rgb = Vec::with_capacity(SIDE * SIDE * 3);
    for pixel in resized.as_chunks::<4>().0 {
        rgb.extend_from_slice(&pixel[..3]);
    }
    Ok(rgb)
}

fn to_u8(v: f32) -> u8 {
    (v.clamp(0.0, 255.0) + 0.5).floor() as u8
}

fn triangle_kernel(x: f32) -> f32 {
    let x = x.abs();
    if x < 1.0 {
        1.0 - x
    } else {
        0.0
    }
}

/// Weights for output index `out` of a triangle-filtered axis, and the first source index they apply to.
fn triangle_weights(out: usize, src_len: usize, dst_len: usize) -> (usize, Vec<f32>) {
    let ratio = src_len as f32 / dst_len as f32;
    let sratio = if ratio < 1.0 { 1.0 } else { ratio };
    let support = sratio;
    let input_orig = (out as f32 + 0.5) * ratio;
    let mut left = (input_orig - support).floor() as i64;
    let mut right = (input_orig + support).ceil() as i64;
    left = left.max(0).min(src_len as i64 - 1);
    right = right.max(left + 1).min(src_len as i64);
    let input = input_orig - 0.5;
    let mut weights: Vec<f32> = (left..right)
        .map(|i| triangle_kernel((i as f32 - input) / sratio))
        .collect();
    let sum: f32 = weights.iter().sum();
    for w in &mut weights {
        *w /= sum;
    }
    (left as usize, weights)
}

fn triangle_resize(src: &[u8], src_w: usize, src_h: usize, dst_w: usize, dst_h: usize) -> Vec<u8> {
    // Vertical first into f32 (no clamp or rounding between passes), then horizontal into u8.
    let mut tmp = vec![0f32; src_w * dst_h * 4];
    for outy in 0..dst_h {
        let (left, weights) = triangle_weights(outy, src_h, dst_h);
        for x in 0..src_w {
            let mut t = [0f32; 4];
            for (k, w) in weights.iter().enumerate() {
                let p = ((left + k) * src_w + x) * 4;
                for c in 0..4 {
                    t[c] += f32::from(src[p + c]) * w;
                }
            }
            let d = (outy * src_w + x) * 4;
            tmp[d..d + 4].copy_from_slice(&t);
        }
    }
    let mut dst = vec![0u8; dst_w * dst_h * 4];
    for outx in 0..dst_w {
        let (left, weights) = triangle_weights(outx, src_w, dst_w);
        for y in 0..dst_h {
            let mut t = [0f32; 4];
            for (k, w) in weights.iter().enumerate() {
                let p = (y * src_w + (left + k)) * 4;
                for c in 0..4 {
                    t[c] += tmp[p + c] * w;
                }
            }
            let d = (y * dst_w + outx) * 4;
            for c in 0..4 {
                dst[d + c] = to_u8(t[c]);
            }
        }
    }
    dst
}

/// `(start, count, weight)` of the box window for output index `out`.
fn box_window(out: usize, src_len: usize, dst_len: usize) -> Option<(usize, usize, f32)> {
    const EPSILON: f64 = 1.0e-12;
    let factor = dst_len as f64 / src_len as f64;
    let mut scale = (1.0 / factor + EPSILON).max(1.0);
    let mut support = scale * 0.5;
    if support < 0.5 {
        support = 0.5;
        scale = 1.0;
    }
    let _ = scale;
    let bisect = (out as f64 + 0.5) / factor + EPSILON;
    let start = (bisect - support + 0.5).max(0.0) as i64;
    let stop = (bisect + support + 0.5).min(src_len as f64) as i64;
    let count = stop - start;
    if count <= 0 {
        return None;
    }
    Some((start as usize, count as usize, 1.0 / count as f32))
}

fn box_resize(src: &[u8], src_w: usize, src_h: usize, dst_w: usize, dst_h: usize) -> Vec<u8> {
    let mut tmp = vec![0f32; src_w * dst_h * 4];
    for outy in 0..dst_h {
        let Some((start, count, weight)) = box_window(outy, src_h, dst_h) else {
            continue;
        };
        for x in 0..src_w {
            let mut t = [0f32; 4];
            for i in start..start + count {
                let p = (i * src_w + x) * 4;
                for c in 0..4 {
                    t[c] += f32::from(src[p + c]) * weight;
                }
            }
            let d = (outy * src_w + x) * 4;
            tmp[d..d + 4].copy_from_slice(&t);
        }
    }
    let mut dst = vec![0u8; dst_w * dst_h * 4];
    for outx in 0..dst_w {
        let Some((start, count, weight)) = box_window(outx, src_w, dst_w) else {
            continue;
        };
        for y in 0..dst_h {
            let mut t = [0f32; 4];
            for i in start..start + count {
                let p = (y * src_w + i) * 4;
                for c in 0..4 {
                    t[c] += tmp[p + c] * weight;
                }
            }
            let d = (y * dst_w + outx) * 4;
            for c in 0..4 {
                dst[d + c] = to_u8(t[c]);
            }
        }
    }
    dst
}
