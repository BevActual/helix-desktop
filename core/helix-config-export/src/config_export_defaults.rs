//! Schema-default `writeTable` slices for `config_export::serialize` (milestone B).
//!
//! Section order matches the former C++ `helix::config::schema::sections()`
//! (`config_sections.cpp`, removed). Extra root tables (`bar`, `widget`,
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
    table.insert(
        "tint_intensity".to_string(),
        Toml::Float(0.30000001192092896),
    );
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
    table.insert(
        "tint_intensity".to_string(),
        Toml::Float(0.30000001192092896),
    );
    table.insert("wallpaper".to_string(), Toml::String(String::new()));
    Toml::Table(table)
}

pub fn write_notification_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert(
        "background_opacity".to_string(),
        Toml::Float(0.970_000_028_610_229_5),
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
    table.insert(
        "position".to_string(),
        Toml::String("top_right".to_string()),
    );
    table.insert("scale".to_string(), Toml::Float(1.0));
    table.insert("show_actions".to_string(), Toml::Boolean(true));
    table.insert("show_app_name".to_string(), Toml::Boolean(true));
    Toml::Table(table)
}

pub(crate) const HOOK_EVENT_KEYS: &[&str] = &[
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
        Toml::Float(0.879_999_995_231_628_4),
    );
    table.insert("border".to_string(), Toml::String("outline".to_string()));
    table.insert("border_width".to_string(), Toml::Float(0.0));
    table.insert("concave_edge_corners".to_string(), Toml::Boolean(true));
    table.insert("cross_axis_padding".to_string(), Toml::Integer(8));
    table.insert("enabled".to_string(), Toml::Boolean(true));
    table.insert("icon_size".to_string(), Toml::Integer(48));
    table.insert(
        "inactive_opacity".to_string(),
        Toml::Float(0.850_000_023_841_857_9),
    );
    table.insert(
        "inactive_scale".to_string(),
        Toml::Float(0.850_000_023_841_857_9),
    );
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
    table.insert(
        "magnification_scale".to_string(),
        Toml::Float(1.4500000476837158),
    );
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

