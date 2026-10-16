//! End-to-end parity with the C++ `helix-template-apply`: same inputs, same file tree,
//! hook markers, failures and persisted applied-id state. Regenerate with
//! `tools/gen_golden_apply.sh`.

use std::fs;
use std::path::{Path, PathBuf};

use helix_theme::apply::{
    apply, build_user_template_root, ApplyInputs, CommunityTemplate, Outcome,
};
use helix_theme::custom::{
    community_palette_cache_path, resolve_palette, PaletteSource, ThemeSelection, WallpaperPalette,
};
use helix_theme::template::make_theme_data;
use helix_theme::transform::apply_resolved_transforms;
use helix_theme::wallpaper::{generate, load_and_resize, Scheme};
use toml::{Table, Value};

fn copy_tree(from: &Path, to: &Path) {
    for entry in fs::read_dir(from).unwrap().flatten() {
        let dest = to.join(entry.file_name());
        if entry.file_type().unwrap().is_dir() {
            fs::create_dir_all(&dest).unwrap();
            copy_tree(&entry.path(), &dest);
        } else {
            fs::copy(entry.path(), dest).unwrap();
        }
    }
}

fn collect(root: &Path, dir: &Path, out: &mut Vec<PathBuf>) {
    for entry in fs::read_dir(dir).unwrap().flatten() {
        let path = entry.path();
        if entry.file_type().unwrap().is_dir() {
            collect(root, &path, out);
        } else {
            out.push(path.strip_prefix(root).unwrap().to_path_buf());
        }
    }
}

fn strings(table: &Table, key: &str) -> Vec<String> {
    table
        .get(key)
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(Value::as_str)
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default()
}

