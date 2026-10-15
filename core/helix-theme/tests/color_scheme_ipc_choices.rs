//! Parity with `tests/cli_schema_test.cpp` (`color-scheme-set` positional choices).

use helix_theme::custom::builtin_palettes;
use helix_theme::wallpaper::{Scheme, SCHEME_NAMES};

#[test]
fn builtin_subcommand_choices_match_runtime_palettes() {
    let palettes = builtin_palettes();
    let choices: Vec<&str> = palettes.iter().map(|p| p.name.as_str()).collect();
    assert_eq!(choices.len(), palettes.len());
    for (i, palette) in palettes.iter().enumerate() {
        assert_eq!(choices[i], palette.name);
    }
}

#[test]
fn wallpaper_subcommand_choices_match_scheme_names() {
    assert_eq!(SCHEME_NAMES.len(), 10);
    for (i, (name, scheme)) in SCHEME_NAMES.iter().enumerate() {
        assert_eq!(Scheme::parse(name), Some(*scheme));
        assert_eq!(Scheme::parse(name).unwrap().as_str(), *name);
        // C++ `schemeToString` round-trip for each IPC choice.
        assert_eq!(SCHEME_NAMES[i].0, *name);
    }
    assert!(Scheme::parse("softened").is_none());
}