pub fn write_shell_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("app_icon_colorize".to_string(), Toml::Boolean(false));
    table.insert("avatar_path".to_string(), Toml::String("".to_string()));
    table.insert("button_borders".to_string(), Toml::Boolean(true));
    table.insert("card_borders".to_string(), Toml::Boolean(true));
    table.insert(
        "clipboard_auto_paste".to_string(),
        Toml::String("auto".to_string()),
    );
    table.insert(
        "clipboard_confirm_clear_history".to_string(),
        Toml::Boolean(true),
    );
    table.insert("clipboard_enabled".to_string(), Toml::Boolean(true));
    table.insert(
        "clipboard_history_max_entries".to_string(),
        Toml::Integer(100),
    );
    table.insert(
        "clipboard_image_action_command".to_string(),
        Toml::String("".to_string()),
    );
    table.insert(
        "clipboard_keep_from_closed_apps".to_string(),
        Toml::Boolean(true),
    );
    table.insert("corner_radius_scale".to_string(), Toml::Float(1.0));
    table.insert(
        "date_format".to_string(),
        Toml::String("%A, %x".to_string()),
    );
    table.insert("disable_mipmaps".to_string(), Toml::Boolean(false));
    table.insert("external_ip_enabled".to_string(), Toml::Boolean(false));
    table.insert(
        "font_family".to_string(),
        Toml::String("sans-serif".to_string()),
    );
    table.insert("input_borders".to_string(), Toml::Boolean(true));
    table.insert(
        "launch_apps_as_systemd_services".to_string(),
        Toml::Boolean(true),
    );
    table.insert(
        "launch_apps_custom_command".to_string(),
        Toml::String("".to_string()),
    );
    table.insert(
        "niri_overview_type_to_launch_enabled".to_string(),
        Toml::Boolean(false),
    );
    table.insert("offline_mode".to_string(), Toml::Boolean(false));
    table.insert(
        "password_style".to_string(),
        Toml::String("default".to_string()),
    );
    table.insert("polkit_agent".to_string(), Toml::Boolean(false));
    table.insert("popup_borders".to_string(), Toml::Boolean(true));
    table.insert("popup_shadows".to_string(), Toml::Boolean(true));
    table.insert("screen_time_enabled".to_string(), Toml::Boolean(false));
    table.insert("settings_show_advanced".to_string(), Toml::Boolean(true));
    table.insert("setup_wizard_enabled".to_string(), Toml::Boolean(true));
    table.insert("shared_gl_context".to_string(), Toml::Boolean(true));
    table.insert("show_location".to_string(), Toml::Boolean(true));
    table.insert("telemetry_enabled".to_string(), Toml::Boolean(false));
    table.insert(
        "time_format".to_string(),
        Toml::String("{:%H:%M}".to_string()),
    );
    table.insert("animation".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("enabled".to_string(), Toml::Boolean(true));
        table.insert("speed".to_string(), Toml::Float(1.0));
        Toml::Table(table)
    });
    table.insert("greeter_sync".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("auto_sync".to_string(), Toml::Boolean(false));
        Toml::Table(table)
    });
    table.insert("keyboard_layout".to_string(), {
        let table = toml::map::Map::new();
        Toml::Table(table)
    });
    table.insert("launcher".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("app_grid".to_string(), Toml::Boolean(false));
        table.insert("auto_paste".to_string(), Toml::String("auto".to_string()));
        table.insert("categories".to_string(), Toml::Boolean(true));
        table.insert("compact".to_string(), Toml::Boolean(false));
        table.insert("fetch_exchange_rates".to_string(), Toml::Boolean(true));
        table.insert("pinned".to_string(), Toml::Array(vec![]));
        table.insert("provider_prefix".to_string(), Toml::String("/".to_string()));
        table.insert("show_app_actions".to_string(), Toml::Boolean(false));
        table.insert("show_app_origin_indicator".to_string(), Toml::Boolean(true));
        table.insert("show_icons".to_string(), Toml::Boolean(true));
        table.insert("sort_by_usage".to_string(), Toml::Boolean(true));
        table.insert("dmenu".to_string(), {
            let table = toml::map::Map::new();
            Toml::Table(table)
        });
        table.insert("panels".to_string(), {
            let mut table = toml::map::Map::new();
            table.insert("ignored".to_string(), {
                Toml::Array(vec![
                    Toml::String("polkit".to_string()),
                    Toml::String("setup-wizard".to_string()),
                    Toml::String("test".to_string()),
                    Toml::String("launcher".to_string()),
                ])
            });
            Toml::Table(table)
        });
        Toml::Table(table)
    });
    table.insert("mpris".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("blacklist".to_string(), Toml::Array(vec![]));
        Toml::Table(table)
    });
    table.insert("panel".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("borders".to_string(), Toml::Boolean(true));
        table.insert(
            "clipboard_placement".to_string(),
            Toml::String("floating".to_string()),
        );
        table.insert(
            "clipboard_position".to_string(),
            Toml::String("center".to_string()),
        );
        table.insert(
            "control_center_placement".to_string(),
            Toml::String("floating".to_string()),
        );
        table.insert(
            "control_center_position".to_string(),
            Toml::String("auto".to_string()),
        );
        table.insert(
            "floating_layer".to_string(),
            Toml::String("overlay".to_string()),
        );
        table.insert("floating_offset".to_string(), Toml::Integer(8));
        table.insert(
            "launcher_placement".to_string(),
            Toml::String("floating".to_string()),
        );
        table.insert(
            "launcher_position".to_string(),
            Toml::String("center".to_string()),
        );
        table.insert("list_item_background".to_string(), Toml::Boolean(false));
        table.insert(
            "open_near_click_clipboard".to_string(),
            Toml::Boolean(false),
        );
        table.insert(
            "open_near_click_control_center".to_string(),
            Toml::Boolean(true),
        );
        table.insert("open_near_click_launcher".to_string(), Toml::Boolean(false));
        table.insert(
            "open_near_click_quick_insert".to_string(),
            Toml::Boolean(false),
        );
        table.insert("open_near_click_session".to_string(), Toml::Boolean(false));
        table.insert(
            "open_near_click_wallpaper".to_string(),
            Toml::Boolean(false),
        );
        table.insert(
            "polkit_placement".to_string(),
            Toml::String("floating".to_string()),
        );
        table.insert(
            "polkit_position".to_string(),
            Toml::String("center".to_string()),
        );
        table.insert(
            "quick_insert_placement".to_string(),
            Toml::String("floating".to_string()),
        );
        table.insert(
            "quick_insert_position".to_string(),
            Toml::String("center".to_string()),
        );
        table.insert("refraction".to_string(), Toml::Boolean(true));
        table.insert(
            "session_placement".to_string(),
            Toml::String("attached".to_string()),
        );
        table.insert(
            "session_position".to_string(),
            Toml::String("auto".to_string()),
        );
        table.insert("shadow".to_string(), Toml::Boolean(true));
        table.insert(
            "transparency_mode".to_string(),
            Toml::String("soft".to_string()),
        );
        table.insert(
            "wallpaper_placement".to_string(),
            Toml::String("attached".to_string()),
        );
        table.insert(
            "wallpaper_position".to_string(),
            Toml::String("auto".to_string()),
        );
        Toml::Table(table)
    });
    table.insert("privacy".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("cam_filter_regex".to_string(), Toml::String("".to_string()));
        table.insert("mic_filter_regex".to_string(), Toml::String("".to_string()));
        table.insert(
            "screen_filter_regex".to_string(),
            Toml::String("".to_string()),
        );
        Toml::Table(table)
    });
    table.insert("screen_corners".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("enabled".to_string(), Toml::Boolean(false));
        table.insert("size".to_string(), Toml::Integer(32));
        Toml::Table(table)
    });
    table.insert("screen_record".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("audio".to_string(), Toml::Boolean(true));
        table.insert("directory".to_string(), Toml::String("".to_string()));
        table.insert("filename_pattern".to_string(), Toml::String("".to_string()));
        table.insert("replay_duration".to_string(), Toml::Integer(30));
        table.insert("show_cursor".to_string(), Toml::Boolean(true));
        Toml::Table(table)
    });
    table.insert("screenshot".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("annotate".to_string(), Toml::Boolean(false));
        table.insert("close_on_copy".to_string(), Toml::Boolean(true));
        table.insert("close_on_save".to_string(), Toml::Boolean(true));
        table.insert("confirm_region".to_string(), Toml::Boolean(false));
        table.insert("copy_to_clipboard".to_string(), Toml::Boolean(true));
        table.insert("directory".to_string(), Toml::String("".to_string()));
        table.insert("filename_pattern".to_string(), Toml::String("".to_string()));
        table.insert("freeze_screen".to_string(), Toml::Boolean(true));
        table.insert("pipe_command".to_string(), Toml::String("".to_string()));
        table.insert("pipe_to_command".to_string(), Toml::Boolean(false));
        table.insert("remember_last_region".to_string(), Toml::Boolean(false));
        table.insert("save_to_file".to_string(), Toml::Boolean(true));
        table.insert("show_cursor".to_string(), Toml::Boolean(false));
        table.insert(
            "skip_annotate_on_copy_save".to_string(),
            Toml::Boolean(false),
        );
        Toml::Table(table)
    });
    table.insert("session".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("grid".to_string(), Toml::Boolean(false));
        table.insert("grid_columns".to_string(), Toml::Integer(3));
        table.insert("show_shortcuts".to_string(), Toml::Boolean(true));
        table.insert("power".to_string(), {
            let table = toml::map::Map::new();
            Toml::Table(table)
        });
        table.insert("actions".to_string(), {
            Toml::Array(vec![
                {
                    let mut table = toml::map::Map::new();
                    table.insert("action".to_string(), Toml::String("lock".to_string()));
                    table.insert("command".to_string(), Toml::String("".to_string()));
                    table.insert("countdown_seconds".to_string(), Toml::Float(0.0));
                    table.insert("enabled".to_string(), Toml::Boolean(true));
                    table.insert("glyph".to_string(), Toml::String("".to_string()));
                    table.insert("label".to_string(), Toml::String("".to_string()));
                    table.insert("shortcut".to_string(), Toml::String("1".to_string()));
                    table.insert("variant".to_string(), Toml::String("default".to_string()));
                    Toml::Table(table)
                },
                {
                    let mut table = toml::map::Map::new();
                    table.insert("action".to_string(), Toml::String("logout".to_string()));
                    table.insert("command".to_string(), Toml::String("".to_string()));
                    table.insert("countdown_seconds".to_string(), Toml::Float(0.0));
                    table.insert("enabled".to_string(), Toml::Boolean(true));
                    table.insert("glyph".to_string(), Toml::String("".to_string()));
                    table.insert("label".to_string(), Toml::String("".to_string()));
                    table.insert("shortcut".to_string(), Toml::String("2".to_string()));
                    table.insert("variant".to_string(), Toml::String("default".to_string()));
                    Toml::Table(table)
                },
                {
                    let mut table = toml::map::Map::new();
                    table.insert(
                        "action".to_string(),
                        Toml::String("lock_and_suspend".to_string()),
                    );
                    table.insert("command".to_string(), Toml::String("".to_string()));
                    table.insert("countdown_seconds".to_string(), Toml::Float(0.0));
                    table.insert("enabled".to_string(), Toml::Boolean(true));
                    table.insert("glyph".to_string(), Toml::String("".to_string()));
                    table.insert("label".to_string(), Toml::String("".to_string()));
                    table.insert("shortcut".to_string(), Toml::String("3".to_string()));
                    table.insert("variant".to_string(), Toml::String("default".to_string()));
                    Toml::Table(table)
                },
                {
                    let mut table = toml::map::Map::new();
                    table.insert("action".to_string(), Toml::String("reboot".to_string()));
                    table.insert("command".to_string(), Toml::String("".to_string()));
                    table.insert("countdown_seconds".to_string(), Toml::Float(0.0));
                    table.insert("enabled".to_string(), Toml::Boolean(true));
                    table.insert("glyph".to_string(), Toml::String("".to_string()));
                    table.insert("label".to_string(), Toml::String("".to_string()));
                    table.insert("shortcut".to_string(), Toml::String("4".to_string()));
                    table.insert("variant".to_string(), Toml::String("default".to_string()));
                    Toml::Table(table)
                },
                {
                    let mut table = toml::map::Map::new();
                    table.insert("action".to_string(), Toml::String("shutdown".to_string()));
                    table.insert("command".to_string(), Toml::String("".to_string()));
                    table.insert("countdown_seconds".to_string(), Toml::Float(0.0));
                    table.insert("enabled".to_string(), Toml::Boolean(true));
                    table.insert("glyph".to_string(), Toml::String("".to_string()));
                    table.insert("label".to_string(), Toml::String("".to_string()));
                    table.insert("shortcut".to_string(), Toml::String("5".to_string()));
                    table.insert(
                        "variant".to_string(),
                        Toml::String("destructive".to_string()),
                    );
                    Toml::Table(table)
                },
            ])
        });
        Toml::Table(table)
    });
    table.insert("shadow".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("alpha".to_string(), Toml::Float(0.550000011920929));
        table.insert("direction".to_string(), Toml::String("down".to_string()));
        Toml::Table(table)
    });
    table.insert("window_switcher".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("mru".to_string(), Toml::Boolean(false));
        Toml::Table(table)
    });
    Toml::Table(table)
}

