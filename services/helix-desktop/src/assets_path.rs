//! Resolve shipped `assets/` (C++ `paths::assetPath`).

use std::env;
use std::path::{Path, PathBuf};

/// Directory containing `templates/`, `translations/`, etc.
pub fn helix_assets_dir() -> Option<PathBuf> {
    if let Ok(dir) = env::var("HELIX_ASSETS_DIR") {
        if !dir.is_empty() {
            let path = PathBuf::from(dir);
            if path.join("templates").join("builtin.toml").is_file() {
                return Some(path);
            }
        }
    }
    if let Ok(exe) = env::current_exe() {
        for ancestor in exe.ancestors() {
            let candidate = ancestor.join("assets");
            if candidate.join("templates").join("builtin.toml").is_file() {
                return Some(candidate);
            }
            let share = ancestor.join("share").join("helix");
            if share.join("templates").join("builtin.toml").is_file() {
                return Some(share);
            }
        }
    }
    None
}

pub fn builtin_templates_root(assets: &Path) -> PathBuf {
    assets.join("templates")
}
