//! Typed `parseConfigTable` → `writeTable` slices for `config_export` (milestone C tail).
//!
//! Every `SCHEMA_EXPORT_SECTIONS` table is exported here (explicit structs, known-leaf overlay,
//! or `shell` via `merge_export_subtree`). `bar` stays in `config_export_rust`.

use toml::Value as Toml;

use super::config_export_defaults::{
    schema_section_default_writer, write_accessibility_defaults, write_battery_defaults,
    write_storage_defaults, write_theme_defaults,
};
use super::config_export_typed_shell::export_shell_section_from_merged;
use super::config_export_rust::export_filtered_schema_section;
use super::config_export_serialize::f32_bits;
use super::config_export_typed_scalar::{
    export_audio_section_from_merged, export_backdrop_section_from_merged,
    export_brightness_section_from_merged, export_calendar_section_from_merged,
    export_hooks_section_from_merged, export_hot_corners_section_from_merged,
    export_control_center_section_from_merged, export_dock_section_from_merged,
    export_idle_section_from_merged, export_keybinds_section_from_merged,
    export_location_section_from_merged, export_lockscreen_section_from_merged,
    export_notification_section_from_merged, export_nightlight_section_from_merged,
    export_osd_section_from_merged, export_system_section_from_merged,
    export_tailscale_section_from_merged, export_wallpaper_section_from_merged,
    export_weather_section_from_merged,
};

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct StorageExport {
    key_file: String,
    key_source: String,
}

impl StorageExport {
    fn export_defaults() -> Self {
        let table = write_storage_defaults();
        Self::from_export_table(&table)
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("storage export table");
        Self {
            key_file: table
                .get("key_file")
                .and_then(Toml::as_str)
                .unwrap_or("")
                .to_string(),
            key_source: table
                .get("key_source")
                .and_then(Toml::as_str)
                .unwrap_or("secret-service")
                .to_string(),
        }
    }

    fn from_merged_section(merged_section: &Toml) -> Self {
        let mut out = Self::export_defaults();
        let table = match merged_section.as_table() {
            Some(table) => table,
            None => return out,
        };
        if let Some(value) = table.get("key_file").and_then(Toml::as_str) {
            out.key_file = value.to_string();
        }
        if let Some(value) = table.get("key_source").and_then(Toml::as_str) {
            out.key_source = value.to_string();
        }
        out
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("key_file".to_string(), Toml::String(self.key_file.clone()));
        table.insert(
            "key_source".to_string(),
            Toml::String(self.key_source.clone()),
        );
        Toml::Table(table)
    }
}