pub fn write_wallpaper_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("directory".to_string(), Toml::String("".to_string()));
    table.insert("directory_dark".to_string(), Toml::String("".to_string()));
    table.insert("directory_light".to_string(), Toml::String("".to_string()));
    table.insert(
        "edge_smoothness".to_string(),
        Toml::Float(0.30000001192092896),
    );
    table.insert("enabled".to_string(), Toml::Boolean(true));
    table.insert("fill_color".to_string(), Toml::String("".to_string()));
    table.insert("fill_mode".to_string(), Toml::String("crop".to_string()));
    table.insert("per_monitor_directories".to_string(), Toml::Boolean(false));
    table.insert("transition".to_string(), {
        Toml::Array(vec![
            Toml::String("fade".to_string()),
            Toml::String("wipe".to_string()),
            Toml::String("disc".to_string()),
            Toml::String("stripes".to_string()),
            Toml::String("zoom".to_string()),
            Toml::String("honeycomb".to_string()),
        ])
    });
    table.insert("transition_duration".to_string(), Toml::Float(1500.0));
    table.insert("transition_on_startup".to_string(), Toml::Boolean(false));
    table.insert("automation".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("enabled".to_string(), Toml::Boolean(false));
        table.insert("interval_seconds".to_string(), Toml::Integer(1800));
        table.insert("order".to_string(), Toml::String("random".to_string()));
        table.insert("recursive".to_string(), Toml::Boolean(false));
        table.insert("source".to_string(), Toml::String("folder".to_string()));
        Toml::Table(table)
    });
    table.insert("daily".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("bing_market".to_string(), Toml::String("".to_string()));
        table.insert("download_dir".to_string(), Toml::String("".to_string()));
        table.insert("nasa_api_key".to_string(), Toml::String("".to_string()));
        table.insert("provider".to_string(), Toml::String("bing".to_string()));
        Toml::Table(table)
    });
    table.insert("video".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("auto_pause".to_string(), Toml::String("full".to_string()));
        table.insert("directory".to_string(), Toml::String("".to_string()));
        table.insert("hardware_decode".to_string(), Toml::Boolean(true));
        table.insert("mpv_options".to_string(), Toml::String("".to_string()));
        table.insert("mute".to_string(), Toml::Boolean(true));
        Toml::Table(table)
    });
    table.insert("wallhaven".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("api_key".to_string(), Toml::String("".to_string()));
        table.insert("download_dir".to_string(), Toml::String("".to_string()));
        Toml::Table(table)
    });
    Toml::Table(table)
}

