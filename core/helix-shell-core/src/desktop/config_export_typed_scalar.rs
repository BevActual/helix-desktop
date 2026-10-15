//! Small schema sections with explicit parse → `writeTable` exporters.

use toml::Value as Toml;

use super::config_export_defaults::{
    write_audio_defaults, write_backdrop_defaults, write_brightness_defaults,
    write_calendar_defaults, write_hooks_defaults, write_hot_corners_defaults,
    write_idle_defaults, write_location_defaults, write_nightlight_defaults,
    write_control_center_defaults, write_dock_defaults, write_keybinds_defaults,
    write_lockscreen_defaults, write_notification_defaults, write_osd_defaults,
    write_system_defaults, write_tailscale_defaults, write_wallpaper_defaults, write_weather_defaults,
    HOOK_EVENT_KEYS,
};
use super::config_export_rust::merged_overlay_for_parsed_section;
use super::config_export_serialize::f32_bits;

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

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct CalendarExport {
    enabled: bool,
    event_date_format: String,
    event_time_format: String,
    refresh_minutes: i64,
}

impl CalendarExport {
    fn export_defaults() -> Self {
        Self::from_export_table(&write_calendar_defaults())
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("calendar export table");
        Self {
            enabled: table.get("enabled").and_then(Toml::as_bool).unwrap_or(false),
            event_date_format: table
                .get("event_date_format")
                .and_then(Toml::as_str)
                .unwrap_or("%A %e %B")
                .to_string(),
            event_time_format: table
                .get("event_time_format")
                .and_then(Toml::as_str)
                .unwrap_or("%H:%M")
                .to_string(),
            refresh_minutes: table
                .get("refresh_minutes")
                .and_then(Toml::as_integer)
                .unwrap_or(15),
        }
    }

    fn from_merged_section(merged_section: &Toml) -> Self {
        let mut out = Self::export_defaults();
        let table = match merged_section.as_table() {
            Some(table) => table,
            None => return out,
        };
        if let Some(value) = table.get("enabled").and_then(Toml::as_bool) {
            out.enabled = value;
        }
        if let Some(value) = table.get("event_date_format").and_then(Toml::as_str) {
            out.event_date_format = value.to_string();
        }
        if let Some(value) = table.get("event_time_format").and_then(Toml::as_str) {
            out.event_time_format = value.to_string();
        }
        if let Some(value) = table.get("refresh_minutes").and_then(Toml::as_integer) {
            out.refresh_minutes = value;
        }
        out
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("enabled".to_string(), Toml::Boolean(self.enabled));
        table.insert(
            "event_date_format".to_string(),
            Toml::String(self.event_date_format.clone()),
        );
        table.insert(
            "event_time_format".to_string(),
            Toml::String(self.event_time_format.clone()),
        );
        table.insert("refresh_minutes".to_string(), Toml::Integer(self.refresh_minutes));
        Toml::Table(table)
    }
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct AudioExport {
    enable_overdrive: bool,
    enable_sounds: bool,
    sound_theme: String,
    sound_volume: f64,
}

impl AudioExport {
    fn export_defaults() -> Self {
        Self::from_export_table(&write_audio_defaults())
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("audio export table");
        Self {
            enable_overdrive: table
                .get("enable_overdrive")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
            enable_sounds: table
                .get("enable_sounds")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
            sound_theme: table
                .get("sound_theme")
                .and_then(Toml::as_str)
                .unwrap_or("freedesktop")
                .to_string(),
            sound_volume: table
                .get("sound_volume")
                .and_then(|v| v.as_float())
                .map(f32_bits)
                .unwrap_or(0.5),
        }
    }