fn export_storage_section_from_merged(merged: &Toml) -> Toml {
    merged
        .get("storage")
        .map(|section| StorageExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| StorageExport::export_defaults().write_export_table())
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct AccessibilityExport {
    high_contrast: bool,
    ui_scale: f64,
}

impl AccessibilityExport {
    fn export_defaults() -> Self {
        Self::from_export_table(&write_accessibility_defaults())
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("accessibility export table");
        Self {
            high_contrast: table
                .get("high_contrast")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
            ui_scale: table
                .get("ui_scale")
                .and_then(|v| v.as_float())
                .map(f32_bits)
                .unwrap_or(1.0),
        }
    }

    fn from_merged_section(merged_section: &Toml) -> Self {
        let mut out = Self::export_defaults();
        let table = match merged_section.as_table() {
            Some(table) => table,
            None => return out,
        };
        if let Some(value) = table.get("high_contrast").and_then(Toml::as_bool) {
            out.high_contrast = value;
        }
        if let Some(value) = table.get("ui_scale").and_then(|v| v.as_float()) {
            out.ui_scale = f32_bits(value);
        }
        out
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("high_contrast".to_string(), Toml::Boolean(self.high_contrast));
        table.insert("ui_scale".to_string(), Toml::Float(self.ui_scale));
        Toml::Table(table)
    }
}

fn export_accessibility_section_from_merged(merged: &Toml) -> Toml {
    merged
        .get("accessibility")
        .map(|section| AccessibilityExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| AccessibilityExport::export_defaults().write_export_table())
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct BatteryExport {
    warning_threshold: i64,
}

impl BatteryExport {
    fn export_defaults() -> Self {
        Self::from_export_table(&write_battery_defaults())
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("battery export table");
        Self {
            warning_threshold: table
                .get("warning_threshold")
                .and_then(Toml::as_integer)
                .unwrap_or(10),
        }
    }

    fn from_merged_section(merged_section: &Toml) -> Self {
        let mut out = Self::export_defaults();
        if let Some(value) = merged_section
            .get("warning_threshold")
            .and_then(Toml::as_integer)
        {
            out.warning_threshold = value;
        }
        out
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert(
            "warning_threshold".to_string(),
            Toml::Integer(self.warning_threshold),
        );
        Toml::Table(table)
    }
}

fn export_battery_section_from_merged(merged: &Toml) -> Toml {
    merged
        .get("battery")
        .map(|section| BatteryExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| BatteryExport::export_defaults().write_export_table())
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ThemeNiriLayoutExport {
    blur: bool,
    blur_noise: f64,
    blur_offset: f64,
    blur_passes: i64,
    border: bool,
    focus_ring: bool,
    overview: String,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ThemeTemplatesExport {
    builtin_ids: Vec<String>,
    community_ids: Vec<String>,
    enable_builtin_templates: bool,
    enable_community_templates: bool,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ThemeExport {
    builtin: String,
    community_palette: String,
    custom_palette: String,
    mode: String,
    pure_black_dark: bool,
    shell_mode: String,
    source: String,
    wallpaper_scheme: String,
    niri_layout: ThemeNiriLayoutExport,
    templates: ThemeTemplatesExport,
}

impl ThemeNiriLayoutExport {
    fn export_defaults() -> Self {
        let root = write_theme_defaults();
        let table = root
            .get("niri_layout")
            .and_then(Toml::as_table)
            .expect("theme.niri_layout");
        Self {
            blur: table.get("blur").and_then(Toml::as_bool).unwrap_or(false),
            blur_noise: table
                .get("blur_noise")
                .and_then(|v| v.as_float())
                .map(f32_bits)
                .unwrap_or(0.03),
            blur_offset: table
                .get("blur_offset")
                .and_then(|v| v.as_float())
                .map(f32_bits)
                .unwrap_or(3.0),
            blur_passes: table
                .get("blur_passes")
                .and_then(Toml::as_integer)
                .unwrap_or(2),
            border: table.get("border").and_then(Toml::as_bool).unwrap_or(false),
            focus_ring: table
                .get("focus_ring")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
            overview: table
                .get("overview")
                .and_then(Toml::as_str)
                .unwrap_or("none")
                .to_string(),
        }
    }

    fn overlay_merged(&mut self, merged: &Toml) {
        let table = match merged.as_table() {
            Some(table) => table,
            None => return,
        };
        if let Some(value) = table.get("blur").and_then(Toml::as_bool) {
            self.blur = value;
        }
        if let Some(value) = table.get("blur_noise").and_then(|v| v.as_float()) {
            self.blur_noise = f32_bits(value);
        }
        if let Some(value) = table.get("blur_offset").and_then(|v| v.as_float()) {
            self.blur_offset = f32_bits(value);
        }
        if let Some(value) = table.get("blur_passes").and_then(Toml::as_integer) {
            self.blur_passes = value;
        }
        if let Some(value) = table.get("border").and_then(Toml::as_bool) {
            self.border = value;
        }
        if let Some(value) = table.get("focus_ring").and_then(Toml::as_bool) {
            self.focus_ring = value;
        }
        if let Some(value) = table.get("overview").and_then(Toml::as_str) {
            self.overview = value.to_string();
        }
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("blur".to_string(), Toml::Boolean(self.blur));
        table.insert("blur_noise".to_string(), Toml::Float(self.blur_noise));
        table.insert("blur_offset".to_string(), Toml::Float(self.blur_offset));
        table.insert("blur_passes".to_string(), Toml::Integer(self.blur_passes));
        table.insert("border".to_string(), Toml::Boolean(self.border));
        table.insert("focus_ring".to_string(), Toml::Boolean(self.focus_ring));
        table.insert("overview".to_string(), Toml::String(self.overview.clone()));
        Toml::Table(table)
    }
}

impl ThemeTemplatesExport {
    fn export_defaults() -> Self {
        let root = write_theme_defaults();
        let table = root
            .get("templates")
            .and_then(Toml::as_table)
            .expect("theme.templates");
        Self {
            builtin_ids: string_array(table.get("builtin_ids")),
            community_ids: string_array(table.get("community_ids")),
            enable_builtin_templates: table
                .get("enable_builtin_templates")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
            enable_community_templates: table
                .get("enable_community_templates")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
        }
    }

    fn overlay_merged(&mut self, merged: &Toml) {
        let table = match merged.as_table() {
            Some(table) => table,
            None => return,
        };
        if let Some(value) = table.get("builtin_ids") {
            self.builtin_ids = string_array(Some(value));
        }
        if let Some(value) = table.get("community_ids") {
            self.community_ids = string_array(Some(value));
        }
        if let Some(value) = table
            .get("enable_builtin_templates")
            .and_then(Toml::as_bool)
        {
            self.enable_builtin_templates = value;
        }
        if let Some(value) = table
            .get("enable_community_templates")
            .and_then(Toml::as_bool)
        {
            self.enable_community_templates = value;
        }
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert(
            "builtin_ids".to_string(),
            Toml::Array(self.builtin_ids.iter().cloned().map(Toml::String).collect()),
        );
        table.insert(
            "community_ids".to_string(),
            Toml::Array(self.community_ids.iter().cloned().map(Toml::String).collect()),
        );
        table.insert(
            "enable_builtin_templates".to_string(),
            Toml::Boolean(self.enable_builtin_templates),
        );
        table.insert(
            "enable_community_templates".to_string(),
            Toml::Boolean(self.enable_community_templates),
        );
        Toml::Table(table)
    }
}

fn string_array(value: Option<&Toml>) -> Vec<String> {
    value
        .and_then(Toml::as_array)
        .map(|items| {
            items
                .iter()
                .filter_map(|item| item.as_str().map(str::to_string))
                .collect()
        })
        .unwrap_or_default()
}

impl ThemeExport {
    fn export_defaults() -> Self {
        Self::from_export_table(&write_theme_defaults())
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("theme export table");
        let mut out = Self {
            builtin: table
                .get("builtin")
                .and_then(Toml::as_str)
                .unwrap_or("Helix")
                .to_string(),
            community_palette: table
                .get("community_palette")
                .and_then(Toml::as_str)
                .unwrap_or("Oxocarbon")
                .to_string(),
            custom_palette: table
                .get("custom_palette")
                .and_then(Toml::as_str)
                .unwrap_or("")
                .to_string(),
            mode: table
                .get("mode")
                .and_then(Toml::as_str)
                .unwrap_or("dark")
                .to_string(),
            pure_black_dark: table
                .get("pure_black_dark")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
            shell_mode: table
                .get("shell_mode")
                .and_then(Toml::as_str)
                .unwrap_or("follow")
                .to_string(),
            source: table
                .get("source")
                .and_then(Toml::as_str)
                .unwrap_or("builtin")
                .to_string(),
            wallpaper_scheme: table
                .get("wallpaper_scheme")
                .and_then(Toml::as_str)
                .unwrap_or("m3-content")
                .to_string(),
            niri_layout: ThemeNiriLayoutExport::export_defaults(),
            templates: ThemeTemplatesExport::export_defaults(),
        };
        if let Some(niri) = table.get("niri_layout") {
            out.niri_layout.overlay_merged(niri);
        }
        if let Some(templates) = table.get("templates") {
            out.templates.overlay_merged(templates);
        }
        out
    }

    fn from_merged_section(merged_section: &Toml) -> Self {
        let mut out = Self::export_defaults();
        let table = match merged_section.as_table() {
            Some(table) => table,
            None => return out,
        };
        if let Some(value) = table.get("builtin").and_then(Toml::as_str) {
            out.builtin = value.to_string();
        }
        if let Some(value) = table.get("community_palette").and_then(Toml::as_str) {
            out.community_palette = value.to_string();
        }
        if let Some(value) = table.get("custom_palette").and_then(Toml::as_str) {
            out.custom_palette = value.to_string();
        }
        if let Some(value) = table.get("mode").and_then(Toml::as_str) {
            out.mode = value.to_string();
        }
        if let Some(value) = table.get("pure_black_dark").and_then(Toml::as_bool) {
            out.pure_black_dark = value;
        }
        if let Some(value) = table.get("shell_mode").and_then(Toml::as_str) {
            out.shell_mode = value.to_string();
        }
        if let Some(value) = table.get("source").and_then(Toml::as_str) {
            out.source = value.to_string();
        }
        if let Some(value) = table.get("wallpaper_scheme").and_then(Toml::as_str) {
            out.wallpaper_scheme = value.to_string();
        }
        if let Some(niri) = table.get("niri_layout") {
            out.niri_layout.overlay_merged(niri);
        }
        if let Some(templates) = table.get("templates") {
            out.templates.overlay_merged(templates);
        }
        out
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("builtin".to_string(), Toml::String(self.builtin.clone()));
        table.insert(
            "community_palette".to_string(),
            Toml::String(self.community_palette.clone()),
        );
        table.insert(
            "custom_palette".to_string(),
            Toml::String(self.custom_palette.clone()),
        );
        table.insert("mode".to_string(), Toml::String(self.mode.clone()));
        table.insert("pure_black_dark".to_string(), Toml::Boolean(self.pure_black_dark));
        table.insert("shell_mode".to_string(), Toml::String(self.shell_mode.clone()));
        table.insert("source".to_string(), Toml::String(self.source.clone()));
        table.insert(
            "wallpaper_scheme".to_string(),
            Toml::String(self.wallpaper_scheme.clone()),
        );
        table.insert("niri_layout".to_string(), self.niri_layout.write_export_table());
        table.insert("templates".to_string(), self.templates.write_export_table());
        Toml::Table(table)
    }
}

fn export_theme_section_from_merged(merged: &Toml) -> Toml {
    merged
        .get("theme")
        .map(|section| ThemeExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| ThemeExport::export_defaults().write_export_table())
}

/// Export one schema section. `bar` is handled only in `config_export_rust`.
pub(crate) fn export_typed_schema_section(
    section: &str,
    merged: &Toml,
    default: &Toml,
) -> Option<Toml> {
    let struct_export = match section {
        "storage" => Some(export_storage_section_from_merged(merged)),
        "accessibility" => Some(export_accessibility_section_from_merged(merged)),
        "battery" => Some(export_battery_section_from_merged(merged)),
        "theme" => Some(export_theme_section_from_merged(merged)),
        "calendar" => Some(export_calendar_section_from_merged(merged)),
        "nightlight" => Some(export_nightlight_section_from_merged(merged)),
        "weather" => Some(export_weather_section_from_merged(merged)),
        "tailscale" => Some(export_tailscale_section_from_merged(merged)),
        "location" => Some(export_location_section_from_merged(merged)),
        "hooks" => Some(export_hooks_section_from_merged(merged)),
        "audio" => Some(export_audio_section_from_merged(merged)),
        "backdrop" => Some(export_backdrop_section_from_merged(merged)),
        "brightness" => Some(export_brightness_section_from_merged(merged)),
        "hot_corners" => Some(export_hot_corners_section_from_merged(merged)),
        "idle" => Some(export_idle_section_from_merged(merged)),
        "notification" => Some(export_notification_section_from_merged(merged)),
        "osd" => Some(export_osd_section_from_merged(merged)),
        "lockscreen" => Some(export_lockscreen_section_from_merged(merged)),
        "system" => Some(export_system_section_from_merged(merged)),
        "control_center" => Some(export_control_center_section_from_merged(merged)),
        "dock" => Some(export_dock_section_from_merged(merged)),
        "wallpaper" => Some(export_wallpaper_section_from_merged(merged)),
        "keybinds" => Some(export_keybinds_section_from_merged(merged)),
        "shell" => Some(export_shell_section_from_merged(merged, default)),
        _ => None,
    };
    if let Some(table) = struct_export {
        return Some(table);
    }
    let writer = schema_section_default_writer(section)?;
    Some(export_filtered_schema_section(section, merged, default, writer))
}

#[cfg(test)]
mod tests {
    use super::*;
    use super::super::config_export_defaults::{PORTED_SCHEMA_WRITERS, SCHEMA_EXPORT_SECTIONS};
    use super::super::config_export_serialize::serialize_default_config;

    #[test]
    fn storage_export_defaults_match_writer() {
        assert_eq!(
            StorageExport::export_defaults().write_export_table(),
            write_storage_defaults()
        );
    }

    #[test]
    fn storage_export_reads_merged_overlay() {
        let merged: Toml = toml::from_str(
            r#"
[storage]
key_source = "file"
key_file = "/etc/helix.key"
"#,
        )
        .unwrap();
        let table = export_storage_section_from_merged(&merged);
        let table = table.as_table().unwrap();
        assert_eq!(table.get("key_source").and_then(Toml::as_str), Some("file"));
        assert_eq!(
            table.get("key_file").and_then(Toml::as_str),
            Some("/etc/helix.key")
        );
    }

    #[test]
    fn theme_export_defaults_match_writer() {
        assert_eq!(
            ThemeExport::export_defaults().write_export_table(),
            write_theme_defaults()
        );
    }

    #[test]
    fn struct_exports_match_filtered_merge_for_theme_and_battery() {
        let default = serialize_default_config();
        let merged: Toml = toml::from_str(
            r#"
[theme]
mode = "light"
pure_black_dark = true

[theme.templates]
builtin_ids = ["helix", "qt"]

[battery]
warning_threshold = 15
"#,
        )
        .unwrap();
        let theme_filtered =
            export_filtered_schema_section("theme", &merged, &default, write_theme_defaults);
        let theme_typed = export_typed_schema_section("theme", &merged, &default).unwrap();
        assert_eq!(theme_typed, theme_filtered);
        let battery_filtered =
            export_filtered_schema_section("battery", &merged, &default, write_battery_defaults);
        let battery_typed = export_typed_schema_section("battery", &merged, &default).unwrap();
        assert_eq!(battery_typed, battery_filtered);
    }

    #[test]
    fn typed_schema_sections_match_default_writers_without_merged_overlay() {
        let default = serialize_default_config();
        let empty = Toml::Table(toml::map::Map::new());
        for &section in SCHEMA_EXPORT_SECTIONS {
            let got = export_typed_schema_section(section, &empty, &default)
                .unwrap_or_else(|| panic!("section {section} should be typed"));
            let writer = PORTED_SCHEMA_WRITERS
                .iter()
                .find(|(name, _)| *name == section)
                .map(|(_, writer)| *writer)
                .unwrap_or_else(|| panic!("writer for {section}"));
            assert_eq!(got, writer(), "section {section}");
        }
    }
}