pub fn write_theme_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("builtin".to_string(), Toml::String("Helix".to_string()));
    table.insert(
        "community_palette".to_string(),
        Toml::String("Oxocarbon".to_string()),
    );
    table.insert("custom_palette".to_string(), Toml::String("".to_string()));
    table.insert("mode".to_string(), Toml::String("dark".to_string()));
    table.insert("pure_black_dark".to_string(), Toml::Boolean(false));
    table.insert("shell_mode".to_string(), Toml::String("follow".to_string()));
    table.insert("source".to_string(), Toml::String("builtin".to_string()));
    table.insert(
        "wallpaper_scheme".to_string(),
        Toml::String("m3-content".to_string()),
    );
    table.insert("niri_layout".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("blur".to_string(), Toml::Boolean(false));
        table.insert("blur_noise".to_string(), Toml::Float(0.029999999329447746));
        table.insert("blur_offset".to_string(), Toml::Float(3.0));
        table.insert("blur_passes".to_string(), Toml::Integer(2));
        table.insert("border".to_string(), Toml::Boolean(false));
        table.insert("focus_ring".to_string(), Toml::Boolean(false));
        table.insert("overview".to_string(), Toml::String("none".to_string()));
        Toml::Table(table)
    });
    table.insert("templates".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("builtin_ids".to_string(), Toml::Array(vec![]));
        table.insert("community_ids".to_string(), Toml::Array(vec![]));
        table.insert("enable_builtin_templates".to_string(), Toml::Boolean(true));
        table.insert(
            "enable_community_templates".to_string(),
            Toml::Boolean(true),
        );
        Toml::Table(table)
    });
    Toml::Table(table)
}

