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
    configure_export_env, discover_config_export_bin, discover_rust_config_export_bin,
    export_effective_toml, rust_export_toml,
};
use helix_shell_core::desktop::settings::ConfigPaths;
use helix_shell_core::ConfigStore;
use toml::Value as Toml;

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
fn rust_export_cli_matches_in_process_for_minimal_fixture() {
    let Some(bin) = discover_rust_config_export_bin() else {
        eprintln!("skip rust_export_cli_matches_in_process_for_minimal_fixture: build helix-rust-config-export");
        return;
    };
    let root = unique_temp_dir("rust-cli");
    let _ = fs::remove_dir_all(&root);
    let paths = minimal_fixture(&root);
    let in_process = rust_export_toml(&paths).expect("rust export");

    std::env::set_var("XDG_CONFIG_HOME", root.join("cfg"));
    std::env::set_var("XDG_STATE_HOME", root.join("state"));
    let mut command = Command::new(&bin);
    configure_export_env(&mut command, &paths);
    let output = command.output().expect("export cli");
    assert!(
        output.status.success(),
        "cli export failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    let cli: Toml = String::from_utf8(output.stdout)
        .expect("utf8")
        .parse()
        .expect("cli parse");
    assert_eq!(cli, in_process);
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
    let expected_value: Toml = expected.parse().expect("golden parse");
    let got_value: Toml = got.parse().expect("export parse");
    assert_eq!(
        got_value,
        expected_value,
        "config_export output drifted; rerun tools/gen_config_export_golden.sh"
    );
    let _ = fs::remove_dir_all(root);
}

#[test]
fn rust_export_matches_cpp_for_light_theme() {
    assert_rust_export_matches_fixture_config("light_theme", "[theme]\nmode = \"light\"\n");
}

fn export_fixture_path(fixture_id: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests/golden/export_fixtures")
        .join(format!("{fixture_id}.toml"))
}

fn load_export_fixture(fixture_id: &str) -> toml::Value {
    let text = fs::read_to_string(export_fixture_path(fixture_id)).unwrap_or_else(|err| {
        panic!(
            "missing export fixture {fixture_id}: {err}; run tools/gen_config_export_fixtures.py"
        )
    });
    text.parse().expect("fixture parse")
}

fn assert_rust_export_matches_fixture(
    fixture_id: &str,
    config_toml: &str,
    settings_toml: &str,
    extra_config_files: &[(&str, &str)],
) {
    let root = unique_temp_dir("parity");
    let config_dir = root.join("cfg").join("helix");
    let state_dir = root.join("state").join("helix");
    fs::create_dir_all(&config_dir).unwrap();
    fs::create_dir_all(&state_dir).unwrap();
    fs::write(config_dir.join("config.toml"), config_toml).unwrap();
    fs::write(state_dir.join("settings.toml"), settings_toml).unwrap();
    for (name, body) in extra_config_files {
        fs::write(config_dir.join(name), body).unwrap();
    }
    let paths = ConfigPaths {
        config_dir,
        settings_toml: state_dir.join("settings.toml"),
    };
    let store = ConfigStore::load(paths.clone());
    let rust = rust_export_toml(store.paths()).expect("rust export");
    let expected = load_export_fixture(fixture_id);
    assert_eq!(
        rust,
        expected,
        "fixture {fixture_id}\nconfig:\n{config_toml}\nsettings:\n{settings_toml}"
    );
    if let Some(bin) = discover_config_export_bin() {
        let cpp =
            helix_shell_core::desktop::config_export::cpp_export_toml(&paths, &bin).expect("cpp");
        assert_eq!(
            cpp,
            expected,
            "fixture {fixture_id} stale vs helix-config-export; rerun tools/gen_config_export_fixtures.py"
        );
    }
    let _ = fs::remove_dir_all(root);
}

fn assert_rust_export_matches_fixture_config(fixture_id: &str, config_toml: &str) {
    assert_rust_export_matches_fixture(fixture_id, config_toml, "\n", &[]);
}

#[test]
fn rust_export_matches_cpp_for_bar_thickness_override() {
    assert_rust_export_matches_fixture_config("bar_thickness_override", "[bar.default]\nthickness = 40\n");
}

#[test]
fn rust_export_matches_cpp_for_legacy_negative_bar_radius() {
    assert_rust_export_matches_fixture_config("legacy_negative_bar_radius", "[bar.default]\nradius = -16\n");
}

#[test]
fn rust_export_matches_cpp_for_legacy_sysmon_show_icon() {
    assert_rust_export_matches_fixture_config("legacy_sysmon_show_icon", "[widget.cpu]\nshow_icon = false\n");
}

#[test]
fn rust_export_matches_cpp_for_legacy_workspaces_widget() {
    assert_rust_export_matches_fixture_config("legacy_workspaces_widget", "[widget.workspaces]\ndisplay = \"none\"\n");
}

#[test]
fn rust_export_matches_cpp_for_custom_named_sysmon_widget() {
    assert_rust_export_matches_fixture_config("custom_named_sysmon_widget", "[widget.my-cpu]\ntype = \"sysmon\"\nstat = \"cpu_usage\"\n");
}

#[test]
fn rust_export_matches_cpp_for_legacy_location_schedule() {
    assert_rust_export_matches_fixture_config("legacy_location_schedule", 
        "[location]\nsunset = \"18:00\"\nsunrise = \"06:00\"\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_plugins_auto_update_bool() {
    assert_rust_export_matches_fixture_config("plugins_auto_update_bool", "[plugins]\nauto_update = false\n");
}

#[test]
fn rust_export_matches_cpp_for_theme_builtin_override() {
    assert_rust_export_matches_fixture_config("theme_builtin_override", "[theme]\nbuiltin = \"Qt\"\n");
}

#[test]
fn rust_export_matches_cpp_with_wallpaper_runtime_state() {
    assert_rust_export_matches_fixture_config("wallpaper_runtime_state", "[wallpaper]\ndefault = \"/tmp/w.png\"\n");
}

#[test]
fn rust_export_matches_cpp_for_calendar_account() {
    assert_rust_export_matches_fixture_config("calendar_account", 
        "[calendar.account.work]\ntype = \"ics\"\nserver_url = \"https://caldav.example.com\"\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_bar_monitor_override() {
    assert_rust_export_matches_fixture_config("bar_monitor_override", "[bar.default.monitor.DP-1]\nthickness = 28\n");
}

#[test]
fn rust_export_matches_cpp_for_legacy_bar_dead_zone_command() {
    assert_rust_export_matches_fixture_config("legacy_bar_dead_zone_command", "[bar.default.dead_zone]\ncommand = \"notify-send hi\"\n");
}

#[test]
fn rust_export_matches_cpp_for_desktop_widgets_disabled() {
    assert_rust_export_matches_fixture_config("desktop_widgets_disabled", "[desktop_widgets]\nenabled = false\n");
}

#[test]
fn rust_export_matches_cpp_for_desktop_widget_placement() {
    assert_rust_export_matches_fixture_config("desktop_widget_placement", 
        "[desktop_widgets]\nwidget_order = [\"clock_main\"]\n\n[desktop_widgets.widget.clock_main]\ntype = \"clock\"\ncx = 960.0\ncy = 540.0\n\n[desktop_widgets.widget.clock_main.settings]\nformat = \"{:%H:%M}\"\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_lockscreen_widget_placement() {
    assert_rust_export_matches_fixture_config("lockscreen_widget_placement", 
        "[lockscreen_widgets]\nwidget_order = [\"clock_main\"]\n\n[lockscreen_widgets.widget.clock_main]\ntype = \"clock\"\ncx = 100.0\ncy = 200.0\n\n[lockscreen_widgets.widget.clock_main.settings]\nformat = \"{:%H:%M}\"\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_bar_monitor_match_override() {
    assert_rust_export_matches_fixture_config("bar_monitor_match_override", 
        "[bar.default.monitor.DP-1]\nmatch = \"DP-1\"\nthickness = 28\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_two_desktop_widget_placements() {
    assert_rust_export_matches_fixture_config("two_desktop_widget_placements", 
        "[desktop_widgets]\nwidget_order = [\"a\", \"b\"]\n\n[desktop_widgets.widget.a]\ntype = \"clock\"\n\n[desktop_widgets.widget.b]\ntype = \"date\"\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_idle_lock_behavior_only() {
    assert_rust_export_matches_fixture_config("idle_lock_behavior_only", 
        "[idle.behavior.lock]\ntimeout = 300\nenabled = true\naction = \"lock\"\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_nightlight_enabled() {
    assert_rust_export_matches_fixture_config("nightlight_enabled", "[nightlight]\nenabled = true\n");
}

#[test]
fn rust_export_matches_cpp_for_two_calendar_accounts() {
    assert_rust_export_matches_fixture_config("two_calendar_accounts", 
        "[calendar.account.zed]\ntype = \"ics\"\nserver_url = \"https://a\"\n\n[calendar.account.alpha]\ntype = \"ics\"\nserver_url = \"https://b\"\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_placement_widget_table_sort_order() {
    assert_rust_export_matches_fixture_config("placement_widget_table_sort_order", 
        "[desktop_widgets]\nwidget_order = [\"b\", \"a\"]\n\n[desktop_widgets.widget.b]\ntype = \"clock\"\n\n[desktop_widgets.widget.a]\ntype = \"date\"\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_idle_behavior_order_override() {
    assert_rust_export_matches_fixture_config("idle_behavior_order_override", 
        "[idle]\nbehavior_order = [\"screen-off\", \"lock\"]\n\n[idle.behavior.screen-off]\ntimeout = 200\nenabled = true\naction = \"screen_off\"\n\n[idle.behavior.lock]\ntimeout = 100\nenabled = true\naction = \"lock\"\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_desktop_widget_flip_x() {
    assert_rust_export_matches_fixture_config("desktop_widget_flip_x", 
        "[desktop_widgets]\nwidget_order = [\"clock_main\"]\n\n[desktop_widgets.widget.clock_main]\ntype = \"clock\"\nflip_x = true\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_bar_thickness_float_literal() {
    assert_rust_export_matches_fixture_config("bar_thickness_float_literal", "[bar.default]\nthickness = 40.0\n");
}

#[test]
fn rust_export_matches_cpp_for_theme_pure_black_dark() {
    assert_rust_export_matches_fixture_config("theme_pure_black_dark", "[theme]\npure_black_dark = true\n");
}

#[test]
fn rust_export_matches_cpp_for_dock_disabled() {
    assert_rust_export_matches_fixture_config("dock_disabled", "[dock]\nenabled = false\n");
}

#[test]
fn rust_export_matches_cpp_for_settings_theme_override() {
    assert_rust_export_matches_fixture(
        "settings_theme_override",
        "",
        "[theme]\nmode = \"light\"\n",
        &[],
    );
}

#[test]
fn rust_export_matches_cpp_for_migrated_settings_sidecar() {
    assert_rust_export_matches_fixture(
        "migrated_settings_sidecar",
        "",
        "config_version = 14\n[shell]\nlaunch_apps_as_systemd_services = false\n",
        &[],
    );
}

#[test]
fn rust_export_matches_cpp_for_notification_max_visible() {
    assert_rust_export_matches_fixture_config("notification_max_visible", "[notification]\nmax_visible = 5\n");
}

#[test]
fn rust_export_matches_cpp_for_shell_polkit_agent() {
    assert_rust_export_matches_fixture_config("shell_polkit_agent", "[shell]\npolkit_agent = true\n");
}

#[test]
fn rust_export_matches_cpp_for_bar_start_widgets() {
    assert_rust_export_matches_fixture_config("bar_start_widgets", "[bar.default]\nstart = [\"clock\", \"date\"]\n");
}

#[test]
fn rust_export_matches_cpp_with_config_include() {
    assert_rust_export_matches_fixture(
        "config_include",
        "include = { files = [\"base.toml\"] }\n\n[theme]\nmode = \"light\"\n",
        "\n",
        &[("base.toml", "[theme]\nmode = \"dark\"\n")],
    );
}

#[test]
fn rust_export_matches_cpp_for_secondary_bar() {
    assert_rust_export_matches_fixture_config("secondary_bar", "[bar.secondary]\nenabled = true\nthickness = 28\n");
}

#[test]
fn rust_export_matches_cpp_for_default_and_secondary_bars() {
    assert_rust_export_matches_fixture_config("default_and_secondary_bars", 
        "[bar.default]\nthickness = 40\n\n[bar.secondary]\nenabled = true\nthickness = 28\n",
    );
}

/// `tools/nested/helix-desktop-config-smoke.sh` config tree (bar name + idle + templates).
#[test]
fn rust_export_matches_cpp_for_explicit_bar_order() {
    assert_rust_export_matches_fixture_config("explicit_bar_order", 
        "[bar]\norder = [\"zed\", \"alpha\"]\n\n[bar.zed]\nenabled = true\n\n[bar.alpha]\nenabled = true\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_desktop_config_smoke() {
    assert_rust_export_matches_fixture_config("desktop_config_smoke", 
        "[bar.main]\nenabled = true\nthickness = 34\n\n[idle.behavior.lock]\ntimeout = 420\nenabled = true\naction = \"lock\"\n\n[theme.templates]\nbuiltin_ids = [\"helix\", \"qt\"]\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_launcher_providers() {
    assert_rust_export_matches_fixture_config(
        "launcher_providers",
        "[shell.launcher.providers.calculator]\nprefix = \"calc\"\nglobal = true\n\n[shell.launcher.providers.emoji]\nprefix = \"emo\"\n\n[shell.launcher.providers.session]\nprefix = \"session\"\nglobal = false\n",
    );
}

#[test]
fn rust_export_matches_cpp_for_shell_panel_glass() {
    assert_rust_export_matches_fixture_config("shell_panel_glass", "[shell.panel]\nglass = 0.25\n");
}

#[test]
fn rust_export_matches_cpp_for_hooks_empty_section() {
    assert_rust_export_matches_fixture_config("hooks_empty_section", "[hooks]\n");
}

#[test]
fn rust_export_matches_cpp_for_example_toml() {
    let example = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../example.toml");
    if !example.is_file() {
        return;
    }
    let root = unique_temp_dir("example");
    let config_dir = root.join("cfg").join("helix");
    let state_dir = root.join("state").join("helix");
    fs::create_dir_all(&config_dir).unwrap();
    fs::create_dir_all(&state_dir).unwrap();
    fs::copy(&example, config_dir.join("config.toml")).unwrap();
    fs::write(state_dir.join("settings.toml"), "\n").unwrap();
    let paths = ConfigPaths {
        config_dir,
        settings_toml: state_dir.join("settings.toml"),
    };
    let rust = rust_export_toml(&paths).expect("example.toml rust export");
    let Some(bin) = discover_config_export_bin() else {
        eprintln!("skip rust_export_matches_cpp_for_example_toml: helix-config-export not built");
        let _ = fs::remove_dir_all(root);
        return;
    };
    let cpp = helix_shell_core::desktop::config_export::cpp_export_toml(&paths, &bin)
        .expect("cpp export");
    assert_eq!(
        rust,
        cpp,
        "example.toml rust/cpp export drift; fix patch rules or regenerate defaults"
    );
    let _ = fs::remove_dir_all(root);
}

#[test]
fn rust_export_matches_cpp_for_minimal_fixture() {
    let root = unique_temp_dir("minimal");
    let paths = minimal_fixture(&root);
    let store = ConfigStore::load(paths.clone());
    let rust = export_effective_toml(&store, None).expect("rust export");
    let expected = load_export_fixture("minimal_fixture");
    assert_eq!(rust, expected);
    let _ = fs::remove_dir_all(root);
}
