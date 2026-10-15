//! Replays the scenarios recorded from the C++ `helix-theme-palette` helper
//! (`tools/gen_golden_custom.sh`) and compares every written palette byte for byte.

use std::fs;
use std::path::{Path, PathBuf};

use helix_theme::custom::{
    save_current_as_custom, save_role_color_as_custom, ColorRole, PaletteSource, SaveError, ThemeSelection,
};
use helix_theme::Color;

fn config_value(config: &str, key: &str) -> Option<String> {
    config.lines().find_map(|l| {
        let (k, v) = l.split_once('=')?;
        (k.trim() == key).then(|| v.trim().trim_matches('"').to_string())
    })
}

fn temp_dir(tag: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("helix-theme-golden-{}-{tag}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    dir
}

fn run_scenario(root: &Path) {
    let scenario = root.file_name().unwrap().to_string_lossy().to_string();
    let config = fs::read_to_string(root.join("config.toml")).unwrap();
    let palettes = temp_dir(&scenario);
    if let Ok(pre) = fs::read_dir(root.join("pre")) {
        for entry in pre.flatten() {
            fs::copy(entry.path(), palettes.join(entry.file_name())).unwrap();
        }
    }

    let mut sel = ThemeSelection {
        source: match config_value(&config, "source").as_deref() {
            Some("builtin") | None => PaletteSource::Builtin,
            Some("custom") => PaletteSource::Custom,
            Some(other) => panic!("unsupported source {other}"),
        },
        builtin: config_value(&config, "builtin").unwrap_or_else(|| "Helix".into()),
        custom_palette: config_value(&config, "custom_palette").unwrap_or_default(),
        community_cache: None,
        shell_light: config_value(&config, "mode").as_deref() == Some("light"),
        wallpaper: None,
    };

    let steps = fs::read_to_string(root.join("steps")).unwrap();
    let expected = fs::read_to_string(root.join("expected_steps")).unwrap();
    let mut last_name = String::new();
    for (i, (step, want)) in steps.lines().zip(expected.lines()).enumerate() {
        let parts: Vec<&str> = step.split_whitespace().collect();
        let result = match parts.as_slice() {
            ["set-accent", hex] => {
                save_role_color_as_custom(&palettes, &sel, ColorRole::Primary, Color::from_hex(hex).unwrap())
            }
            ["set-role", idx, hex] => {
                let role = ColorRole::from_index(idx.parse().unwrap()).unwrap();
                save_role_color_as_custom(&palettes, &sel, role, Color::from_hex(hex).unwrap())
            }
            ["save-palette", "current"] => save_current_as_custom(&palettes, &sel),
            other => panic!("unknown step {other:?}"),
        };
        let (code, name) = want.split_once(' ').unwrap_or((want, ""));
        match result {
            Ok(got) => {
                assert_eq!(code, "0", "{scenario} step {i} ({step}) succeeded but C++ exited {code}");
                assert_eq!(got, name, "{scenario} step {i} ({step}) palette name");
                sel.source = PaletteSource::Custom;
                sel.custom_palette = got.clone();
                last_name = got;
            }
            Err(SaveError::Message(_)) => {
                assert_eq!(code, "2", "{scenario} step {i} ({step}) failed but C++ exited {code}");
            }
            Err(SaveError::WallpaperSource) => panic!("{scenario}: wallpaper not expected"),
        }
    }

    let settings = fs::read_to_string(root.join("expected_settings.toml")).unwrap_or_default();
    if !last_name.is_empty() {
        assert_eq!(config_value(&settings, "custom_palette").as_deref(), Some(last_name.as_str()), "{scenario}");
    }

    let mut want_files: Vec<_> = fs::read_dir(root.join("expected"))
        .unwrap()
        .flatten()
        .map(|e| e.file_name().to_string_lossy().to_string())
        .collect();
    want_files.sort();
    let mut got_files: Vec<_> = fs::read_dir(&palettes)
        .unwrap()
        .flatten()
        .map(|e| e.file_name().to_string_lossy().to_string())
        .collect();
    got_files.sort();
    assert_eq!(got_files, want_files, "{scenario} palette files");
    for file in &want_files {
        let want = fs::read_to_string(root.join("expected").join(file)).unwrap();
        let got = fs::read_to_string(palettes.join(file)).unwrap();
        assert_eq!(got, want, "{scenario}/{file}");
    }
    let _ = fs::remove_dir_all(&palettes);
}

#[test]
fn matches_cpp_helper_scenarios() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/golden/custom");
    let mut count = 0;
    for entry in fs::read_dir(root).unwrap().flatten() {
        run_scenario(&entry.path());
        count += 1;
    }
    assert_eq!(count, 9);
}