pub fn write_osd_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert(
        "background_opacity".to_string(),
        Toml::Float(0.9700000286102295),
    );
    table.insert("border".to_string(), Toml::Boolean(true));
    table.insert("enabled".to_string(), Toml::Boolean(true));
    table.insert("monitors".to_string(), Toml::Array(vec![]));
    table.insert("offset_x".to_string(), Toml::Integer(20));
    table.insert("offset_y".to_string(), Toml::Integer(8));
    table.insert(
        "orientation".to_string(),
        Toml::String("horizontal".to_string()),
    );
    table.insert(
        "position".to_string(),
        Toml::String("top_center".to_string()),
    );
    table.insert(
        "position_vertical".to_string(),
        Toml::String("top_center".to_string()),
    );
    table.insert("scale".to_string(), Toml::Float(1.0));
    table.insert("kinds".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("bluetooth".to_string(), Toml::Boolean(true));
        table.insert("brightness".to_string(), Toml::Boolean(true));
        table.insert("caffeine".to_string(), Toml::Boolean(true));
        table.insert("dnd".to_string(), Toml::Boolean(true));
        table.insert("keyboard_backlight".to_string(), Toml::Boolean(true));
        table.insert("keyboard_layout".to_string(), Toml::Boolean(true));
        table.insert("lock_keys".to_string(), Toml::Boolean(true));
        table.insert("media".to_string(), Toml::Boolean(true));
        table.insert("nightlight".to_string(), Toml::Boolean(true));
        table.insert("power_profile".to_string(), Toml::Boolean(true));
        table.insert("privacy".to_string(), Toml::Boolean(true));
        table.insert("volume".to_string(), Toml::Boolean(true));
        table.insert("volume_input".to_string(), Toml::Boolean(true));
        table.insert("volume_output".to_string(), Toml::Boolean(true));
        table.insert("wifi".to_string(), Toml::Boolean(true));
        Toml::Table(table)
    });
    Toml::Table(table)
}

pub fn write_system_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("monitor".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("cpu_freq_activity_threshold".to_string(), Toml::Float(2.5));
        table.insert("cpu_freq_critical_threshold".to_string(), Toml::Float(4.5));
        table.insert("cpu_poll_seconds".to_string(), Toml::Float(2.0));
        table.insert("cpu_temp_activity_threshold".to_string(), Toml::Float(60.0));
        table.insert("cpu_temp_critical_threshold".to_string(), Toml::Float(85.0));
        table.insert(
            "cpu_temp_sensor_path".to_string(),
            Toml::String("".to_string()),
        );
        table.insert(
            "cpu_usage_activity_threshold".to_string(),
            Toml::Float(50.0),
        );
        table.insert(
            "cpu_usage_critical_threshold".to_string(),
            Toml::Float(90.0),
        );
        table.insert(
            "disk_free_activity_threshold".to_string(),
            Toml::Float(80.0),
        );
        table.insert(
            "disk_free_critical_threshold".to_string(),
            Toml::Float(95.0),
        );
        table.insert(
            "disk_free_pct_activity_threshold".to_string(),
            Toml::Float(80.0),
        );
        table.insert(
            "disk_free_pct_critical_threshold".to_string(),
            Toml::Float(95.0),
        );
        table.insert("disk_poll_seconds".to_string(), Toml::Float(10.0));
        table.insert(
            "disk_used_activity_threshold".to_string(),
            Toml::Float(80.0),
        );
        table.insert(
            "disk_used_critical_threshold".to_string(),
            Toml::Float(95.0),
        );
        table.insert(
            "disk_used_pct_activity_threshold".to_string(),
            Toml::Float(80.0),
        );
        table.insert(
            "disk_used_pct_critical_threshold".to_string(),
            Toml::Float(95.0),
        );
        table.insert("enabled".to_string(), Toml::Boolean(true));
        table.insert("gpu_poll_seconds".to_string(), Toml::Float(5.0));
        table.insert("gpu_temp_activity_threshold".to_string(), Toml::Float(60.0));
        table.insert("gpu_temp_critical_threshold".to_string(), Toml::Float(85.0));
        table.insert(
            "gpu_usage_activity_threshold".to_string(),
            Toml::Float(50.0),
        );
        table.insert(
            "gpu_usage_critical_threshold".to_string(),
            Toml::Float(95.0),
        );
        table.insert("gpu_vram_activity_threshold".to_string(), Toml::Float(50.0));
        table.insert("gpu_vram_critical_threshold".to_string(), Toml::Float(90.0));
        table.insert("memory_poll_seconds".to_string(), Toml::Float(2.0));
        table.insert("net_rx_activity_threshold".to_string(), Toml::Float(1.0));
        table.insert("net_rx_critical_threshold".to_string(), Toml::Float(50.0));
        table.insert("net_tx_activity_threshold".to_string(), Toml::Float(1.0));
        table.insert("net_tx_critical_threshold".to_string(), Toml::Float(50.0));
        table.insert("network_poll_seconds".to_string(), Toml::Float(3.0));
        table.insert("ram_pct_activity_threshold".to_string(), Toml::Float(60.0));
        table.insert("ram_pct_critical_threshold".to_string(), Toml::Float(90.0));
        table.insert("swap_pct_activity_threshold".to_string(), Toml::Float(20.0));
        table.insert("swap_pct_critical_threshold".to_string(), Toml::Float(80.0));
        Toml::Table(table)
    });
    Toml::Table(table)
}