    fn from_merged_section(merged_section: &Toml) -> Self {
        let mut out = Self::export_defaults();
        let table = match merged_section.as_table() {
            Some(table) => table,
            None => return out,
        };
        if let Some(value) = table.get("enable_overdrive").and_then(Toml::as_bool) {
            out.enable_overdrive = value;
        }
        if let Some(value) = table.get("enable_sounds").and_then(Toml::as_bool) {
            out.enable_sounds = value;
        }
        if let Some(value) = table.get("sound_theme").and_then(Toml::as_str) {
            out.sound_theme = value.to_string();
        }
        if let Some(value) = table.get("sound_volume").and_then(|v| v.as_float()) {
            out.sound_volume = f32_bits(value);
        }
        out
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("enable_overdrive".to_string(), Toml::Boolean(self.enable_overdrive));
        table.insert("enable_sounds".to_string(), Toml::Boolean(self.enable_sounds));
        table.insert("sound_theme".to_string(), Toml::String(self.sound_theme.clone()));
        table.insert("sound_volume".to_string(), Toml::Float(self.sound_volume));
        Toml::Table(table)
    }
}

pub(crate) fn export_audio_section_from_merged(merged: &Toml) -> Toml {
    merged
        .get("audio")
        .map(|section| AudioExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| AudioExport::export_defaults().write_export_table())
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct BackdropExport {
    blur_intensity: f64,
    enabled: bool,
    tint_intensity: f64,
}

impl BackdropExport {
    fn export_defaults() -> Self {
        Self::from_export_table(&write_backdrop_defaults())
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("backdrop export table");
        Self {
            blur_intensity: table
                .get("blur_intensity")
                .and_then(|v| v.as_float())
                .map(f32_bits)
                .unwrap_or(0.5),
            enabled: table.get("enabled").and_then(Toml::as_bool).unwrap_or(false),
            tint_intensity: table
                .get("tint_intensity")
                .and_then(|v| v.as_float())
                .map(f32_bits)
                .unwrap_or(0.30000001192092896),
        }
    }

    fn from_merged_section(merged_section: &Toml) -> Self {
        let mut out = Self::export_defaults();
        let table = match merged_section.as_table() {
            Some(table) => table,
            None => return out,
        };
        if let Some(value) = table.get("blur_intensity").and_then(|v| v.as_float()) {
            out.blur_intensity = f32_bits(value);
        }
        if let Some(value) = table.get("enabled").and_then(Toml::as_bool) {
            out.enabled = value;
        }
        if let Some(value) = table.get("tint_intensity").and_then(|v| v.as_float()) {
            out.tint_intensity = f32_bits(value);
        }
        out
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("blur_intensity".to_string(), Toml::Float(self.blur_intensity));
        table.insert("enabled".to_string(), Toml::Boolean(self.enabled));
        table.insert("tint_intensity".to_string(), Toml::Float(self.tint_intensity));
        Toml::Table(table)
    }
}

pub(crate) fn export_backdrop_section_from_merged(merged: &Toml) -> Toml {
    merged
        .get("backdrop")
        .map(|section| BackdropExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| BackdropExport::export_defaults().write_export_table())
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct BrightnessExport {
    enable_ddcutil: bool,
    ignore_mmids: Vec<String>,
    minimum_brightness: f64,
    sync_all_monitors: bool,
}

impl BrightnessExport {
    fn export_defaults() -> Self {
        Self::from_export_table(&write_brightness_defaults())
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("brightness export table");
        Self {
            enable_ddcutil: table
                .get("enable_ddcutil")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
            ignore_mmids: string_array(table.get("ignore_mmids")),
            minimum_brightness: table
                .get("minimum_brightness")
                .and_then(|v| v.as_float())
                .map(f32_bits)
                .unwrap_or(0.0),
            sync_all_monitors: table
                .get("sync_all_monitors")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
        }
    }

    fn from_merged_section(merged_section: &Toml) -> Self {
        let mut out = Self::export_defaults();
        let table = match merged_section.as_table() {
            Some(table) => table,
            None => return out,
        };
        if let Some(value) = table.get("enable_ddcutil").and_then(Toml::as_bool) {
            out.enable_ddcutil = value;
        }
        if let Some(value) = table.get("ignore_mmids") {
            out.ignore_mmids = string_array(Some(value));
        }
        if let Some(value) = table.get("minimum_brightness").and_then(|v| v.as_float()) {
            out.minimum_brightness = f32_bits(value);
        }
        if let Some(value) = table.get("sync_all_monitors").and_then(Toml::as_bool) {
            out.sync_all_monitors = value;
        }
        out
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("enable_ddcutil".to_string(), Toml::Boolean(self.enable_ddcutil));
        table.insert(
            "ignore_mmids".to_string(),
            Toml::Array(self.ignore_mmids.iter().cloned().map(Toml::String).collect()),
        );
        table.insert(
            "minimum_brightness".to_string(),
            Toml::Float(self.minimum_brightness),
        );
        table.insert(
            "sync_all_monitors".to_string(),
            Toml::Boolean(self.sync_all_monitors),
        );
        Toml::Table(table)
    }
}

pub(crate) fn export_brightness_section_from_merged(merged: &Toml) -> Toml {
    merged
        .get("brightness")
        .map(|section| BrightnessExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| BrightnessExport::export_defaults().write_export_table())
}

pub(crate) fn export_calendar_section_from_merged(merged: &Toml) -> Toml {
    merged
        .get("calendar")
        .map(|section| CalendarExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| CalendarExport::export_defaults().write_export_table())
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct NightlightExport {
    enabled: bool,
    force: bool,
    temperature_day: i64,
    temperature_night: i64,
}

impl NightlightExport {
    fn export_defaults() -> Self {
        Self::from_export_table(&write_nightlight_defaults())
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("nightlight export table");
        Self {
            enabled: table.get("enabled").and_then(Toml::as_bool).unwrap_or(false),
            force: table.get("force").and_then(Toml::as_bool).unwrap_or(false),
            temperature_day: table
                .get("temperature_day")
                .and_then(Toml::as_integer)
                .unwrap_or(6500),
            temperature_night: table
                .get("temperature_night")
                .and_then(Toml::as_integer)
                .unwrap_or(4000),
        }
    }

    fn from_merged_section(merged_section: &Toml) -> Self {
        let mut out = Self::export_defaults();
        let table = match merged_section.as_table() {
            Some(table) => table,
            None => return out,
        };
        if let Some(value) = table.get("enabled").and_then(Toml::as_bool) {
            out.enabled = value;
        }
        if let Some(value) = table.get("force").and_then(Toml::as_bool) {
            out.force = value;
        }
        if let Some(value) = table.get("temperature_day").and_then(Toml::as_integer) {
            out.temperature_day = value;
        }
        if let Some(value) = table.get("temperature_night").and_then(Toml::as_integer) {
            out.temperature_night = value;
        }
        out
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("enabled".to_string(), Toml::Boolean(self.enabled));
        table.insert("force".to_string(), Toml::Boolean(self.force));
        table.insert("temperature_day".to_string(), Toml::Integer(self.temperature_day));
        table.insert(
            "temperature_night".to_string(),
            Toml::Integer(self.temperature_night),
        );
        Toml::Table(table)
    }
}

pub(crate) fn export_nightlight_section_from_merged(merged: &Toml) -> Toml {
    merged
        .get("nightlight")
        .map(|section| NightlightExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| NightlightExport::export_defaults().write_export_table())
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct WeatherExport {
    effects: bool,
    enabled: bool,
    refresh_minutes: i64,
    unit: String,
}

impl WeatherExport {
    fn export_defaults() -> Self {
        Self::from_export_table(&write_weather_defaults())
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("weather export table");
        Self {
            effects: table.get("effects").and_then(Toml::as_bool).unwrap_or(true),
            enabled: table.get("enabled").and_then(Toml::as_bool).unwrap_or(true),
            refresh_minutes: table
                .get("refresh_minutes")
                .and_then(Toml::as_integer)
                .unwrap_or(30),
            unit: table
                .get("unit")
                .and_then(Toml::as_str)
                .unwrap_or("metric")
                .to_string(),
        }
    }

    fn from_merged_section(merged_section: &Toml) -> Self {
        let mut out = Self::export_defaults();
        let table = match merged_section.as_table() {
            Some(table) => table,
            None => return out,
        };
        if let Some(value) = table.get("effects").and_then(Toml::as_bool) {
            out.effects = value;
        }
        if let Some(value) = table.get("enabled").and_then(Toml::as_bool) {
            out.enabled = value;
        }
        if let Some(value) = table.get("refresh_minutes").and_then(Toml::as_integer) {
            out.refresh_minutes = value;
        }
        if let Some(value) = table.get("unit").and_then(Toml::as_str) {
            out.unit = value.to_string();
        }
        out
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("effects".to_string(), Toml::Boolean(self.effects));
        table.insert("enabled".to_string(), Toml::Boolean(self.enabled));
        table.insert("refresh_minutes".to_string(), Toml::Integer(self.refresh_minutes));
        table.insert("unit".to_string(), Toml::String(self.unit.clone()));
        Toml::Table(table)
    }
}

pub(crate) fn export_weather_section_from_merged(merged: &Toml) -> Toml {
    merged
        .get("weather")
        .map(|section| WeatherExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| WeatherExport::export_defaults().write_export_table())
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct TailscaleExport {
    admin_url: String,
    notify_on_peer_change: bool,
    refresh_interval_seconds: i64,
}

impl TailscaleExport {
    fn export_defaults() -> Self {
        Self::from_export_table(&write_tailscale_defaults())
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("tailscale export table");
        Self {
            admin_url: table
                .get("admin_url")
                .and_then(Toml::as_str)
                .unwrap_or("")
                .to_string(),
            notify_on_peer_change: table
                .get("notify_on_peer_change")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
            refresh_interval_seconds: table
                .get("refresh_interval_seconds")
                .and_then(Toml::as_integer)
                .unwrap_or(10),
        }
    }

    fn from_merged_section(merged_section: &Toml) -> Self {
        let mut out = Self::export_defaults();
        let table = match merged_section.as_table() {
            Some(table) => table,
            None => return out,
        };
        if let Some(value) = table.get("admin_url").and_then(Toml::as_str) {
            out.admin_url = value.to_string();
        }
        if let Some(value) = table.get("notify_on_peer_change").and_then(Toml::as_bool) {
            out.notify_on_peer_change = value;
        }
        if let Some(value) = table
            .get("refresh_interval_seconds")
            .and_then(Toml::as_integer)
        {
            out.refresh_interval_seconds = value;
        }
        out
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("admin_url".to_string(), Toml::String(self.admin_url.clone()));
        table.insert(
            "notify_on_peer_change".to_string(),
            Toml::Boolean(self.notify_on_peer_change),
        );
        table.insert(
            "refresh_interval_seconds".to_string(),
            Toml::Integer(self.refresh_interval_seconds),
        );
        Toml::Table(table)
    }
}

pub(crate) fn export_tailscale_section_from_merged(merged: &Toml) -> Toml {
    merged
        .get("tailscale")
        .map(|section| TailscaleExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| TailscaleExport::export_defaults().write_export_table())
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct LocationExport {
    address: String,
    auto_locate: bool,
    custom_schedule: bool,
    sunrise: String,
    sunset: String,
}

impl LocationExport {
    fn export_defaults() -> Self {
        Self::from_export_table(&write_location_defaults())
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("location export table");
        Self {
            address: table
                .get("address")
                .and_then(Toml::as_str)
                .unwrap_or("")
                .to_string(),
            auto_locate: table
                .get("auto_locate")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
            custom_schedule: table
                .get("custom_schedule")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
            sunrise: table
                .get("sunrise")
                .and_then(Toml::as_str)
                .unwrap_or("")
                .to_string(),
            sunset: table
                .get("sunset")
                .and_then(Toml::as_str)
                .unwrap_or("")
                .to_string(),
        }
    }

    fn from_merged_section(merged_section: &Toml) -> Self {
        let mut out = Self::export_defaults();
        let table = match merged_section.as_table() {
            Some(table) => table,
            None => return out,
        };
        if let Some(value) = table.get("address").and_then(Toml::as_str) {
            out.address = value.to_string();
        }
        if let Some(value) = table.get("auto_locate").and_then(Toml::as_bool) {
            out.auto_locate = value;
        }
        if let Some(value) = table.get("custom_schedule").and_then(Toml::as_bool) {
            out.custom_schedule = value;
        }
        if let Some(value) = table.get("sunrise").and_then(Toml::as_str) {
            out.sunrise = value.to_string();
        }
        if let Some(value) = table.get("sunset").and_then(Toml::as_str) {
            out.sunset = value.to_string();
        }
        out
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("address".to_string(), Toml::String(self.address.clone()));
        table.insert("auto_locate".to_string(), Toml::Boolean(self.auto_locate));
        table.insert(
            "custom_schedule".to_string(),
            Toml::Boolean(self.custom_schedule),
        );
        table.insert("sunrise".to_string(), Toml::String(self.sunrise.clone()));
        table.insert("sunset".to_string(), Toml::String(self.sunset.clone()));
        Toml::Table(table)
    }
}

pub(crate) fn export_location_section_from_merged(merged: &Toml) -> Toml {
    merged
        .get("location")
        .map(|section| LocationExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| LocationExport::export_defaults().write_export_table())
}

const HOT_CORNER_NAMES: &[&str] = &[
    "bottom_left",
    "bottom_right",
    "top_left",
    "top_right",
];

#[derive(Clone, Debug, PartialEq, Eq)]
struct HotCornerActionExport {
    action: String,
    command: String,
}

impl HotCornerActionExport {
    fn none() -> Self {
        Self {
            action: "none".to_string(),
            command: String::new(),
        }
    }

    fn overlay_merged(&mut self, merged: &Toml) {
        let table = match merged.as_table() {
            Some(table) => table,
            None => return,
        };
        if let Some(value) = table.get("action").and_then(Toml::as_str) {
            self.action = value.to_string();
        }
        if let Some(value) = table.get("command").and_then(Toml::as_str) {
            self.command = value.to_string();
        }
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("action".to_string(), Toml::String(self.action.clone()));
        table.insert("command".to_string(), Toml::String(self.command.clone()));
        Toml::Table(table)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct HotCornersExport {
    delay_ms: i64,
    enabled: bool,
    bottom_left: HotCornerActionExport,
    bottom_right: HotCornerActionExport,
    top_left: HotCornerActionExport,
    top_right: HotCornerActionExport,
}

impl HotCornersExport {
    fn export_defaults() -> Self {
        Self::from_export_table(&write_hot_corners_defaults())
    }

    fn corner_mut(&mut self, name: &str) -> Option<&mut HotCornerActionExport> {
        match name {
            "bottom_left" => Some(&mut self.bottom_left),
            "bottom_right" => Some(&mut self.bottom_right),
            "top_left" => Some(&mut self.top_left),
            "top_right" => Some(&mut self.top_right),
            _ => None,
        }
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("hot_corners export table");
        let mut out = Self {
            delay_ms: table.get("delay_ms").and_then(Toml::as_integer).unwrap_or(0),
            enabled: table.get("enabled").and_then(Toml::as_bool).unwrap_or(false),
            bottom_left: HotCornerActionExport::none(),
            bottom_right: HotCornerActionExport::none(),
            top_left: HotCornerActionExport::none(),
            top_right: HotCornerActionExport::none(),
        };
        for name in HOT_CORNER_NAMES {
            if let Some(corner) = table.get(*name) {
                out.corner_mut(name)
                    .expect("hot corner name")
                    .overlay_merged(corner);
            }
        }
        out
    }

    fn from_merged_section(merged_section: &Toml) -> Self {
        let mut out = Self::export_defaults();
        let table = match merged_section.as_table() {
            Some(table) => table,
            None => return out,
        };
        if let Some(value) = table.get("delay_ms").and_then(Toml::as_integer) {
            out.delay_ms = value;
        }
        if let Some(value) = table.get("enabled").and_then(Toml::as_bool) {
            out.enabled = value;
        }
        for name in HOT_CORNER_NAMES {
            if let Some(corner) = table.get(*name) {
                out.corner_mut(name)
                    .expect("hot corner name")
                    .overlay_merged(corner);
            }
        }
        out
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("delay_ms".to_string(), Toml::Integer(self.delay_ms));
        table.insert("enabled".to_string(), Toml::Boolean(self.enabled));
        table.insert(
            "bottom_left".to_string(),
            self.bottom_left.write_export_table(),
        );
        table.insert(
            "bottom_right".to_string(),
            self.bottom_right.write_export_table(),
        );
        table.insert("top_left".to_string(), self.top_left.write_export_table());
        table.insert("top_right".to_string(), self.top_right.write_export_table());
        Toml::Table(table)
    }
}

pub(crate) fn export_hot_corners_section_from_merged(merged: &Toml) -> Toml {
    merged
        .get("hot_corners")
        .map(|section| HotCornersExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| HotCornersExport::export_defaults().write_export_table())
}

/// `idle.behavior` / `behavior_order` are rebuilt in `config_export_rust` after export.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct IdleExport {
    pre_action_fade_seconds: f64,
}

impl IdleExport {
    fn export_defaults() -> Self {
        let table = write_idle_defaults();
        Self {
            pre_action_fade_seconds: table
                .get("pre_action_fade_seconds")
                .and_then(|v| v.as_float())
                .map(f32_bits)
                .unwrap_or(2.0),
        }
    }

    fn from_merged_section(merged_section: &Toml) -> Self {
        let mut out = Self::export_defaults();
        if let Some(value) = merged_section
            .get("pre_action_fade_seconds")
            .and_then(|v| v.as_float())
        {
            out.pre_action_fade_seconds = f32_bits(value);
        }
        out
    }

    fn write_export_table(&self) -> Toml {
        let mut out = write_idle_defaults();
        out.as_table_mut()
            .expect("idle export table")
            .insert(
                "pre_action_fade_seconds".to_string(),
                Toml::Float(self.pre_action_fade_seconds),
            );
        out
    }
}

pub(crate) fn export_idle_section_from_merged(merged: &Toml) -> Toml {
    merged
        .get("idle")
        .map(|section| IdleExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| IdleExport::export_defaults().write_export_table())
}

/// Overlay merged leaves onto `defaults` when the key exists in defaults (one table level).
fn overlay_known_leaves(defaults: &Toml, merged: &Toml) -> Toml {
    let default_table = match defaults.as_table() {
        Some(table) => table,
        None => return defaults.clone(),
    };
    let merged_table = match merged.as_table() {
        Some(table) => table,
        None => return defaults.clone(),
    };
    let mut out = default_table.clone();
    for (key, merged_value) in merged_table {
        let default_value = match default_table.get(key) {
            Some(value) => value,
            None => continue,
        };
        let merged_leaf = match (default_value, merged_value) {
            (Toml::Table(_), Toml::Table(_)) => overlay_known_leaves(default_value, merged_value),
            (Toml::Float(_), Toml::Float(value)) => Toml::Float(f32_bits(*value)),
            (Toml::Float(_), Toml::Integer(value)) => Toml::Float(f32_bits(*value as f64)),
            _ => merged_value.clone(),
        };
        out.insert(key.clone(), merged_leaf);
    }
    Toml::Table(out)
}

fn export_overlay_section_from_merged(
    section: &str,
    merged: &Toml,
    write_defaults: fn() -> Toml,
) -> Toml {
    let defaults = write_defaults();
    match merged.get(section) {
        Some(merged_section) => {
            let overlay = merged_overlay_for_parsed_section(section, merged_section);
            overlay_known_leaves(&defaults, &overlay)
        }
        None => defaults,
    }
}

fn overlay_bool_subtable_from_merged(
    defaults: &toml::map::Map<String, Toml>,
    merged: Option<&Toml>,
) -> toml::map::Map<String, Toml> {
    let mut out = defaults.clone();
    let merged_table = match merged.and_then(Toml::as_table) {
        Some(table) => table,
        None => return out,
    };
    for (key, value) in merged_table {
        if defaults.contains_key(key) && value.as_bool().is_some() {
            out.insert(key.clone(), value.clone());
        }
    }
    out
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct NotificationExport {
    background_opacity: f64,
    border: bool,
    collapse_on_dismiss: bool,
    enable_daemon: bool,
    filter_replace: bool,
    history_retention_hours: i64,
    keep_dismissed_in_history: bool,
    layer: String,
    max_visible: i64,
    monitors: Vec<String>,
    offset_x: i64,
    offset_y: i64,
    position: String,
    scale: f64,
    show_actions: bool,
    show_app_name: bool,
}

impl NotificationExport {
    fn export_defaults() -> Self {
        Self::from_export_table(&write_notification_defaults())
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("notification export table");
        Self {
            background_opacity: table
                .get("background_opacity")
                .and_then(|v| v.as_float())
                .map(f32_bits)
                .unwrap_or(0.97),
            border: table.get("border").and_then(Toml::as_bool).unwrap_or(true),
            collapse_on_dismiss: table
                .get("collapse_on_dismiss")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
            enable_daemon: table
                .get("enable_daemon")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
            filter_replace: table
                .get("filter_replace")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
            history_retention_hours: table
                .get("history_retention_hours")
                .and_then(Toml::as_integer)
                .unwrap_or(0),
            keep_dismissed_in_history: table
                .get("keep_dismissed_in_history")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
            layer: table
                .get("layer")
                .and_then(Toml::as_str)
                .unwrap_or("top")
                .to_string(),
            max_visible: table
                .get("max_visible")
                .and_then(Toml::as_integer)
                .unwrap_or(0),
            monitors: string_array(table.get("monitors")),
            offset_x: table.get("offset_x").and_then(Toml::as_integer).unwrap_or(20),
            offset_y: table.get("offset_y").and_then(Toml::as_integer).unwrap_or(8),
            position: table
                .get("position")
                .and_then(Toml::as_str)
                .unwrap_or("top_right")
                .to_string(),
            scale: table
                .get("scale")
                .and_then(|v| v.as_float())
                .map(f32_bits)
                .unwrap_or(1.0),
            show_actions: table
                .get("show_actions")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
            show_app_name: table
                .get("show_app_name")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
        }
    }

    fn from_merged_section(merged_section: &Toml) -> Self {
        let mut out = Self::export_defaults();
        let table = match merged_section.as_table() {
            Some(table) => table,
            None => return out,
        };
        if let Some(value) = table.get("background_opacity").and_then(|v| v.as_float()) {
            out.background_opacity = f32_bits(value);
        }
        if let Some(value) = table.get("border").and_then(Toml::as_bool) {
            out.border = value;
        }
        if let Some(value) = table.get("collapse_on_dismiss").and_then(Toml::as_bool) {
            out.collapse_on_dismiss = value;
        }
        if let Some(value) = table.get("enable_daemon").and_then(Toml::as_bool) {
            out.enable_daemon = value;
        }
        if let Some(value) = table.get("filter_replace").and_then(Toml::as_bool) {
            out.filter_replace = value;
        }
        if let Some(value) = table
            .get("history_retention_hours")
            .and_then(Toml::as_integer)
        {
            out.history_retention_hours = value;
        }
        if let Some(value) = table
            .get("keep_dismissed_in_history")
            .and_then(Toml::as_bool)
        {
            out.keep_dismissed_in_history = value;
        }
        if let Some(value) = table.get("layer").and_then(Toml::as_str) {
            out.layer = value.to_string();
        }
        if let Some(value) = table.get("max_visible").and_then(Toml::as_integer) {
            out.max_visible = value;
        }
        if table.contains_key("monitors") {
            out.monitors = string_array(table.get("monitors"));
        }
        if let Some(value) = table.get("offset_x").and_then(Toml::as_integer) {
            out.offset_x = value;
        }
        if let Some(value) = table.get("offset_y").and_then(Toml::as_integer) {
            out.offset_y = value;
        }
        if let Some(value) = table.get("position").and_then(Toml::as_str) {
            out.position = value.to_string();
        }
        if let Some(value) = table.get("scale").and_then(|v| v.as_float()) {
            out.scale = f32_bits(value);
        }
        if let Some(value) = table.get("show_actions").and_then(Toml::as_bool) {
            out.show_actions = value;
        }
        if let Some(value) = table.get("show_app_name").and_then(Toml::as_bool) {
            out.show_app_name = value;
        }
        out
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert(
            "background_opacity".to_string(),
            Toml::Float(self.background_opacity),
        );
        table.insert("border".to_string(), Toml::Boolean(self.border));
        table.insert(
            "collapse_on_dismiss".to_string(),
            Toml::Boolean(self.collapse_on_dismiss),
        );
        table.insert("enable_daemon".to_string(), Toml::Boolean(self.enable_daemon));
        table.insert(
            "filter_replace".to_string(),
            Toml::Boolean(self.filter_replace),
        );
        table.insert(
            "history_retention_hours".to_string(),
            Toml::Integer(self.history_retention_hours),
        );
        table.insert(
            "keep_dismissed_in_history".to_string(),
            Toml::Boolean(self.keep_dismissed_in_history),
        );
        table.insert("layer".to_string(), Toml::String(self.layer.clone()));
        table.insert("max_visible".to_string(), Toml::Integer(self.max_visible));
        table.insert(
            "monitors".to_string(),
            Toml::Array(
                self.monitors
                    .iter()
                    .map(|name| Toml::String(name.clone()))
                    .collect(),
            ),
        );
        table.insert("offset_x".to_string(), Toml::Integer(self.offset_x));
        table.insert("offset_y".to_string(), Toml::Integer(self.offset_y));
        table.insert("position".to_string(), Toml::String(self.position.clone()));
        table.insert("scale".to_string(), Toml::Float(self.scale));
        table.insert("show_actions".to_string(), Toml::Boolean(self.show_actions));
        table.insert("show_app_name".to_string(), Toml::Boolean(self.show_app_name));
        Toml::Table(table)
    }
}

pub(crate) fn export_notification_section_from_merged(merged: &Toml) -> Toml {
    merged
        .get("notification")
        .map(|section| NotificationExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| NotificationExport::export_defaults().write_export_table())
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct OsdExport {
    background_opacity: f64,
    border: bool,
    enabled: bool,
    monitors: Vec<String>,
    offset_x: i64,
    offset_y: i64,
    orientation: String,
    position: String,
    position_vertical: String,
    scale: f64,
    kinds: toml::map::Map<String, Toml>,
}

impl OsdExport {
    fn export_defaults() -> Self {
        Self::from_export_table(&write_osd_defaults())
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("osd export table");
        let kinds_defaults = table
            .get("kinds")
            .and_then(Toml::as_table)
            .cloned()
            .unwrap_or_default();
        Self {
            background_opacity: table
                .get("background_opacity")
                .and_then(|v| v.as_float())
                .map(f32_bits)
                .unwrap_or(0.97),
            border: table.get("border").and_then(Toml::as_bool).unwrap_or(true),
            enabled: table.get("enabled").and_then(Toml::as_bool).unwrap_or(true),
            monitors: string_array(table.get("monitors")),
            offset_x: table.get("offset_x").and_then(Toml::as_integer).unwrap_or(20),
            offset_y: table.get("offset_y").and_then(Toml::as_integer).unwrap_or(8),
            orientation: table
                .get("orientation")
                .and_then(Toml::as_str)
                .unwrap_or("horizontal")
                .to_string(),
            position: table
                .get("position")
                .and_then(Toml::as_str)
                .unwrap_or("top_center")
                .to_string(),
            position_vertical: table
                .get("position_vertical")
                .and_then(Toml::as_str)
                .unwrap_or("top_center")
                .to_string(),
            scale: table
                .get("scale")
                .and_then(|v| v.as_float())
                .map(f32_bits)
                .unwrap_or(1.0),
            kinds: kinds_defaults,
        }
    }

    fn from_merged_section(merged_section: &Toml) -> Self {
        let mut out = Self::export_defaults();
        let table = match merged_section.as_table() {
            Some(table) => table,
            None => return out,
        };
        if let Some(value) = table.get("background_opacity").and_then(|v| v.as_float()) {
            out.background_opacity = f32_bits(value);
        }
        if let Some(value) = table.get("border").and_then(Toml::as_bool) {
            out.border = value;
        }
        if let Some(value) = table.get("enabled").and_then(Toml::as_bool) {
            out.enabled = value;
        }
        if table.contains_key("monitors") {
            out.monitors = string_array(table.get("monitors"));
        }
        if let Some(value) = table.get("offset_x").and_then(Toml::as_integer) {
            out.offset_x = value;
        }
        if let Some(value) = table.get("offset_y").and_then(Toml::as_integer) {
            out.offset_y = value;
        }
        if let Some(value) = table.get("orientation").and_then(Toml::as_str) {
            out.orientation = value.to_string();
        }
        if let Some(value) = table.get("position").and_then(Toml::as_str) {
            out.position = value.to_string();
        }
        if let Some(value) = table.get("position_vertical").and_then(Toml::as_str) {
            out.position_vertical = value.to_string();
        }
        if let Some(value) = table.get("scale").and_then(|v| v.as_float()) {
            out.scale = f32_bits(value);
        }
        out.kinds = overlay_bool_subtable_from_merged(&out.kinds, table.get("kinds"));
        out
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert(
            "background_opacity".to_string(),
            Toml::Float(self.background_opacity),
        );
        table.insert("border".to_string(), Toml::Boolean(self.border));
        table.insert("enabled".to_string(), Toml::Boolean(self.enabled));
        table.insert(
            "monitors".to_string(),
            Toml::Array(
                self.monitors
                    .iter()
                    .map(|name| Toml::String(name.clone()))
                    .collect(),
            ),
        );
        table.insert("offset_x".to_string(), Toml::Integer(self.offset_x));
        table.insert("offset_y".to_string(), Toml::Integer(self.offset_y));
        table.insert("orientation".to_string(), Toml::String(self.orientation.clone()));
        table.insert("position".to_string(), Toml::String(self.position.clone()));
        table.insert(
            "position_vertical".to_string(),
            Toml::String(self.position_vertical.clone()),
        );
        table.insert("scale".to_string(), Toml::Float(self.scale));
        table.insert("kinds".to_string(), Toml::Table(self.kinds.clone()));
        Toml::Table(table)
    }
}

pub(crate) fn export_osd_section_from_merged(merged: &Toml) -> Toml {
    merged
        .get("osd")
        .map(|section| OsdExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| OsdExport::export_defaults().write_export_table())
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct LockscreenExport {
    allow_empty_password: bool,
    blur_intensity: f64,
    blurred_desktop: bool,
    enabled: bool,
    fingerprint: bool,
    lid_close_keep_external_displays: bool,
    lock_before_suspend: bool,
    monitors: Vec<String>,
    tint_intensity: f64,
    wallpaper: String,
}

impl LockscreenExport {
    fn export_defaults() -> Self {
        Self::from_export_table(&write_lockscreen_defaults())
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("lockscreen export table");
        Self {
            allow_empty_password: table
                .get("allow_empty_password")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
            blur_intensity: table
                .get("blur_intensity")
                .and_then(|v| v.as_float())
                .map(f32_bits)
                .unwrap_or(0.5),
            blurred_desktop: table
                .get("blurred_desktop")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
            enabled: table.get("enabled").and_then(Toml::as_bool).unwrap_or(true),
            fingerprint: table
                .get("fingerprint")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
            lid_close_keep_external_displays: table
                .get("lid_close_keep_external_displays")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
            lock_before_suspend: table
                .get("lock_before_suspend")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
            monitors: string_array(table.get("monitors")),
            tint_intensity: table
                .get("tint_intensity")
                .and_then(|v| v.as_float())
                .map(f32_bits)
                .unwrap_or(0.3),
            wallpaper: table
                .get("wallpaper")
                .and_then(Toml::as_str)
                .unwrap_or("")
                .to_string(),
        }
    }

    fn from_merged_section(merged_section: &Toml) -> Self {
        let mut out = Self::export_defaults();
        let table = match merged_section.as_table() {
            Some(table) => table,
            None => return out,
        };
        if let Some(value) = table.get("allow_empty_password").and_then(Toml::as_bool) {
            out.allow_empty_password = value;
        }
        if let Some(value) = table.get("blur_intensity").and_then(|v| v.as_float()) {
            out.blur_intensity = f32_bits(value);
        }
        if let Some(value) = table.get("blurred_desktop").and_then(Toml::as_bool) {
            out.blurred_desktop = value;
        }
        if let Some(value) = table.get("enabled").and_then(Toml::as_bool) {
            out.enabled = value;
        }
        if let Some(value) = table.get("fingerprint").and_then(Toml::as_bool) {
            out.fingerprint = value;
        }
        if let Some(value) = table
            .get("lid_close_keep_external_displays")
            .and_then(Toml::as_bool)
        {
            out.lid_close_keep_external_displays = value;
        }
        if let Some(value) = table.get("lock_before_suspend").and_then(Toml::as_bool) {
            out.lock_before_suspend = value;
        }
        if table.contains_key("monitors") {
            out.monitors = string_array(table.get("monitors"));
        }
        if let Some(value) = table.get("tint_intensity").and_then(|v| v.as_float()) {
            out.tint_intensity = f32_bits(value);
        }
        if let Some(value) = table.get("wallpaper").and_then(Toml::as_str) {
            out.wallpaper = value.to_string();
        }
        out
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert(
            "allow_empty_password".to_string(),
            Toml::Boolean(self.allow_empty_password),
        );
        table.insert("blur_intensity".to_string(), Toml::Float(self.blur_intensity));
        table.insert(
            "blurred_desktop".to_string(),
            Toml::Boolean(self.blurred_desktop),
        );
        table.insert("enabled".to_string(), Toml::Boolean(self.enabled));
        table.insert("fingerprint".to_string(), Toml::Boolean(self.fingerprint));
        table.insert(
            "lid_close_keep_external_displays".to_string(),
            Toml::Boolean(self.lid_close_keep_external_displays),
        );
        table.insert(
            "lock_before_suspend".to_string(),
            Toml::Boolean(self.lock_before_suspend),
        );
        table.insert(
            "monitors".to_string(),
            Toml::Array(
                self.monitors
                    .iter()
                    .map(|name| Toml::String(name.clone()))
                    .collect(),
            ),
        );
        table.insert("tint_intensity".to_string(), Toml::Float(self.tint_intensity));
        table.insert("wallpaper".to_string(), Toml::String(self.wallpaper.clone()));
        Toml::Table(table)
    }
}

pub(crate) fn export_lockscreen_section_from_merged(merged: &Toml) -> Toml {
    merged
        .get("lockscreen")
        .map(|section| LockscreenExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| LockscreenExport::export_defaults().write_export_table())
}

pub(crate) fn export_system_section_from_merged(merged: &Toml) -> Toml {
    export_overlay_section_from_merged("system", merged, write_system_defaults)
}

pub(crate) fn export_control_center_section_from_merged(merged: &Toml) -> Toml {
    export_overlay_section_from_merged("control_center", merged, write_control_center_defaults)
}

pub(crate) fn export_dock_section_from_merged(merged: &Toml) -> Toml {
    export_overlay_section_from_merged("dock", merged, write_dock_defaults)
}

pub(crate) fn export_wallpaper_section_from_merged(merged: &Toml) -> Toml {
    export_overlay_section_from_merged("wallpaper", merged, write_wallpaper_defaults)
}

pub(crate) fn export_keybinds_section_from_merged(merged: &Toml) -> Toml {
    export_overlay_section_from_merged("keybinds", merged, write_keybinds_defaults)
}

pub(crate) fn export_hooks_section_from_merged(merged: &Toml) -> Toml {
    let mut out = write_hooks_defaults();
    let out_table = out.as_table_mut().expect("hooks export table");
    let merged_table = match merged.get("hooks").and_then(Toml::as_table) {
        Some(table) => table,
        None => return out,
    };
    for key in HOOK_EVENT_KEYS {
        if let Some(value) = merged_table.get(*key).and_then(Toml::as_array) {
            out_table.insert(key.to_string(), Toml::Array(value.clone()));
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use super::super::config_export_rust::export_filtered_schema_section;
    use super::super::config_export_serialize::serialize_default_config;

    #[test]
    fn scalar_exports_match_default_writers() {
        assert_eq!(
            export_hooks_section_from_merged(&Toml::Table(toml::map::Map::new())),
            write_hooks_defaults()
        );
        assert_eq!(
            CalendarExport::export_defaults().write_export_table(),
            write_calendar_defaults()
        );
        assert_eq!(
            NightlightExport::export_defaults().write_export_table(),
            write_nightlight_defaults()
        );
        assert_eq!(
            AudioExport::export_defaults().write_export_table(),
            write_audio_defaults()
        );
        assert_eq!(
            BackdropExport::export_defaults().write_export_table(),
            write_backdrop_defaults()
        );
        assert_eq!(
            BrightnessExport::export_defaults().write_export_table(),
            write_brightness_defaults()
        );
        assert_eq!(
            HotCornersExport::export_defaults().write_export_table(),
            write_hot_corners_defaults()
        );
        assert_eq!(
            export_idle_section_from_merged(&Toml::Table(toml::map::Map::new())),
            write_idle_defaults()
        );
        assert_eq!(
            NotificationExport::export_defaults().write_export_table(),
            write_notification_defaults()
        );
        assert_eq!(
            OsdExport::export_defaults().write_export_table(),
            write_osd_defaults()
        );
        assert_eq!(
            LockscreenExport::export_defaults().write_export_table(),
            write_lockscreen_defaults()
        );
        assert_eq!(
            export_system_section_from_merged(&Toml::Table(toml::map::Map::new())),
            write_system_defaults()
        );
        assert_eq!(
            export_control_center_section_from_merged(&Toml::Table(toml::map::Map::new())),
            write_control_center_defaults()
        );
        assert_eq!(
            export_dock_section_from_merged(&Toml::Table(toml::map::Map::new())),
            write_dock_defaults()
        );
        assert_eq!(
            export_wallpaper_section_from_merged(&Toml::Table(toml::map::Map::new())),
            write_wallpaper_defaults()
        );
        assert_eq!(
            export_keybinds_section_from_merged(&Toml::Table(toml::map::Map::new())),
            write_keybinds_defaults()
        );
    }

    #[test]
    fn overlay_sections_match_filtered_merge() {
        let default = serialize_default_config();
        let merged: Toml = toml::from_str(
            r#"
[system.monitor]
cpu_poll_seconds = 5.0
enabled = false

[control_center]
height = 800

[control_center.network]
wifi_band = "5ghz"

[dock]
enabled = false
icon_size = 64

[wallpaper]
enabled = false
default = "/tmp/w.png"

[wallpaper.automation]
enabled = true

[keybinds]
copy = ["Ctrl+Shift+c"]
"#,
        )
        .unwrap();
        let system_filtered =
            export_filtered_schema_section("system", &merged, &default, write_system_defaults);
        assert_eq!(export_system_section_from_merged(&merged), system_filtered);
        let cc_filtered = export_filtered_schema_section(
            "control_center",
            &merged,
            &default,
            write_control_center_defaults,
        );
        assert_eq!(
            export_control_center_section_from_merged(&merged),
            cc_filtered
        );
        let dock_filtered =
            export_filtered_schema_section("dock", &merged, &default, write_dock_defaults);
        assert_eq!(export_dock_section_from_merged(&merged), dock_filtered);
        let wallpaper_filtered = export_filtered_schema_section(
            "wallpaper",
            &merged,
            &default,
            write_wallpaper_defaults,
        );
        assert_eq!(export_wallpaper_section_from_merged(&merged), wallpaper_filtered);
        let keybinds_filtered = export_filtered_schema_section(
            "keybinds",
            &merged,
            &default,
            write_keybinds_defaults,
        );
        assert_eq!(export_keybinds_section_from_merged(&merged), keybinds_filtered);
    }

    #[test]
    fn scalar_exports_match_filtered_merge_for_hooks_and_nightlight() {
        let default = serialize_default_config();
        let merged: Toml = toml::from_str(
            r#"
[hooks]

[nightlight]
enabled = true
"#,
        )
        .unwrap();
        let hooks_filtered =
            export_filtered_schema_section("hooks", &merged, &default, write_hooks_defaults);
        assert_eq!(export_hooks_section_from_merged(&merged), hooks_filtered);
        let nightlight_filtered = export_filtered_schema_section(
            "nightlight",
            &merged,
            &default,
            write_nightlight_defaults,
        );
        assert_eq!(
            export_nightlight_section_from_merged(&merged),
            nightlight_filtered
        );
    }
}
