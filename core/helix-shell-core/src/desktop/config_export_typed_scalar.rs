//! Small schema sections with explicit parse → `writeTable` exporters.

use toml::Value as Toml;

use super::config_export_defaults::{
    write_calendar_defaults, write_hooks_defaults, write_location_defaults,
    write_nightlight_defaults, write_tailscale_defaults, write_weather_defaults, HOOK_EVENT_KEYS,
};

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
