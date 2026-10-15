//! Shared test fixtures (`helix-desktop` unit tests only).

use std::sync::Mutex;

/// Serializes tests that mutate process environment variables (`XDG_*`, `HELIX_*`).
pub static ENV_LOCK: Mutex<()> = Mutex::new(());

#[cfg(test)]
pub mod fixtures {
    use std::fs;
    use std::path::Path;
    use std::time::{SystemTime, UNIX_EPOCH};

    pub fn copy_tree(from: &Path, to: &Path) {
        for entry in fs::read_dir(from).unwrap().flatten() {
            let dest = to.join(entry.file_name());
            if entry.file_type().unwrap().is_dir() {
                fs::create_dir_all(&dest).unwrap();
                copy_tree(&entry.path(), &dest);
            } else {
                if let Some(parent) = dest.parent() {
                    fs::create_dir_all(parent).unwrap();
                }
                fs::copy(entry.path(), dest).unwrap();
            }
        }
    }

    pub fn temp_home(tag: &str) -> std::path::PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let home = std::env::temp_dir().join(format!("helix-desktop-{tag}-{nanos}"));
        let _ = fs::remove_dir_all(&home);
        for sub in ["cfg/helix", "state/helix", "data", "cache"] {
            fs::create_dir_all(home.join(sub)).unwrap();
        }
        home
    }

    /// Point the config/state/assets env at `home` (hold [`super::ENV_LOCK`] while this is in effect).
    pub fn apply_test_env(home: &Path, assets: &Path) {
        unsafe {
            std::env::remove_var("HELIX_CONFIG_HOME");
            std::env::remove_var("HELIX_STATE_HOME");
            std::env::remove_var("HELIX_TEMPLATE_APPLY_CMD");
            std::env::set_var("HOME", home);
            std::env::set_var("XDG_CONFIG_HOME", home.join("cfg"));
            std::env::set_var("XDG_STATE_HOME", home.join("state"));
            std::env::set_var("XDG_DATA_HOME", home.join("data"));
            std::env::set_var("XDG_CACHE_HOME", home.join("cache"));
            std::env::set_var("HELIX_ASSETS_DIR", assets);
        }
    }

    pub fn repo_root() -> std::path::PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
    }
}
