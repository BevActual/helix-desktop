//! Palette ids for Configuration1 `palette` / `community` / `custom` setting options.

use std::fs;
use std::path::{Path, PathBuf};

pub const BUILTIN_PALETTE_NAMES: &[&str] = &[
    "Ayu",
    "Catppuccin",
    "Dracula",
    "Eldritch",
    "Gruvbox",
    "Kanagawa",
    "Helix",
    "Nord",
    "Rosé Pine",
    "Tokyo-Night",
];

fn community_catalog_path(state_dir: &Path) -> PathBuf {
    state_dir
        .join("community-palettes")
        .join(".catalog")
        .join("palettes.json")
}

pub fn list_community_palette_names(state_dir: &Path) -> Vec<String> {
    let path = community_catalog_path(state_dir);
    let Ok(text) = fs::read_to_string(&path) else {
        return Vec::new();
    };
    let Ok(root) = serde_json::from_str::<serde_json::Value>(&text) else {
        return Vec::new();
    };
    let entries = root
        .as_array()
        .or_else(|| root.get("palettes").and_then(|v| v.as_array()));
    let Some(entries) = entries else {
        return Vec::new();
    };
    let mut names = Vec::new();
    for item in entries {
        let name = if let Some(text) = item.as_str() {
            text.to_string()
        } else {
            item.get("name")
                .and_then(|v| v.as_str())
                .unwrap_or("")
                .to_string()
        };
        if !name.is_empty() {
            names.push(name);
        }
    }
    names.sort_by_key(|a| a.to_lowercase());
    names.dedup();
    names.truncate(100);
    names
}

pub fn list_custom_palette_names(config_dir: &Path) -> Vec<String> {
    let dir = config_dir.join("palettes");
    let Ok(entries) = fs::read_dir(&dir) else {
        return Vec::new();
    };
    let mut names = Vec::new();
    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) != Some("json") {
            continue;
        }
        if let Some(stem) = path.file_stem().and_then(|s| s.to_str()) {
            names.push(stem.to_string());
        }
    }
    names.sort_by_key(|a| a.to_lowercase());
    names.truncate(100);
    names
}
