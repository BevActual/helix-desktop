//! Parity with `tests/scheme_test.cpp` (scheme strings, soft generation, contrast headroom, terminals).

use helix_theme::contrast::{contrast_ratio, ensure_contrast};
use helix_theme::custom::{expand_fixed_palette, find_builtin_palette};
use helix_theme::custom_schemes::generate_custom;
use helix_theme::wallpaper::{generate, Scheme, SCHEME_NAMES};
use helix_theme::Color;

fn colorful_buffer() -> Vec<u8> {
    let mut rgb = vec![0u8; 112 * 112 * 3];
    for y in 0..112 {
        for x in 0..112 {
            let color = if x < 84 {
                [222, 59, 75]
            } else if y < 56 {
                [50, 124, 222]
            } else {
                [236, 166, 51]
            };
            let i = (y * 112 + x) * 3;
            rgb[i] = color[0];
            rgb[i + 1] = color[1];
            rgb[i + 2] = color[2];
        }
    }
    rgb
}

fn token(map: &std::collections::HashMap<String, u32>, name: &str) -> u32 {
    map.get(name).copied().unwrap_or(0)
}

fn saturation(argb: u32) -> f64 {
    let (_, s, _) = Color::from_argb(argb).to_hsl();
    s
}

#[test]
fn scheme_strings_round_trip() {
    assert_eq!(Scheme::parse("soft"), Some(Scheme::Soft));
    assert_eq!(Scheme::Soft.as_str(), "soft");
    assert!(Scheme::parse("softened").is_none());
}

#[test]
fn soft_generation_relationships() {
    let rgb = colorful_buffer();
    let faithful = generate_custom(&rgb, Scheme::Faithful);
    let soft = generate_custom(&rgb, Scheme::Soft);
    let muted = generate_custom(&rgb, Scheme::Muted);

    for name in [
        "source_color",
        "primary",
        "on_primary",
        "surface",
        "on_surface",
        "secondary",
        "tertiary",
        "terminal_normal_red",
    ] {
        assert!(soft.dark.contains_key(name), "soft dark missing {name}");
        assert!(soft.light.contains_key(name), "soft light missing {name}");
    }
    assert_eq!(token(&soft.dark, "source_color"), token(&faithful.dark, "source_color"));

    let muted_primary = saturation(token(&muted.dark, "primary"));
    let soft_primary = saturation(token(&soft.dark, "primary"));
    let faithful_primary = saturation(token(&faithful.dark, "primary"));
    assert!(muted_primary < soft_primary && soft_primary < faithful_primary);

    let muted_surface = saturation(token(&muted.dark, "surface"));
    let soft_surface = saturation(token(&soft.dark, "surface"));
    let faithful_surface = saturation(token(&faithful.dark, "surface"));
    assert!(muted_surface < soft_surface && soft_surface < faithful_surface);
}

#[test]
fn container_saturation_headroom() {
    let catppuccin = find_builtin_palette("Catppuccin").expect("Catppuccin");
    let fixed = expand_fixed_palette(&catppuccin.palette);
    let container = Color::from_argb(token(&fixed.dark, "primary_container"));
    assert_eq!(container.to_hex(), "#6d10da");

    let custom = generate_custom(&colorful_buffer(), Scheme::Faithful);
    let custom_primary_saturation = saturation(token(&custom.dark, "primary"));
    let custom_container_saturation = saturation(token(&custom.dark, "primary_container"));
    assert!(custom_container_saturation < custom_primary_saturation + 0.10);
}

#[test]
fn perceptual_contrast_adjustment() {
    let background = Color::from_hex("#fbf8ff").unwrap();
    let pale_lavender = Color::from_hex("#bdc2ff").unwrap();
    let adjusted = ensure_contrast(pale_lavender, background, 4.5, 0);
    assert!(contrast_ratio(adjusted, background) >= 4.5);
    assert_eq!(adjusted.to_hex(), "#6b6ea6");

    let readable = Color::from_hex("#5158a1").unwrap();
    assert_eq!(
        ensure_contrast(readable, background, 4.5, 0).to_argb(),
        readable.to_argb()
    );

    let impossible_background = Color::from_hex("#777777").unwrap();
    let impossible_foreground = Color::from_hex("#888888").unwrap();
    let best = ensure_contrast(impossible_foreground, impossible_background, 7.0, 1);
    assert_eq!(best.to_hex(), "#000000");
}

#[test]
fn terminal_and_accent_contrast_for_every_scheme() {
    const COLOR_KEYS: [&str; 12] = [
        "terminal_normal_red",
        "terminal_normal_green",
        "terminal_normal_yellow",
        "terminal_normal_blue",
        "terminal_normal_magenta",
        "terminal_normal_cyan",
        "terminal_bright_red",
        "terminal_bright_green",
        "terminal_bright_yellow",
        "terminal_bright_blue",
        "terminal_bright_magenta",
        "terminal_bright_cyan",
    ];
    const ACCENT_KEYS: [&str; 4] = ["primary", "secondary", "tertiary", "error"];

    let rgb = colorful_buffer();
    for (_, scheme) in SCHEME_NAMES {
        let label = scheme.as_str();
        let generated = generate(&rgb, scheme).expect(label);
        for (mode, tokens) in [("dark", &generated.dark), ("light", &generated.light)] {
            let background = Color::from_argb(
                *tokens
                    .get("terminal_background")
                    .expect(&format!("{label} {mode} terminal_background")),
            );
            for key in COLOR_KEYS {
                let color = Color::from_argb(*tokens.get(key).expect(&format!("{label} {mode} {key}")));
                assert!(
                    contrast_ratio(color, background) >= 4.5,
                    "{label} {mode} {key}"
                );
            }
            for key in ACCENT_KEYS {
                let bg = Color::from_argb(*tokens.get(key).expect(&format!("{label} {mode} {key}")));
                let fg = Color::from_argb(
                    *tokens
                        .get(&format!("on_{key}"))
                        .expect(&format!("{label} {mode} on_{key}")),
                );
                assert!(contrast_ratio(fg, bg) >= 4.5, "{label} {mode} on_{key}");
            }
        }
    }
}