pub fn write_control_center_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("height".to_string(), Toml::Integer(700));
    table.insert("hidden_tabs".to_string(), Toml::Array(vec![]));
    table.insert("shortcuts".to_string(), Toml::Array(vec![]));
    table.insert("show_session_button".to_string(), Toml::Boolean(true));
    table.insert("sidebar".to_string(), Toml::String("compact".to_string()));
    table.insert(
        "sidebar_section".to_string(),
        Toml::String("compact".to_string()),
    );
    table.insert("width".to_string(), Toml::Integer(700));
    table.insert("ai_usage".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("claude".to_string(), Toml::String("auto".to_string()));
        table.insert("codex".to_string(), Toml::String("auto".to_string()));
        table.insert("copilot".to_string(), Toml::String("auto".to_string()));
        table.insert("cursor".to_string(), Toml::String("auto".to_string()));
        table.insert("deepseek".to_string(), Toml::String("auto".to_string()));
        table.insert("gemini".to_string(), Toml::String("auto".to_string()));
        table.insert("gemini_daily_limit".to_string(), Toml::Integer(1000));
        table.insert("localai".to_string(), Toml::String("auto".to_string()));
        table.insert("localai_host".to_string(), Toml::String("".to_string()));
        table.insert("localai_model".to_string(), Toml::String("".to_string()));
        table.insert("ollama".to_string(), Toml::String("auto".to_string()));
        table.insert("ollama_host".to_string(), Toml::String("".to_string()));
        table.insert("ollama_model".to_string(), Toml::String("".to_string()));
        table.insert("qwen".to_string(), Toml::String("auto".to_string()));
        table.insert("qwen_daily_limit".to_string(), Toml::Integer(2000));
        table.insert("refresh_seconds".to_string(), Toml::Integer(120));
        table.insert("warn_percent".to_string(), Toml::Integer(90));
        table.insert("warn_reset_minutes".to_string(), Toml::Integer(30));
        Toml::Table(table)
    });
    table.insert("bluetooth".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("discovery_seconds".to_string(), Toml::Integer(10));
        table.insert("hide_unnamed".to_string(), Toml::Boolean(false));
        Toml::Table(table)
    });
    table.insert("calendar".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("show_events_card".to_string(), Toml::Boolean(true));
        table.insert("show_week_numbers".to_string(), Toml::Boolean(false));
        Toml::Table(table)
    });
    table.insert("clock".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("timezones".to_string(), Toml::Array(vec![]));
        Toml::Table(table)
    });
    table.insert("network".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("custom_dns".to_string(), Toml::String("".to_string()));
        table.insert("hidden_ssids".to_string(), Toml::Array(vec![]));
        table.insert("nextdns_id".to_string(), Toml::String("".to_string()));
        table.insert("wifi_band".to_string(), Toml::String("all".to_string()));
        Toml::Table(table)
    });
    Toml::Table(table)
}

