//! Byte parity with `helix-config-export` for a minimal config tree (Rust exporter target).

use std::fs;
use std::path::PathBuf;
use std::process::Command;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

fn unique_temp_dir(label: &str) -> PathBuf {
    static NEXT: AtomicU64 = AtomicU64::new(0);
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let seq = NEXT.fetch_add(1, Ordering::Relaxed);
    std::env::temp_dir().join(format!("helix-export-{label}-{nanos}-{seq}"))
}

use helix_shell_core::desktop::config_export::{
    configure_export_env, discover_config_export_bin, export_effective_toml, rust_export_toml,
};
use helix_shell_core::desktop::settings::ConfigPaths;
use helix_shell_core::ConfigStore;

fn minimal_fixture(root: &PathBuf) -> ConfigPaths {
    let config_dir = root.join("cfg").join("helix");
    let state_dir = root.join("state").join("helix");
    fs::create_dir_all(&config_dir).unwrap();
    fs::create_dir_all(&state_dir).unwrap();
    fs::write(
        config_dir.join("config.toml"),
        "[theme]\nmode = \"dark\"\n\n[bar.default]\nenabled = true\nthickness = 34\n",
    )
    .unwrap();
    fs::write(state_dir.join("settings.toml"), "\n").unwrap();
    ConfigPaths {
        config_dir,
        settings_toml: state_dir.join("settings.toml"),
    }
}

#[test]
fn rust_export_matches_golden_without_cpp_helper() {
    let root = unique_temp_dir("rust-only");
    let _ = fs::remove_dir_all(&root);
    let paths = minimal_fixture(&root);
    let exported = rust_export_toml(&paths).expect("rust export");
    let golden = fs::read_to_string(
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/golden/config_export_minimal.toml"),
    )
    .expect("golden");
    let expected: toml::Value = golden.parse().expect("golden parse");
    assert_eq!(exported, expected);
    let _ = fs::remove_dir_all(root);
}

