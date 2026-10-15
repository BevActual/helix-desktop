//! Byte parity with `helix-config-export` for a minimal config tree (Rust exporter target).

use std::fs;
use std::path::PathBuf;
use std::process::Command;

use helix_shell_core::desktop::config_export::configure_export_env;
use helix_shell_core::desktop::settings::ConfigPaths;
fn export_bin() -> Option<PathBuf> {
    if let Ok(path) = std::env::var("HELIX_CONFIG_EXPORT_BIN") {
        let candidate = PathBuf::from(path);
        if candidate.is_file() {
            return Some(candidate);
        }
    }
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    for sub in ["../../build-debug", "../../build-fast", "../../_build"] {
        let candidate = root.join(sub).join("helix-config-export");
        if candidate.is_file() {
            return Some(candidate);
        }
    }
    None
}

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
    let Some(bin) = export_bin() else {
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
