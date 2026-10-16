//! Parity with C++ `expandFixedPaletteMode` + terminal synthesis + `mapGeneratedPaletteMode`.
//! Regenerate with `tools/gen_golden_palette.cpp`.

use helix_theme::palette::{
    expand_fixed_palette_mode, map_generated_palette_mode, synthesize_terminal_palette_tokens,
    Palette,
};
use helix_theme::Color;

fn palette_from(colors: &[Color]) -> Palette {
    Palette {
        primary: colors[0],
        on_primary: colors[1],
        secondary: colors[2],
        on_secondary: colors[3],
        tertiary: colors[4],
        on_tertiary: colors[5],
        error: colors[6],
        on_error: colors[7],
        surface: colors[8],
        on_surface: colors[9],
        surface_variant: colors[10],
        on_surface_variant: colors[11],
        outline: colors[12],
        shadow: colors[13],
        hover: colors[14],
        on_hover: colors[15],
    }
}

fn hex_list(p: &Palette) -> String {
    [
        p.primary,
        p.on_primary,
        p.secondary,
        p.on_secondary,
        p.tertiary,
        p.on_tertiary,
        p.error,
        p.on_error,
        p.surface,
        p.on_surface,
        p.surface_variant,
        p.on_surface_variant,
        p.outline,
        p.shadow,
        p.hover,
        p.on_hover,
    ]
    .iter()
    .map(|c| c.to_hex())
    .collect::<Vec<_>>()
    .join(",")
}

#[test]
fn matches_cpp_reference() {
    let golden = include_str!("golden/palette_expand.tsv");
    let mut rows = 0;
    for line in golden.lines() {
        let f: Vec<&str> = line.split('\t').collect();
        let colors: Vec<Color> = f[0]
            .split(',')
            .map(|h| Color::from_hex(h).unwrap())
            .collect();
        let dark = f[1] == "1";
        let palette = palette_from(&colors);

        let mut tokens = expand_fixed_palette_mode(&palette, dark);
        synthesize_terminal_palette_tokens(&mut tokens);

        let mut want_tokens = 0;
        for field in &f[2..f.len() - 1] {
            let (key, value) = field.split_once('=').unwrap();
            let got = tokens
                .get(key)
                .unwrap_or_else(|| panic!("missing token {key}"));
            assert_eq!(
                format!("#{:06x}", got & 0xff_ffff),
                value,
                "row {rows} dark={dark} token {key}"
            );
            want_tokens += 1;
        }
        assert_eq!(tokens.len(), want_tokens, "row {rows} token count");

        let mapped = map_generated_palette_mode(&tokens);
        let want_mapped = f[f.len() - 1].strip_prefix("mapped=").unwrap();
        assert_eq!(hex_list(&mapped), want_mapped, "row {rows} mapped");
        rows += 1;
    }
    assert_eq!(rows, 240);
}