pub fn write_bar_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("order".to_string(), {
        Toml::Array(vec![Toml::String("default".to_string())])
    });
    table.insert("default".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("auto_hide".to_string(), Toml::Boolean(false));
        table.insert(
            "background_opacity".to_string(),
            Toml::Float(0.7200000286102295),
        );
        table.insert("border".to_string(), Toml::String("outline".to_string()));
        table.insert("border_width".to_string(), Toml::Float(0.0));
        table.insert("capsule".to_string(), Toml::Boolean(false));
        table.insert(
            "capsule_fill".to_string(),
            Toml::String("surface_variant".to_string()),
        );
        table.insert(
            "capsule_opacity".to_string(),
            Toml::Float(0.550000011920929),
        );
        table.insert("capsule_padding".to_string(), Toml::Float(6.0));
        table.insert(
            "capsule_thickness".to_string(),
            Toml::Float(0.7200000286102295),
        );
        table.insert("center".to_string(), Toml::Array(vec![]));
        table.insert("concave_edge_corners".to_string(), Toml::Boolean(false));
        table.insert("contact_shadow".to_string(), Toml::Boolean(false));
        table.insert("enabled".to_string(), Toml::Boolean(true));
        table.insert("end".to_string(), {
            Toml::Array(vec![
                Toml::String("media".to_string()),
                Toml::String("notifications".to_string()),
                Toml::String("tray".to_string()),
                Toml::String("group:status".to_string()),
            ])
        });
        table.insert("font_scale".to_string(), Toml::Float(1.0));
        table.insert("font_weight".to_string(), Toml::Integer(500));
        table.insert("hover_highlight".to_string(), Toml::Boolean(true));
        table.insert("layer".to_string(), Toml::String("top".to_string()));
        table.insert("margin_edge".to_string(), Toml::Integer(8));
        table.insert("margin_ends".to_string(), Toml::Integer(12));
        table.insert("margin_opposite_edge".to_string(), Toml::Integer(0));
        table.insert("padding".to_string(), Toml::Integer(12));
        table.insert("panel_overlap".to_string(), Toml::Integer(1));
        table.insert("position".to_string(), Toml::String("top".to_string()));
        table.insert("radius".to_string(), Toml::Integer(18));
        table.insert("radius_bottom_left".to_string(), Toml::Integer(18));
        table.insert("radius_bottom_right".to_string(), Toml::Integer(18));
        table.insert("radius_top_left".to_string(), Toml::Integer(18));
        table.insert("radius_top_right".to_string(), Toml::Integer(18));
        table.insert("reserve_space".to_string(), Toml::Boolean(true));
        table.insert("scale".to_string(), Toml::Float(1.0));
        table.insert("shadow".to_string(), Toml::Boolean(true));
        table.insert("show_on_workspace_switch".to_string(), Toml::Boolean(true));
        table.insert("smart_auto_hide".to_string(), Toml::Boolean(false));
        table.insert("start".to_string(), {
            Toml::Array(vec![Toml::String("clock".to_string())])
        });
        table.insert("thickness".to_string(), Toml::Integer(34));
        table.insert("widget_spacing".to_string(), Toml::Integer(8));
        table.insert("dead_zone".to_string(), {
            let table = toml::map::Map::new();
            Toml::Table(table)
        });
        table.insert("capsule_group".to_string(), {
            Toml::Array(vec![{
                let mut table = toml::map::Map::new();
                table.insert("accordion".to_string(), Toml::Boolean(false));
                table.insert(
                    "accordion_direction".to_string(),
                    Toml::String("end".to_string()),
                );
                table.insert("enabled".to_string(), Toml::Boolean(true));
                table.insert(
                    "fill".to_string(),
                    Toml::String("surface_variant".to_string()),
                );
                table.insert("id".to_string(), Toml::String("status".to_string()));
                table.insert("members".to_string(), {
                    Toml::Array(vec![
                        Toml::String("network".to_string()),
                        Toml::String("bluetooth".to_string()),
                        Toml::String("volume".to_string()),
                        Toml::String("battery".to_string()),
                    ])
                });
                table.insert("opacity".to_string(), Toml::Float(0.550000011920929));
                table.insert("padding".to_string(), Toml::Float(6.0));
                Toml::Table(table)
            }])
        });
        Toml::Table(table)
    });
    Toml::Table(table)
}

pub fn write_widget_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("active_window".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("icon_size".to_string(), Toml::Float(14.0));
        table.insert("max_length".to_string(), Toml::Float(260.0));
        table.insert("min_length".to_string(), Toml::Float(80.0));
        table.insert("title_scroll".to_string(), Toml::String("none".to_string()));
        table.insert(
            "type".to_string(),
            Toml::String("active_window".to_string()),
        );
        Toml::Table(table)
    });
    table.insert("cpu".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("stat".to_string(), Toml::String("cpu_usage".to_string()));
        table.insert("type".to_string(), Toml::String("sysmon".to_string()));
        Toml::Table(table)
    });
    table.insert("date".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert(
            "format".to_string(),
            Toml::String("{:%a %d %b}".to_string()),
        );
        table.insert("type".to_string(), Toml::String("clock".to_string()));
        Toml::Table(table)
    });
    table.insert("input_volume".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("device".to_string(), Toml::String("input".to_string()));
        table.insert("type".to_string(), Toml::String("volume".to_string()));
        Toml::Table(table)
    });
    table.insert("keyboard_layout".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("hide_when_single_layout".to_string(), Toml::Boolean(false));
        table.insert(
            "type".to_string(),
            Toml::String("keyboard_layout".to_string()),
        );
        Toml::Table(table)
    });
    table.insert("lock_keys".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("display".to_string(), Toml::String("short".to_string()));
        table.insert("hide_when_off".to_string(), Toml::Boolean(false));
        table.insert("show_caps_lock".to_string(), Toml::Boolean(true));
        table.insert("show_num_lock".to_string(), Toml::Boolean(true));
        table.insert("show_scroll_lock".to_string(), Toml::Boolean(false));
        table.insert("type".to_string(), Toml::String("lock_keys".to_string()));
        Toml::Table(table)
    });
    table.insert("media".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("art_size".to_string(), Toml::Float(16.0));
        table.insert("max_length".to_string(), Toml::Float(220.0));
        table.insert("min_length".to_string(), Toml::Float(80.0));
        table.insert("title_scroll".to_string(), Toml::String("none".to_string()));
        table.insert("type".to_string(), Toml::String("media".to_string()));
        Toml::Table(table)
    });
    table.insert("network_rx".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("stat".to_string(), Toml::String("net_rx".to_string()));
        table.insert("type".to_string(), Toml::String("sysmon".to_string()));
        Toml::Table(table)
    });
    table.insert("network_tx".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("stat".to_string(), Toml::String("net_tx".to_string()));
        table.insert("type".to_string(), Toml::String("sysmon".to_string()));
        Toml::Table(table)
    });
    table.insert("output_volume".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("device".to_string(), Toml::String("output".to_string()));
        table.insert("type".to_string(), Toml::String("volume".to_string()));
        Toml::Table(table)
    });
    table.insert("ram".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("stat".to_string(), Toml::String("ram_used".to_string()));
        table.insert("type".to_string(), Toml::String("sysmon".to_string()));
        Toml::Table(table)
    });
    table.insert("spacer".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("interactive".to_string(), Toml::Boolean(false));
        table.insert("type".to_string(), Toml::String("spacer".to_string()));
        Toml::Table(table)
    });
    table.insert("temp".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("stat".to_string(), Toml::String("cpu_temp".to_string()));
        table.insert("type".to_string(), Toml::String("sysmon".to_string()));
        Toml::Table(table)
    });
    Toml::Table(table)
}