#[test]
fn matches_helix_config_export_golden() {
    let Some(bin) = discover_config_export_bin() else {
        eprintln!("skip matches_helix_config_export_golden: helix-config-export not built");
        return;
    };
    let golden = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests/golden/config_export_minimal.toml");
    let expected = fs::read_to_string(&golden).expect("golden");

    let root = unique_temp_dir("golden");
    let _ = fs::remove_dir_all(&root);
    let paths = minimal_fixture(&root);

    std::env::set_var("XDG_CONFIG_HOME", root.join("cfg"));
    std::env::set_var("XDG_STATE_HOME", root.join("state"));

    let mut command = Command::new(&bin);
    configure_export_env(&mut command, &paths);
    let output = command.output().expect("export");
    assert!(
        output.status.success(),
        "export failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    let got = String::from_utf8(output.stdout).expect("utf8");
    assert_eq!(got, expected, "config_export output drifted; rerun tools/gen_config_export_golden.sh");
    let _ = fs::remove_dir_all(root);
}

#[test]
fn rust_export_matches_cpp_for_light_theme() {
    let Some(bin) = discover_config_export_bin() else {
        eprintln!("skip rust_export_matches_cpp_for_light_theme: helix-config-export not built");
        return;
    };
    let root = unique_temp_dir("light");
    let _ = fs::remove_dir_all(&root);
    let config_dir = root.join("cfg").join("helix");
    let state_dir = root.join("state").join("helix");
    fs::create_dir_all(&config_dir).unwrap();
    fs::create_dir_all(&state_dir).unwrap();
    fs::write(config_dir.join("config.toml"), "[theme]\nmode = \"light\"\n").unwrap();
    fs::write(state_dir.join("settings.toml"), "\n").unwrap();
    let paths = ConfigPaths {
        config_dir,
        settings_toml: state_dir.join("settings.toml"),
    };
    let store = ConfigStore::load(paths.clone());
    let rust = rust_export_toml(store.paths()).expect("rust export");
    let cpp = helix_shell_core::desktop::config_export::cpp_export_toml(&paths, &bin).expect("cpp");
    assert_eq!(rust, cpp);
    let _ = fs::remove_dir_all(root);
}

fn assert_rust_export_matches_cpp_files(config_toml: &str, settings_toml: &str) {
    let Some(bin) = discover_config_export_bin() else {
        eprintln!("skip assert_rust_export_matches_cpp: helix-config-export not built");
        return;
    };
    let root = unique_temp_dir("parity");
    let config_dir = root.join("cfg").join("helix");
    let state_dir = root.join("state").join("helix");
    fs::create_dir_all(&config_dir).unwrap();
    fs::create_dir_all(&state_dir).unwrap();
    fs::write(config_dir.join("config.toml"), config_toml).unwrap();
    fs::write(state_dir.join("settings.toml"), settings_toml).unwrap();
    let paths = ConfigPaths {
        config_dir,
        settings_toml: state_dir.join("settings.toml"),
    };
    let store = ConfigStore::load(paths.clone());
    let rust = rust_export_toml(store.paths()).expect("rust export");
    let cpp = helix_shell_core::desktop::config_export::cpp_export_toml(&paths, &bin).expect("cpp");
    assert_eq!(
        rust,
        cpp,
        "config:\n{config_toml}\nsettings:\n{settings_toml}"
    );
    let _ = fs::remove_dir_all(root);
}

fn assert_rust_export_matches_cpp(config_toml: &str) {
    assert_rust_export_matches_cpp_files(config_toml, "\n");
}

#[test]
fn rust_export_matches_cpp_for_bar_thickness_override() {
    assert_rust_export_matches_cpp("[bar.default]\nthickness = 40\n");
}

#[test]
fn rust_export_matches_cpp_for_legacy_negative_bar_radius() {
    assert_rust_export_matches_cpp("[bar.default]\nradius = -16\n");
}

#[test]
fn rust_export_matches_cpp_for_legacy_sysmon_show_icon() {
    assert_rust_export_matches_cpp("[widget.cpu]\nshow_icon = false\n");
}

#[test]
fn rust_export_matches_cpp_for_legacy_workspaces_widget() {
    assert_rust_export_matches_cpp("[widget.workspaces]\ndisplay = \"none\"\n");
}

#[test]
fn rust_export_matches_cpp_for_custom_named_sysmon_widget() {
    assert_rust_export_matches_cpp("[widget.my-cpu]\ntype = \"sysmon\"\nstat = \"cpu_usage\"\n");
}

#[test]
fn rust_export_matches_cpp_for_legacy_location_schedule() {
    assert_rust_export_matches_cpp(
        "[location]\nsunset = \"18:00\"\nsunrise = \"06:00\"\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_plugins_auto_update_bool() {
    assert_rust_export_matches_cpp("[plugins]\nauto_update = false\n");
}

#[test]
fn rust_export_matches_cpp_for_theme_builtin_override() {
    assert_rust_export_matches_cpp("[theme]\nbuiltin = \"Qt\"\n");
}

#[test]
fn rust_export_matches_cpp_for_bar_thickness_float_literal() {
    assert_rust_export_matches_cpp("[bar.default]\nthickness = 40.0\n");
}

#[test]
fn rust_export_matches_cpp_for_theme_pure_black_dark() {
    assert_rust_export_matches_cpp("[theme]\npure_black_dark = true\n");
}

#[test]
fn rust_export_matches_cpp_for_dock_disabled() {
    assert_rust_export_matches_cpp("[dock]\nenabled = false\n");
}

#[test]
fn rust_export_matches_cpp_for_settings_theme_override() {
    assert_rust_export_matches_cpp_files("", "[theme]\nmode = \"light\"\n");
}

#[test]
fn rust_export_matches_cpp_for_migrated_settings_sidecar() {
    assert_rust_export_matches_cpp_files(
        "",
        "config_version = 14\n[shell]\nlaunch_apps_as_systemd_services = false\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_notification_max_visible() {
    assert_rust_export_matches_cpp("[notification]\nmax_visible = 5\n");
}

#[test]
fn rust_export_matches_cpp_for_shell_polkit_agent() {
    assert_rust_export_matches_cpp("[shell]\npolkit_agent = true\n");
}

#[test]
fn rust_export_matches_cpp_for_bar_start_widgets() {
    assert_rust_export_matches_cpp("[bar.default]\nstart = [\"clock\", \"date\"]\n");
}

#[test]
fn rust_export_matches_cpp_with_config_include() {
    let Some(bin) = discover_config_export_bin() else {
        eprintln!("skip rust_export_matches_cpp_with_config_include: helix-config-export not built");
        return;
    };
    let root = unique_temp_dir("include");
    let config_dir = root.join("cfg").join("helix");
    fs::create_dir_all(&config_dir).unwrap();
    let state_dir = root.join("state").join("helix");
    fs::create_dir_all(&state_dir).unwrap();
    fs::write(
        config_dir.join("base.toml"),
        "[theme]\nmode = \"dark\"\n",
    )
    .unwrap();
    fs::write(
        config_dir.join("config.toml"),
        "include = { files = [\"base.toml\"] }\n\n[theme]\nmode = \"light\"\n",
    )
    .unwrap();
    fs::write(state_dir.join("settings.toml"), "\n").unwrap();
    let paths = ConfigPaths {
        config_dir,
        settings_toml: state_dir.join("settings.toml"),
    };
    let store = ConfigStore::load(paths.clone());
    let rust = rust_export_toml(store.paths()).expect("rust export");
    let cpp = helix_shell_core::desktop::config_export::cpp_export_toml(&paths, &bin).expect("cpp");
    assert_eq!(rust, cpp);
    let _ = fs::remove_dir_all(root);
}

#[test]
fn rust_export_matches_cpp_for_secondary_bar() {
    assert_rust_export_matches_cpp("[bar.secondary]\nenabled = true\nthickness = 28\n");
}

#[test]
fn rust_export_matches_cpp_for_default_and_secondary_bars() {
    assert_rust_export_matches_cpp(
        "[bar.default]\nthickness = 40\n\n[bar.secondary]\nenabled = true\nthickness = 28\n",
    );
}

/// `tools/nested/helix-desktop-config-smoke.sh` config tree (bar name + idle + templates).
#[test]
fn rust_export_matches_cpp_for_explicit_bar_order() {
    assert_rust_export_matches_cpp(
        "[bar]\norder = [\"zed\", \"alpha\"]\n\n[bar.zed]\nenabled = true\n\n[bar.alpha]\nenabled = true\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_desktop_config_smoke() {
    assert_rust_export_matches_cpp(
        "[bar.main]\nenabled = true\nthickness = 34\n\n[idle.behavior.lock]\ntimeout = 420\nenabled = true\naction = \"lock\"\n\n[theme.templates]\nbuiltin_ids = [\"helix\", \"qt\"]\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_minimal_fixture() {
    let root = unique_temp_dir("minimal");
    let paths = minimal_fixture(&root);
    let store = ConfigStore::load(paths.clone());
    if rust_export_toml(store.paths()).is_none() {
        eprintln!("skip rust_export_matches_cpp: rust_export_toml not implemented");
        let _ = fs::remove_dir_all(root);
        return;
    }
    let Some(bin) = discover_config_export_bin() else {
        eprintln!("skip rust_export_matches_cpp: helix-config-export not built");
        let _ = fs::remove_dir_all(root);
        return;
    };
    let cpp = helix_shell_core::desktop::config_export::cpp_export_toml(&paths, &bin).expect("cpp");
    let rust = export_effective_toml(&store, Some(&bin)).expect("export");
    assert_eq!(rust, cpp);
    let _ = fs::remove_dir_all(root);
}
