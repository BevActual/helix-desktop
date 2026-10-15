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

pub fn write_backdrop_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("blur_intensity".to_string(), Toml::Float(0.5));
    table.insert("enabled".to_string(), Toml::Boolean(false));
    table.insert("tint_intensity".to_string(), Toml::Float(0.30000001192092896));
    Toml::Table(table)
}

pub fn write_lockscreen_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("allow_empty_password".to_string(), Toml::Boolean(false));
    table.insert("blur_intensity".to_string(), Toml::Float(0.5));
    table.insert("blurred_desktop".to_string(), Toml::Boolean(false));
    table.insert("enabled".to_string(), Toml::Boolean(true));
    table.insert("fingerprint".to_string(), Toml::Boolean(true));
    table.insert(
        "lid_close_keep_external_displays".to_string(),
        Toml::Boolean(true),
    );
    table.insert("lock_before_suspend".to_string(), Toml::Boolean(true));
    table.insert("monitors".to_string(), Toml::Array(vec![]));
    table.insert("tint_intensity".to_string(), Toml::Float(0.30000001192092896));
    table.insert("wallpaper".to_string(), Toml::String(String::new()));
    Toml::Table(table)
}

pub fn write_notification_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert(
        "background_opacity".to_string(),
        Toml::Float(0.97000002861022949),
    );
    table.insert("border".to_string(), Toml::Boolean(true));
    table.insert("collapse_on_dismiss".to_string(), Toml::Boolean(true));
    table.insert("enable_daemon".to_string(), Toml::Boolean(true));
    table.insert("filter_replace".to_string(), Toml::Boolean(false));
    table.insert("history_retention_hours".to_string(), Toml::Integer(0));
    table.insert("keep_dismissed_in_history".to_string(), Toml::Boolean(true));
    table.insert("layer".to_string(), Toml::String("top".to_string()));
    table.insert("max_visible".to_string(), Toml::Integer(0));
    table.insert("monitors".to_string(), Toml::Array(vec![]));
    table.insert("offset_x".to_string(), Toml::Integer(20));
    table.insert("offset_y".to_string(), Toml::Integer(8));
    table.insert("position".to_string(), Toml::String("top_right".to_string()));
    table.insert("scale".to_string(), Toml::Float(1.0));
    table.insert("show_actions".to_string(), Toml::Boolean(true));
    table.insert("show_app_name".to_string(), Toml::Boolean(true));
    Toml::Table(table)
}

const HOOK_EVENT_KEYS: &[&str] = &[
    "battery_charging",
    "battery_discharging",
    "battery_percentage_changed",
    "battery_plugged",
    "bluetooth_disabled",
    "bluetooth_enabled",
    "colors_changed",
    "logging_out",
    "power_profile_changed",
    "rebooting",
    "session_locked",
    "session_unlocked",
    "shutting_down",
    "started",
    "theme_mode_changed",
    "wallpaper_changed",
    "wifi_disabled",
    "wifi_enabled",
];

pub fn write_hooks_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    for key in HOOK_EVENT_KEYS {
        table.insert(key.to_string(), Toml::Array(vec![]));
    }
    Toml::Table(table)
}

fn hot_corner_action_none() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("action".to_string(), Toml::String("none".to_string()));
    table.insert("command".to_string(), Toml::String(String::new()));
    Toml::Table(table)
}

pub fn write_hot_corners_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("delay_ms".to_string(), Toml::Integer(0));
    table.insert("enabled".to_string(), Toml::Boolean(false));
    table.insert("bottom_left".to_string(), hot_corner_action_none());
    table.insert("bottom_right".to_string(), hot_corner_action_none());
    table.insert("top_left".to_string(), hot_corner_action_none());
    table.insert("top_right".to_string(), hot_corner_action_none());
    Toml::Table(table)
}

fn idle_behavior(action: &str, timeout: f64) -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("action".to_string(), Toml::String(action.to_string()));
    table.insert("command".to_string(), Toml::String(String::new()));
    table.insert("enabled".to_string(), Toml::Boolean(false));
    table.insert("locked_timeout".to_string(), Toml::Float(0.0));
    table.insert("resume_command".to_string(), Toml::String(String::new()));
    table.insert("timeout".to_string(), Toml::Float(timeout));
    Toml::Table(table)
}

pub fn write_idle_defaults() -> Toml {
    let mut behavior = toml::map::Map::new();
    behavior.insert("lock".to_string(), idle_behavior("lock", 600.0));
    behavior.insert(
        "lock-and-suspend".to_string(),
        idle_behavior("lock_and_suspend", 900.0),
    );
    behavior.insert("screen-off".to_string(), idle_behavior("screen_off", 660.0));
    let mut table = toml::map::Map::new();
    table.insert(
        "behavior_order".to_string(),
        Toml::Array(vec![
            Toml::String("lock".to_string()),
            Toml::String("screen-off".to_string()),
            Toml::String("lock-and-suspend".to_string()),
        ]),
    );
    table.insert("pre_action_fade_seconds".to_string(), Toml::Float(2.0));
    table.insert("behavior".to_string(), Toml::Table(behavior));
    Toml::Table(table)
}

