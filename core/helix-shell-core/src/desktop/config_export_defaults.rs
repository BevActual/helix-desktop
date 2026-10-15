//! Schema-default `writeTable` slices for `config_export::serialize` (milestone B).
//!
//! Section order matches `helix::config::schema::sections()` in
//! `src/config/schema/config_sections.cpp`. Extra root tables (`bar`, `widget`,
//! `desktop_widgets`, `lockscreen_widgets`) are emitted after these sections in C++.

use toml::Value as Toml;

/// Top-level sections written by `config_export::serialize` before `bar` / `widget` roots.
#[allow(dead_code)]
pub const SCHEMA_EXPORT_SECTIONS: &[&str] = &[
    "storage",
    "shell",
    "accessibility",
    "wallpaper",
    "theme",
    "backdrop",
    "lockscreen",
    "notification",
    "osd",
    "system",
    "weather",
    "tailscale",
    "calendar",
    "audio",
    "brightness",
    "battery",
    "nightlight",
    "location",
    "idle",
    "keybinds",
    "dock",
    "hot_corners",
    "control_center",
    "hooks",
];

/// `writeTable` for default `StorageConfig` (`storageSchema` / `makeDefaultConfig`).
#[allow(dead_code)]
pub fn write_storage_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("key_file".to_string(), Toml::String(String::new()));
    table.insert(
        "key_source".to_string(),
        Toml::String("secret-service".to_string()),
    );
    Toml::Table(table)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn storage_defaults_match_golden_export() {
        let golden: Toml = include_str!("../../tests/golden/config_export_minimal.toml")
            .parse()
            .expect("golden parse");
        let expected = golden.get("storage").expect("golden storage section");
        assert_eq!(write_storage_defaults(), *expected);
    }
}
