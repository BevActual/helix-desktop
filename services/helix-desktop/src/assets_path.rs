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

fn find_meson_helper_bin(env_key: &str, base: &str) -> Option<PathBuf> {
    if let Ok(path) = env::var(env_key) {
        if !path.is_empty() {
            let candidate = PathBuf::from(path);
            if candidate.is_file() {
                return Some(candidate);
            }
        }
    }
    if let Ok(exe) = env::current_exe() {
        for name in [base, &format!("{base}.exe")] {
            if let Some(parent) = exe.parent() {
                let sibling = parent.join(name);
                if sibling.is_file() {
                    return Some(sibling);
                }
            }
        }
        for ancestor in exe.ancestors() {
            for subdir in ["", "build-debug", "build-fast", "_build"] {
                let candidate = if subdir.is_empty() {
                    ancestor.join(base)
                } else {
                    ancestor.join(subdir).join(base)
                };
                if candidate.is_file() {
                    return Some(candidate);
                }
            }
        }
    }
    None
}

/// C++ `helix-template-apply` (TemplateEngine) helper for Rust `helix-desktop`.
pub fn helix_template_apply_bin() -> Option<PathBuf> {
    find_meson_helper_bin("HELIX_TEMPLATE_APPLY_BIN", "helix-template-apply")
}

/// C++ `helix-theme-palette` helper for Rust `Appearance1` palette mutations.
pub fn helix_theme_palette_bin() -> Option<PathBuf> {
    find_meson_helper_bin("HELIX_THEME_PALETTE_BIN", "helix-theme-palette")
}

/// C++ `config_export::serialize` helper for in-process template apply.
pub fn helix_config_export_bin() -> Option<PathBuf> {
    helix_shell_core::desktop::config_export::discover_config_export_bin()
}
