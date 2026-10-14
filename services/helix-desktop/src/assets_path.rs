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

/// C++ `helix-template-apply` (TemplateEngine) helper for Rust `helix-desktop`.
pub fn helix_template_apply_bin() -> Option<PathBuf> {
    if let Ok(path) = env::var("HELIX_TEMPLATE_APPLY_BIN") {
        if !path.is_empty() {
            let candidate = PathBuf::from(path);
            if candidate.is_file() {
                return Some(candidate);
            }
        }
    }
    if let Ok(exe) = env::current_exe() {
        for name in ["helix-template-apply", "helix-template-apply.exe"] {
            let sibling = exe.parent().map(|dir| dir.join(name));
            if sibling.as_ref().is_some_and(|p| p.is_file()) {
                return sibling;
            }
        }
        for ancestor in exe.ancestors() {
            let build = ancestor.join("helix-template-apply");
            if build.is_file() {
                return Some(build);
            }
        }
    }
    None
}

/// C++ `helix-theme-palette` helper for Rust `Appearance1` palette mutations.
pub fn helix_theme_palette_bin() -> Option<PathBuf> {
    if let Ok(path) = env::var("HELIX_THEME_PALETTE_BIN") {
        if !path.is_empty() {
            let candidate = PathBuf::from(path);
            if candidate.is_file() {
                return Some(candidate);
            }
        }
    }
    if let Ok(exe) = env::current_exe() {
        for name in ["helix-theme-palette", "helix-theme-palette.exe"] {
            let sibling = exe.parent().map(|dir| dir.join(name));
            if sibling.as_ref().is_some_and(|p| p.is_file()) {
                return sibling;
            }
        }
        for ancestor in exe.ancestors() {
            let build = ancestor.join("helix-theme-palette");
            if build.is_file() {
                return Some(build);
            }
        }
    }
    None
}