pub fn write_keybinds_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert(
        "cancel".to_string(),
        Toml::Array(vec![Toml::String("Escape".to_string())]),
    );
    table.insert(
        "copy".to_string(),
        Toml::Array(vec![Toml::String("Ctrl+c".to_string())]),
    );
    table.insert(
        "delete".to_string(),
        Toml::Array(vec![Toml::String("Delete".to_string())]),
    );
    table.insert(
        "down".to_string(),
        Toml::Array(vec![Toml::String("Down".to_string())]),
    );
    table.insert(
        "left".to_string(),
        Toml::Array(vec![Toml::String("Left".to_string())]),
    );
    table.insert(
        "right".to_string(),
        Toml::Array(vec![Toml::String("Right".to_string())]),
    );
    table.insert(
        "save".to_string(),
        Toml::Array(vec![Toml::String("Ctrl+s".to_string())]),
    );
    table.insert(
        "tab_next".to_string(),
        Toml::Array(vec![Toml::String("Tab".to_string())]),
    );
    table.insert(
        "tab_previous".to_string(),
        Toml::Array(vec![Toml::String("Shift+ISO_Left_Tab".to_string())]),
    );
    table.insert(
        "up".to_string(),
        Toml::Array(vec![Toml::String("Up".to_string())]),
    );
    table.insert(
        "validate".to_string(),
        Toml::Array(vec![
            Toml::String("Return".to_string()),
            Toml::String("KP_Enter".to_string()),
            Toml::String("space".to_string()),
        ]),
    );
    Toml::Table(table)
}

pub fn write_dock_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("active_monitor_only".to_string(), Toml::Boolean(false));
    table.insert("active_opacity".to_string(), Toml::Float(1.0));
    table.insert("active_scale".to_string(), Toml::Float(1.0));
    table.insert("auto_hide".to_string(), Toml::Boolean(false));
    table.insert(
        "background_opacity".to_string(),
        Toml::Float(0.87999999523162842),
    );
    table.insert("border".to_string(), Toml::String("outline".to_string()));
    table.insert("border_width".to_string(), Toml::Float(0.0));
    table.insert("concave_edge_corners".to_string(), Toml::Boolean(true));
    table.insert("cross_axis_padding".to_string(), Toml::Integer(8));
    table.insert("enabled".to_string(), Toml::Boolean(true));
    table.insert("icon_size".to_string(), Toml::Integer(48));
    table.insert("inactive_opacity".to_string(), Toml::Float(0.85000002384185791));
    table.insert("inactive_scale".to_string(), Toml::Float(0.85000002384185791));
    table.insert("item_spacing".to_string(), Toml::Integer(6));
    table.insert(
        "launcher_custom_image".to_string(),
        Toml::String(String::new()),
    );
    table.insert(
        "launcher_custom_image_colorize".to_string(),
        Toml::Boolean(false),
    );
    table.insert(
        "launcher_icon".to_string(),
        Toml::String("grid-dots".to_string()),
    );
    table.insert(
        "launcher_position".to_string(),
        Toml::String("start".to_string()),
    );
    table.insert("layer".to_string(), Toml::String("top".to_string()));
    table.insert("magnification".to_string(), Toml::Boolean(true));
    table.insert("magnification_scale".to_string(), Toml::Float(1.4500000476837158));
    table.insert("main_axis_padding".to_string(), Toml::Integer(16));
    table.insert("margin_edge".to_string(), Toml::Integer(10));
    table.insert("margin_ends".to_string(), Toml::Integer(0));
    table.insert("monitors".to_string(), Toml::Array(vec![]));
    table.insert("pinned".to_string(), Toml::Array(vec![]));
    table.insert("position".to_string(), Toml::String("bottom".to_string()));
    table.insert("radius".to_string(), Toml::Integer(16));
    table.insert("radius_bottom_left".to_string(), Toml::Integer(16));
    table.insert("radius_bottom_right".to_string(), Toml::Integer(16));
    table.insert("radius_top_left".to_string(), Toml::Integer(16));
    table.insert("radius_top_right".to_string(), Toml::Integer(16));
    table.insert("reserve_space".to_string(), Toml::Boolean(true));
    table.insert("shadow".to_string(), Toml::Boolean(true));
    table.insert("show_dots".to_string(), Toml::Boolean(true));
    table.insert("show_instance_count".to_string(), Toml::Boolean(true));
    table.insert("show_running".to_string(), Toml::Boolean(true));
    table.insert("smart_auto_hide".to_string(), Toml::Boolean(false));
    Toml::Table(table)
}

const PORTED_SCHEMA_WRITERS: &[(&str, fn() -> Toml)] = &[
    ("storage", write_storage_defaults),
    ("accessibility", write_accessibility_defaults),
    ("backdrop", write_backdrop_defaults),
    ("lockscreen", write_lockscreen_defaults),
    ("notification", write_notification_defaults),
    ("audio", write_audio_defaults),
    ("battery", write_battery_defaults),
    ("brightness", write_brightness_defaults),
    ("calendar", write_calendar_defaults),
    ("tailscale", write_tailscale_defaults),
    ("weather", write_weather_defaults),
    ("nightlight", write_nightlight_defaults),
    ("location", write_location_defaults),
    ("idle", write_idle_defaults),
    ("keybinds", write_keybinds_defaults),
    ("dock", write_dock_defaults),
    ("hot_corners", write_hot_corners_defaults),
    ("hooks", write_hooks_defaults),
];

/// Replace golden slices with in-Rust `writeTable` defaults as they are ported.
pub fn overlay_ported_schema_defaults(root: &mut Toml) {
    let root = match root.as_table_mut() {
        Some(table) => table,
        None => return,
    };
    for (name, writer) in PORTED_SCHEMA_WRITERS {
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
        for (name, writer) in PORTED_SCHEMA_WRITERS {
            assert_section_matches_golden(name, *writer);
        }

        let mut assembled = golden_root();
        overlay_ported_schema_defaults(&mut assembled);
        for (name, _) in PORTED_SCHEMA_WRITERS {
            assert_eq!(
                assembled.get(name),
                golden_root().get(name),
                "overlay {name}"
            );
        }
    }
}
