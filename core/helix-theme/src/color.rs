//! RGB colour (0-255 per channel) with hex/HSL/ARGB conversions (`theme/color.cpp`).

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Color {
    pub r: i32,
    pub g: i32,
    pub b: i32,
}

fn round_clamp_255(v: f64) -> i32 {
    // `f64::round` rounds half away from zero, matching `std::lround`.
    ((v * 255.0).round() as i64).clamp(0, 255) as i32
}

impl Color {
    pub const fn new(r: i32, g: i32, b: i32) -> Self {
        Self { r, g, b }
    }

    /// Accepts `#RRGGBB` or `RRGGBB`.
    pub fn from_hex(hex: &str) -> Option<Self> {
        let hex = hex.strip_prefix('#').unwrap_or(hex);
        if hex.len() != 6 || !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
            return None;
        }
        let byte = |i: usize| u8::from_str_radix(&hex[i..i + 2], 16).ok().map(i32::from);
        Some(Self::new(byte(0)?, byte(2)?, byte(4)?))
    }

    pub fn from_argb(argb: u32) -> Self {
        Self::new(
            ((argb >> 16) & 0xff) as i32,
            ((argb >> 8) & 0xff) as i32,
            (argb & 0xff) as i32,
        )
    }

    pub fn to_argb(self) -> u32 {
        0xff00_0000 | ((self.r as u32) << 16) | ((self.g as u32) << 8) | (self.b as u32)
    }

    /// `#rrggbb`
    pub fn to_hex(self) -> String {
        format!(
            "#{:02x}{:02x}{:02x}",
            self.r & 0xff,
            self.g & 0xff,
            self.b & 0xff
        )
    }

    /// `(h°, s, l)` with h in `[0, 360)`.
    pub fn to_hsl(self) -> (f64, f64, f64) {
        let rn = f64::from(self.r) / 255.0;
        let gn = f64::from(self.g) / 255.0;
        let bn = f64::from(self.b) / 255.0;
        let max_c = rn.max(gn).max(bn);
        let min_c = rn.min(gn).min(bn);
        let delta = max_c - min_c;

        let l = (max_c + min_c) / 2.0;
        let mut h = 0.0;
        let mut s = 0.0;
        if delta != 0.0 {
            if l != 0.0 && l != 1.0 {
                s = delta / (1.0 - (2.0 * l - 1.0).abs());
            }
            if max_c == rn {
                // `%` keeps the dividend's sign, like `std::fmod`.
                let mut t = ((gn - bn) / delta) % 6.0;
                if t < 0.0 {
                    t += 6.0;
                }
                h = 60.0 * t;
            } else if max_c == gn {
                h = 60.0 * ((bn - rn) / delta + 2.0);
            } else {
                h = 60.0 * ((rn - gn) / delta + 4.0);
            }
        }
        (h, s, l)
    }

    pub fn from_hsl(h: f64, s: f64, l: f64) -> Self {
        if s == 0.0 {
            let v = round_clamp_255(l);
            return Self::new(v, v, v);
        }
        let q = if l < 0.5 {
            l * (1.0 + s)
        } else {
            l + s - l * s
        };
        let p = 2.0 * l - q;
        let hn = h / 360.0;

        let hue_to_rgb = |mut t: f64| -> f64 {
            if t < 0.0 {
                t += 1.0;
            }
            if t > 1.0 {
                t -= 1.0;
            }
            if t < 1.0 / 6.0 {
                p + (q - p) * 6.0 * t
            } else if t < 1.0 / 2.0 {
                q
            } else if t < 2.0 / 3.0 {
                p + (q - p) * (2.0 / 3.0 - t) * 6.0
            } else {
                p
            }
        };

        Self::new(
            round_clamp_255(hue_to_rgb(hn + 1.0 / 3.0)),
            round_clamp_255(hue_to_rgb(hn)),
            round_clamp_255(hue_to_rgb(hn - 1.0 / 3.0)),
        )
    }
}

/// Shortest circular hue distance in degrees (result in `[0, 180]`).
pub fn hue_distance(h1: f64, h2: f64) -> f64 {
    let diff = (h1 - h2).abs();
    diff.min(360.0 - diff)
}

/// Shift a colour's hue by `degrees`, preserving saturation and lightness.
pub fn shift_hue(c: Color, degrees: f64) -> Color {
    let (h, s, l) = c.to_hsl();
    let mut new_h = (h + degrees) % 360.0;
    if new_h < 0.0 {
        new_h += 360.0;
    }
    Color::from_hsl(new_h, s, l)
}

/// Keep `base`'s hue, cap saturation at `s_max`, force lightness to `l_target`.
pub fn adjust_surface(base: Color, s_max: f64, l_target: f64) -> Color {
    let (h, s, _) = base.to_hsl();
    Color::from_hsl(h, s.min(s_max), l_target)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_round_trip() {
        let c = Color::from_hex("#1a2B3c").unwrap();
        assert_eq!(c, Color::new(0x1a, 0x2b, 0x3c));
        assert_eq!(c.to_hex(), "#1a2b3c");
        assert_eq!(Color::from_hex("1a2b3c"), Some(c));
        assert_eq!(Color::from_hex("#12345"), None);
        assert_eq!(Color::from_hex("#gggggg"), None);
        assert_eq!(Color::from_hex("#+1+2+3"), None);
    }

    #[test]
    fn hsl_round_trip_is_stable_for_primaries() {
        for hex in [
            "#ff0000", "#00ff00", "#0000ff", "#808080", "#000000", "#ffffff",
        ] {
            let c = Color::from_hex(hex).unwrap();
            let (h, s, l) = c.to_hsl();
            assert_eq!(Color::from_hsl(h, s, l), c, "{hex}");
        }
    }

    #[test]
    fn argb_has_opaque_alpha() {
        let c = Color::new(1, 2, 3);
        assert_eq!(c.to_argb(), 0xff01_0203);
        assert_eq!(Color::from_argb(c.to_argb()), c);
    }
}
