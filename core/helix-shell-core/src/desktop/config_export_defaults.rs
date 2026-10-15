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
pub fn write_storage_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("key_file".to_string(), Toml::String(String::new()));
    table.insert(
        "key_source".to_string(),
        Toml::String("secret-service".to_string()),
    );
    Toml::Table(table)
}

pub fn write_accessibility_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("high_contrast".to_string(), Toml::Boolean(false));
    table.insert("ui_scale".to_string(), Toml::Float(1.0));
    Toml::Table(table)
}

pub fn write_audio_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("enable_overdrive".to_string(), Toml::Boolean(false));
    table.insert("enable_sounds".to_string(), Toml::Boolean(false));
    table.insert(
        "sound_theme".to_string(),
        Toml::String("freedesktop".to_string()),
    );
    table.insert("sound_volume".to_string(), Toml::Float(0.5));
    Toml::Table(table)
}

pub fn write_battery_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("warning_threshold".to_string(), Toml::Integer(10));
    Toml::Table(table)
}

pub fn write_brightness_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("enable_ddcutil".to_string(), Toml::Boolean(false));
    table.insert("ignore_mmids".to_string(), Toml::Array(vec![]));
    table.insert("minimum_brightness".to_string(), Toml::Float(0.0));
    table.insert("sync_all_monitors".to_string(), Toml::Boolean(false));
    Toml::Table(table)
}

pub fn write_calendar_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("enabled".to_string(), Toml::Boolean(false));
    table.insert(
        "event_date_format".to_string(),
        Toml::String("%A %e %B".to_string()),
    );
    table.insert(
        "event_time_format".to_string(),
        Toml::String("%H:%M".to_string()),
    );
    table.insert("refresh_minutes".to_string(), Toml::Integer(15));
    Toml::Table(table)
}

pub fn write_tailscale_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("admin_url".to_string(), Toml::String(String::new()));
    table.insert("notify_on_peer_change".to_string(), Toml::Boolean(false));
    table.insert("refresh_interval_seconds".to_string(), Toml::Integer(10));
    Toml::Table(table)
}

pub fn write_weather_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("effects".to_string(), Toml::Boolean(true));
    table.insert("enabled".to_string(), Toml::Boolean(true));
    table.insert("refresh_minutes".to_string(), Toml::Integer(30));
    table.insert("unit".to_string(), Toml::String("metric".to_string()));
    Toml::Table(table)
}

pub fn write_nightlight_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("enabled".to_string(), Toml::Boolean(false));
    table.insert("force".to_string(), Toml::Boolean(false));
    table.insert("temperature_day".to_string(), Toml::Integer(6500));
    table.insert("temperature_night".to_string(), Toml::Integer(4000));
    Toml::Table(table)
}

pub fn write_location_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("address".to_string(), Toml::String(String::new()));
    table.insert("auto_locate".to_string(), Toml::Boolean(false));
    table.insert("custom_schedule".to_string(), Toml::Boolean(false));
    table.insert("sunrise".to_string(), Toml::String(String::new()));
    table.insert("sunset".to_string(), Toml::String(String::new()));
    Toml::Table(table)
}

/// Replace golden slices with in-Rust `writeTable` defaults as they are ported.
pub fn overlay_ported_schema_defaults(root: &mut Toml) {
    let root = match root.as_table_mut() {
        Some(table) => table,
        None => return,
    };
    let writers: [(&str, fn() -> Toml); 10] = [
        ("storage", write_storage_defaults),
        ("accessibility", write_accessibility_defaults),
        ("audio", write_audio_defaults),
        ("battery", write_battery_defaults),
        ("brightness", write_brightness_defaults),
        ("calendar", write_calendar_defaults),
        ("tailscale", write_tailscale_defaults),
        ("weather", write_weather_defaults),
        ("nightlight", write_nightlight_defaults),
        ("location", write_location_defaults),
    ];
    for (name, writer) in writers {
        root.insert(name.to_string(), writer());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn golden_root() -> Toml {
        include_str!("../../tests/golden/config_export_minimal.toml")
            .parse()
            .expect("golden parse")
    }

    fn assert_section_matches_golden(name: &str, writer: fn() -> Toml) {
        let golden = golden_root();
        let expected = golden.get(name).expect("golden section");
        assert_eq!(writer(), *expected, "section {name}");
    }

    #[test]
    fn ported_schema_sections_match_golden_export() {
        for (name, writer) in [
            ("storage", write_storage_defaults as fn() -> Toml),
            ("accessibility", write_accessibility_defaults),
            ("audio", write_audio_defaults),
            ("battery", write_battery_defaults),
            ("brightness", write_brightness_defaults),
            ("calendar", write_calendar_defaults),
            ("tailscale", write_tailscale_defaults),
            ("weather", write_weather_defaults),
            ("nightlight", write_nightlight_defaults),
            ("location", write_location_defaults),
        ] {
            assert_section_matches_golden(name, writer);
        }

        let mut assembled = golden_root();
        overlay_ported_schema_defaults(&mut assembled);
        for name in [
            "storage",
            "accessibility",
            "audio",
            "battery",
            "brightness",
            "calendar",
            "tailscale",
            "weather",
            "nightlight",
            "location",
        ] {
            assert_eq!(
                assembled.get(name),
                golden_root().get(name),
                "overlay {name}"
            );
        }
    }
}