pub fn write_desktop_widgets_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("enabled".to_string(), Toml::Boolean(true));
    table.insert("schema_version".to_string(), Toml::Integer(2));
    table.insert("grid".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("cell_size".to_string(), Toml::Integer(16));
        table.insert("major_interval".to_string(), Toml::Integer(4));
        table.insert("visible".to_string(), Toml::Boolean(true));
        Toml::Table(table)
    });
    Toml::Table(table)
}

pub fn write_lockscreen_widgets_defaults() -> Toml {
    let mut table = toml::map::Map::new();
    table.insert("enabled".to_string(), Toml::Boolean(false));
    table.insert("schema_version".to_string(), Toml::Integer(2));
    table.insert("grid".to_string(), {
        let mut table = toml::map::Map::new();
        table.insert("cell_size".to_string(), Toml::Integer(16));
        table.insert("major_interval".to_string(), Toml::Integer(4));
        table.insert("visible".to_string(), Toml::Boolean(true));
        Toml::Table(table)
    });
    Toml::Table(table)
}

/// A root table name and the writer that produces its schema-default export.
pub(crate) type ExportWriter = (&'static str, fn() -> Toml);

pub(crate) const PORTED_SCHEMA_WRITERS: &[ExportWriter] = &[
    ("storage", write_storage_defaults),
    ("shell", write_shell_defaults),
    ("accessibility", write_accessibility_defaults),
    ("wallpaper", write_wallpaper_defaults),
    ("theme", write_theme_defaults),
    ("backdrop", write_backdrop_defaults),
    ("lockscreen", write_lockscreen_defaults),
    ("notification", write_notification_defaults),
    ("osd", write_osd_defaults),
    ("system", write_system_defaults),
    ("weather", write_weather_defaults),
    ("tailscale", write_tailscale_defaults),
    ("calendar", write_calendar_defaults),
    ("audio", write_audio_defaults),
    ("brightness", write_brightness_defaults),
    ("battery", write_battery_defaults),
    ("nightlight", write_nightlight_defaults),
    ("location", write_location_defaults),
    ("idle", write_idle_defaults),
    ("keybinds", write_keybinds_defaults),
    ("dock", write_dock_defaults),
    ("hot_corners", write_hot_corners_defaults),
    ("control_center", write_control_center_defaults),
    ("hooks", write_hooks_defaults),
];

pub(crate) fn schema_section_default_writer(section: &str) -> Option<fn() -> Toml> {
    PORTED_SCHEMA_WRITERS
        .iter()
        .find(|(name, _)| *name == section)
        .map(|(_, writer)| *writer)
}

/// `config_export::serialize` root tables outside `config_sections.cpp`.
pub(crate) const EXTRA_EXPORT_ROOT_WRITERS: &[ExportWriter] = &[
    ("bar", write_bar_defaults),
    ("widget", write_widget_defaults),
    ("desktop_widgets", write_desktop_widgets_defaults),
    ("lockscreen_widgets", write_lockscreen_widgets_defaults),
];

/// Full schema-default export (`makeDefaultConfig` + `config_export::serialize`).
pub fn assemble_default_export() -> Toml {
    let mut root = Toml::Table(toml::map::Map::new());
    overlay_ported_schema_defaults(&mut root);
    overlay_extra_export_roots(&mut root);
    root
}

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

pub fn overlay_extra_export_roots(root: &mut Toml) {
    let root = match root.as_table_mut() {
        Some(table) => table,
        None => return,
    };
    for (name, writer) in EXTRA_EXPORT_ROOT_WRITERS {
        root.insert(name.to_string(), writer());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn golden_root() -> Toml {
        include_str!("../tests/golden/config_export_minimal.toml")
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
        for (name, writer) in EXTRA_EXPORT_ROOT_WRITERS {
            assert_section_matches_golden(name, *writer);
        }

        assert_eq!(assemble_default_export(), golden_root());
    }
}
