//! WCAG contrast and OKLCH-based contrast adjustment (`theme/contrast.cpp`).

use crate::color::Color;

#[derive(Clone, Copy)]
struct Oklch {
    lightness: f64,
    chroma: f64,
    hue_radians: f64,
}

#[derive(Clone, Copy)]
struct LinearRgb {
    red: f64,
    green: f64,
    blue: f64,
}

fn linearize_channel(channel: i32) -> f64 {
    let normalized = f64::from(channel) / 255.0;
    if normalized <= 0.04045 {
        normalized / 12.92
    } else {
        ((normalized + 0.055) / 1.055).powf(2.4)
    }
}

fn delinearize_channel(channel: f64) -> f64 {
    if channel <= 0.003_130_8 {
        channel * 12.92
    } else {
        1.055 * channel.powf(1.0 / 2.4) - 0.055
    }
}

fn to_oklch(color: Color) -> Oklch {
    let red = linearize_channel(color.r);
    let green = linearize_channel(color.g);
    let blue = linearize_channel(color.b);

    let l = (0.4122214708 * red + 0.5363325363 * green + 0.0514459929 * blue).cbrt();
    let m = (0.2119034982 * red + 0.6806995451 * green + 0.1073969566 * blue).cbrt();
    let s = (0.0883024619 * red + 0.2817188376 * green + 0.6299787005 * blue).cbrt();

    let lightness = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    let a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    let b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    Oklch {
        lightness,
        chroma: a.hypot(b),
        hue_radians: b.atan2(a),
    }
}

fn from_oklch(color: Oklch) -> LinearRgb {
    let a = color.chroma * color.hue_radians.cos();
    let b = color.chroma * color.hue_radians.sin();
    let l_root = color.lightness + 0.3963377774 * a + 0.2158037573 * b;
    let m_root = color.lightness - 0.1055613458 * a - 0.0638541728 * b;
    let s_root = color.lightness - 0.0894841775 * a - 1.2914855480 * b;
    let l = l_root * l_root * l_root;
    let m = m_root * m_root * m_root;
    let s = s_root * s_root * s_root;
    LinearRgb {
        red: 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        green: -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        blue: -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
    }
}

fn is_in_srgb_gamut(c: LinearRgb) -> bool {
    const TOLERANCE: f64 = 1e-7;
    [c.red, c.green, c.blue]
        .iter()
        .all(|v| *v >= -TOLERANCE && *v <= 1.0 + TOLERANCE)
}

fn to_srgb_color(c: LinearRgb) -> Color {
    let to_byte =
        |channel: f64| (delinearize_channel(channel).clamp(0.0, 1.0) * 255.0).round() as i32;
    Color::new(to_byte(c.red), to_byte(c.green), to_byte(c.blue))
}

fn gamut_map_to_srgb(mut color: Oklch) -> Color {
    let linear = from_oklch(color);
    if is_in_srgb_gamut(linear) {
        return to_srgb_color(linear);
    }

    let mut low = 0.0;
    let mut high = color.chroma;
    color.chroma = 0.0;
    let mut best = from_oklch(color);
    for _ in 0..20 {
        let candidate_chroma = (low + high) / 2.0;
        color.chroma = candidate_chroma;
        let candidate = from_oklch(color);
        if is_in_srgb_gamut(candidate) {
            low = candidate_chroma;
            best = candidate;
        } else {
            high = candidate_chroma;
        }
    }
    to_srgb_color(best)
}

pub fn relative_luminance(r: i32, g: i32, b: i32) -> f64 {
    0.2126 * linearize_channel(r) + 0.7152 * linearize_channel(g) + 0.0722 * linearize_channel(b)
}

pub fn contrast_ratio(a: Color, b: Color) -> f64 {
    let l1 = relative_luminance(a.r, a.g, a.b);
    let l2 = relative_luminance(b.r, b.g, b.b);
    (l1.max(l2) + 0.05) / (l1.min(l2) + 0.05)
}

pub fn is_dark(c: Color) -> bool {
    relative_luminance(c.r, c.g, c.b) < 0.179
}

/// Binary-search the foreground's OKLCH lightness toward black or white until its
/// WCAG contrast ratio against `background` meets `min_ratio`. Chroma is reduced
/// only when needed to stay inside sRGB. `prefer_light`: `>0` lighten, `<0` darken,
/// `0` decide from the background.
pub fn ensure_contrast(
    foreground: Color,
    background: Color,
    min_ratio: f64,
    prefer_light: i32,
) -> Color {
    if contrast_ratio(foreground, background) >= min_ratio {
        return foreground;
    }

    let source = to_oklch(foreground);
    let lighten = match prefer_light {
        n if n > 0 => true,
        n if n < 0 => false,
        _ => is_dark(background),
    };

    let endpoint_at = |lightness: f64| {
        gamut_map_to_srgb(Oklch {
            lightness,
            chroma: source.chroma,
            hue_radians: source.hue_radians,
        })
    };
    let preferred_endpoint = endpoint_at(if lighten { 1.0 } else { 0.0 });
    let preferred_ratio = contrast_ratio(preferred_endpoint, background);
    if preferred_ratio < min_ratio {
        let opposite = endpoint_at(if lighten { 0.0 } else { 1.0 });
        return if contrast_ratio(opposite, background) > preferred_ratio {
            opposite
        } else {
            preferred_endpoint
        };
    }

    let (mut low, mut high) = if lighten {
        (source.lightness, 1.0)
    } else {
        (0.0, source.lightness)
    };
    let mut best = preferred_endpoint;
    for _ in 0..20 {
        let lightness = (low + high) / 2.0;
        let test = endpoint_at(lightness);
        if contrast_ratio(test, background) >= min_ratio {
            best = test;
            if lighten {
                high = lightness;
            } else {
                low = lightness;
            }
        } else if lighten {
            low = lightness;
        } else {
            high = lightness;
        }
    }
    best
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn black_on_white_is_21() {
        let r = contrast_ratio(Color::new(0, 0, 0), Color::new(255, 255, 255));
        assert!((r - 21.0).abs() < 1e-9);
    }

    #[test]
    fn ensure_contrast_meets_ratio_and_keeps_passing_colors() {
        let bg = Color::new(0x20, 0x20, 0x28);
        let ok = Color::new(0xf0, 0xf0, 0xf0);
        assert_eq!(ensure_contrast(ok, bg, 4.5, 0), ok);

        let weak = Color::new(0x30, 0x30, 0x40);
        let fixed = ensure_contrast(weak, bg, 4.5, 0);
        assert!(contrast_ratio(fixed, bg) >= 4.5);
        assert!(!is_dark(fixed));
    }
}
