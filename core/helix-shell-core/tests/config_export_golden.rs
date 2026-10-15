//! Byte parity with `helix-config-export` for a minimal config tree (Rust exporter target).

use std::fs;
use std::path::PathBuf;
use std::process::Command;

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
fn matches_helix_config_export_golden() {
    let Some(bin) = discover_config_export_bin() else {
        eprintln!("skip matches_helix_config_export_golden: helix-config-export not built");
        return;
    };
    let golden = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests/golden/config_export_minimal.toml");
    let expected = fs::read_to_string(&golden).expect("golden");

    let root = std::env::temp_dir().join(format!(
        "helix-export-golden-{}",
        std::process::id()
    ));
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
fn rust_export_matches_cpp_for_minimal_fixture() {
    let root = std::env::temp_dir().join(format!("helix-rust-export-{}", std::process::id()));
    let _ = fs::remove_dir_all(&root);
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