fn run_scenario(scenario: &Path, root: &Path) {
    let name = scenario.file_name().unwrap().to_string_lossy().to_string();
    let _ = fs::remove_dir_all(root);
    for d in ["cfg/helix", "state/helix", "data", "cache"] {
        fs::create_dir_all(root.join(d)).unwrap();
    }
    fs::copy(
        scenario.join("config.toml"),
        root.join("cfg/helix/config.toml"),
    )
    .unwrap();
    if scenario.join("files").is_dir() {
        copy_tree(&scenario.join("files"), &root.join("cfg"));
    }
    if scenario.join("statefiles").is_dir() {
        copy_tree(&scenario.join("statefiles"), &root.join("state/helix"));
    }
    if scenario.join("state.toml").is_file() {
        fs::copy(
            scenario.join("state.toml"),
            root.join("state/helix/state.toml"),
        )
        .unwrap();
    }

    std::env::set_var("HOME", root);
    std::env::set_var("XDG_CONFIG_HOME", root.join("cfg"));
    std::env::set_var("XDG_STATE_HOME", root.join("state"));
    std::env::set_var("XDG_DATA_HOME", root.join("data"));
    std::env::set_var("XDG_CACHE_HOME", root.join("cache"));

    let config: Table = fs::read_to_string(root.join("cfg/helix/config.toml"))
        .unwrap()
        .parse()
        .unwrap();
    let theme = config
        .get("theme")
        .and_then(Value::as_table)
        .cloned()
        .unwrap_or_default();
    let templates = theme
        .get("templates")
        .and_then(Value::as_table)
        .cloned()
        .unwrap_or_default();
    let builtin = theme
        .get("builtin")
        .and_then(Value::as_str)
        .unwrap_or("Helix");
    let mode = theme.get("mode").and_then(Value::as_str).unwrap_or("dark");

    let source = match theme
        .get("source")
        .and_then(Value::as_str)
        .unwrap_or("builtin")
    {
        "builtin" => PaletteSource::Builtin,
        "custom" => PaletteSource::Custom,
        "community" => PaletteSource::Community,
        "wallpaper" => PaletteSource::Wallpaper,
        other => panic!("{name}: unsupported source {other}"),
    };
    // The wallpaper the palette comes from: `[wallpaper.last] path` in the state's settings.toml (a leading ~ is HOME).
    let wallpaper_path = fs::read_to_string(root.join("state/helix/settings.toml"))
        .ok()
        .and_then(|s| s.parse::<Table>().ok())
        .and_then(|t| {
            let path = t
                .get("wallpaper")?
                .get("last")?
                .get("path")?
                .as_str()?
                .to_string();
            Some(match path.strip_prefix("~/") {
                Some(rest) => root.join(rest).to_string_lossy().into_owned(),
                None => path,
            })
        })
        .unwrap_or_default();
    let wallpaper = (source == PaletteSource::Wallpaper).then(|| {
        let scheme_name = theme
            .get("wallpaper_scheme")
            .and_then(Value::as_str)
            .unwrap_or("m3-content");
        let scheme = Scheme::parse(scheme_name).unwrap_or(Scheme::Content);
        let rgb = load_and_resize(Path::new(&wallpaper_path), scheme).expect("wallpaper loads");
        WallpaperPalette {
            generated: generate(&rgb, scheme).expect("wallpaper palette"),
            path: wallpaper_path.clone(),
            scheme: scheme_name.to_string(),
        }
    });
    let community = theme
        .get("community_palette")
        .and_then(Value::as_str)
        .unwrap_or("Oxocarbon");
    let selection = ThemeSelection {
        source,
        builtin: builtin.to_string(),
        custom_palette: theme
            .get("custom_palette")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string(),
        community_cache: (source == PaletteSource::Community && !community.is_empty())
            .then(|| community_palette_cache_path(&root.join("state/helix"), community)),
        shell_light: false,
        wallpaper,
    };
    let mut generated = resolve_palette(&root.join("cfg/helix/palettes"), &selection).unwrap();
    let high_contrast = config
        .get("accessibility")
        .and_then(Value::as_table)
        .and_then(|t| t.get("high_contrast"))
        .and_then(Value::as_bool)
        .unwrap_or(false);
    apply_resolved_transforms(
        &mut generated.dark,
        &mut generated.light,
        theme
            .get("pure_black_dark")
            .and_then(Value::as_bool)
            .unwrap_or(false),
        high_contrast,
    );
    let scheme = theme
        .get("wallpaper_scheme")
        .and_then(Value::as_str)
        .unwrap_or("m3-content");
    let scheme_type = scheme.strip_prefix("m3-").unwrap_or(scheme).to_string();
    let state_path = root.join("state/helix/state.toml");
    let applied_before = fs::read_to_string(&state_path)
        .ok()
        .and_then(|s| s.parse::<Table>().ok())
        .and_then(|t| {
            t.get("theme_templates")?
                .as_table()?
                .get("applied_builtin_ids")?
                .as_str()
                .map(|s| s.split_whitespace().map(str::to_string).collect::<Vec<_>>())
        })
        .unwrap_or_default();

    let user_root = build_user_template_root(&templates);
    let user_config = root.join("cfg/helix/config.toml");
    let inputs = ApplyInputs {
        theme_data: make_theme_data(&generated.dark, &generated.light),
        default_mode: mode.to_string(),
        scheme_type,
        image_path: Some(wallpaper_path.clone()),
        config_table: None,
        icon_theme: String::new(),
        builtin_config: Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../assets/templates/builtin.toml"),
        enable_builtin: templates
            .get("enable_builtin_templates")
            .and_then(Value::as_bool)
            .unwrap_or(true),
        builtin_ids: strings(&templates, "builtin_ids"),
        enable_community: templates
            .get("enable_community_templates")
            .and_then(Value::as_bool)
            .unwrap_or(true),
        community: strings(&templates, "community_ids")
            .into_iter()
            .map(|id| CommunityTemplate {
                config_path: root.join(format!(
                    "state/helix/community-templates/{id}/template.toml"
                )),
                id,
            })
            .collect(),
        applied_before,
        notify_kde: None,
        user_root: if user_root.is_empty() {
            None
        } else {
            Some((user_root, user_config))
        },
    };

    let report = match apply(&inputs) {
        Outcome::Applied(report) => report,
        Outcome::Unsupported(why) => panic!("{name}: unexpectedly unsupported: {why}"),
    };
    // The C++ service persists the applied ids; mirror that so the trees are comparable.
    fs::write(
        &state_path,
        format!(
            "\n[theme_templates]\napplied_builtin_ids = \"{}\"",
            report.applied_builtin_ids.join(" ")
        ),
    )
    .unwrap();

    let expected = scenario.join("expected");
    let want_exit: i32 = fs::read_to_string(expected.join("exit_code"))
        .unwrap()
        .trim()
        .parse()
        .unwrap();
    assert_eq!(
        i32::from(!report.failures.is_empty()) * 2,
        want_exit,
        "{name} exit status"
    );
    let want_failures = fs::read_to_string(expected.join("failures.txt")).unwrap_or_default();
    let root_str = root.to_string_lossy().to_string();
    let mut got_lines: Vec<String> = report
        .failures
        .iter()
        .map(|(id, msg)| format!("template failure: {id}: {msg}").replace(&root_str, "@ROOT@"))
        .collect();
    got_lines.sort();
    let got_failures: String = got_lines.into_iter().map(|l| l + "\n").collect();
    assert_eq!(got_failures, want_failures, "{name} failures");

    let mut want_files = Vec::new();
    collect(
        &expected.join("tree"),
        &expected.join("tree"),
        &mut want_files,
    );
    want_files.sort();
    let mut got_files = Vec::new();
    collect(root, root, &mut got_files);
    got_files.sort();
    assert_eq!(got_files, want_files, "{name} file tree");

    for rel in &want_files {
        if rel.ends_with("settings.toml") {
            // An input here (the wallpaper path); the C++ run re-serialises it, which is not what is being compared.
            continue;
        }
        if rel.ends_with("state.toml") {
            // C++ writes its own TOML layout; compare the persisted value instead.
            let ids = |p: &Path| {
                fs::read_to_string(p).unwrap().parse::<Table>().unwrap()["theme_templates"]
                    ["applied_builtin_ids"]
                    .as_str()
                    .unwrap()
                    .to_string()
            };
            assert_eq!(
                ids(&root.join(rel)),
                ids(&expected.join("tree").join(rel)),
                "{name} applied ids"
            );
            continue;
        }
        let want = fs::read(expected.join("tree").join(rel)).unwrap();
        let mut got = fs::read(root.join(rel)).unwrap();
        // Text files carry the scratch root in them; binary ones (a copied wallpaper) are compared as they are.
        if let Ok(text) = String::from_utf8(got.clone()) {
            got = text.replace(&root_str, "@ROOT@").into_bytes();
        }
        assert!(got == want, "{name}: {} differs", rel.display());
    }
}

#[test]
fn matches_cpp_template_apply() {
    let golden = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/golden/apply");
    let root = std::env::temp_dir().join(format!("helix-theme-apply-{}", std::process::id()));
    let mut count = 0;
    let mut dirs: Vec<_> = fs::read_dir(&golden)
        .unwrap()
        .flatten()
        .map(|e| e.path())
        .collect();
    dirs.sort();
    for scenario in dirs {
        run_scenario(&scenario, &root);
        count += 1;
    }
    let _ = fs::remove_dir_all(&root);
    assert_eq!(count, 23);
}
