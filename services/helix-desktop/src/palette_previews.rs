//! Builtin palette preview chips for Appearance1 (surface + accents only).
//! Static preview chips (surface + accents); full palette math lives in `helix-theme`.

use serde_json::{json, Value};

struct ModePreview {
    surface: u32,
    accents: [u32; 3],
}

struct BuiltinPreview {
    name: &'static str,
    dark: ModePreview,
    light: ModePreview,
}

const BUILTINS: &[BuiltinPreview] = &[
    BuiltinPreview {
        name: "Ayu",
        dark: ModePreview { surface: 0x0b0e14, accents: [0xe6b450, 0xaad94c, 0x39bae6] },
        light: ModePreview { surface: 0xf8f9fa, accents: [0xff8f40, 0x86b300, 0x55b4d4] },
    },
    BuiltinPreview {
        name: "Catppuccin",
        dark: ModePreview { surface: 0x1e1e2e, accents: [0xcba6f7, 0xfab387, 0x94e2d5] },
        light: ModePreview { surface: 0xeff1f5, accents: [0x8839ef, 0xfe640b, 0x40a02b] },
    },
    BuiltinPreview {
        name: "Dracula",
        dark: ModePreview { surface: 0x282a36, accents: [0xbd93f9, 0xff79c6, 0x8be9fd] },
        light: ModePreview { surface: 0xf8f8f2, accents: [0x8332f4, 0xff1399, 0x0398b9] },
    },
    BuiltinPreview {
        name: "Eldritch",
        dark: ModePreview { surface: 0x212337, accents: [0x37f499, 0x04d1f9, 0xa48cf2] },
        light: ModePreview { surface: 0xf0f3f4, accents: [0xfb5bb6, 0x0ad6ff, 0x8a69f7] },
    },
    BuiltinPreview {
        name: "Gruvbox",
        dark: ModePreview { surface: 0x282828, accents: [0xb8bb26, 0xfabd2f, 0x83a598] },
        light: ModePreview { surface: 0xfbf1c7, accents: [0x98971a, 0xd79921, 0x458588] },
    },
    BuiltinPreview {
        name: "Kanagawa",
        dark: ModePreview { surface: 0x1f1f28, accents: [0x76946a, 0xc0a36e, 0x7e9cd8] },
        light: ModePreview { surface: 0xf2ecbc, accents: [0x6f894e, 0x77713f, 0x4d699b] },
    },
    BuiltinPreview {
        name: "Helix",
        dark: ModePreview { surface: 0x070722, accents: [0xfff59b, 0xa9aefe, 0x9bfece] },
        light: ModePreview { surface: 0xe6e8fa, accents: [0x5d65f5, 0x8e93d8, 0x0e0e43] },
    },
    BuiltinPreview {
        name: "Nord",
        dark: ModePreview { surface: 0x2e3440, accents: [0x8fbcbb, 0x88c0d0, 0x5e81ac] },
        light: ModePreview { surface: 0xeceff4, accents: [0x5e81ac, 0x64adc2, 0x6fa9a8] },
    },
    BuiltinPreview {
        name: "Rosé Pine",
        dark: ModePreview { surface: 0x191724, accents: [0xebbcba, 0x9ccfd8, 0x31748f] },
        light: ModePreview { surface: 0xfffaf3, accents: [0xd7827e, 0x56949f, 0x286983] },
    },
    BuiltinPreview {
        name: "Tokyo-Night",
        dark: ModePreview { surface: 0x1a1b26, accents: [0x7aa2f7, 0xbb9af7, 0x9ece6a] },
        light: ModePreview { surface: 0xe1e2e7, accents: [0x2e7de9, 0x9854f1, 0x587539] },
    },
];

fn mode_json(mode: &ModePreview) -> Value {
    json!({
        "surface": mode.surface,
        "accents": mode.accents.to_vec(),
    })
}

/// `palette-previews` array entries for GetSnapshot.
pub fn builtin_palette_previews() -> Vec<Value> {
    BUILTINS
        .iter()
        .map(|palette| {
            json!({
                "source": "builtin",
                "name": palette.name,
                "dark": mode_json(&palette.dark),
                "light": mode_json(&palette.light),
            })
        })
        .collect()
}
