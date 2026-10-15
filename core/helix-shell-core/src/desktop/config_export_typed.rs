//! Typed `parseConfigTable` → `writeTable` slices for `config_export` (milestone C tail).
//!
//! Sections move here incrementally; until a section is ported, `config_export_rust` keeps
//! using `merge_export_subtree` on `write_*_defaults`.

use toml::Value as Toml;

use super::config_export_defaults::{
    write_accessibility_defaults, write_audio_defaults, write_backdrop_defaults,
    write_battery_defaults, write_brightness_defaults, write_location_defaults,
    write_nightlight_defaults, write_storage_defaults, write_tailscale_defaults,
    write_weather_defaults,
};
use super::config_export_rust::export_flat_schema_section;

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

/// Ported schema sections (flat or explicit struct). `None` → merge path in `config_export_rust`.
pub(crate) fn export_typed_schema_section(
    section: &str,
    merged: &Toml,
    default: &Toml,
) -> Option<Toml> {
    if section == "storage" {
        return Some(export_storage_section_from_merged(merged));
    }
    let writer = match section {
        "accessibility" => write_accessibility_defaults,
        "audio" => write_audio_defaults,
        "backdrop" => write_backdrop_defaults,
        "battery" => write_battery_defaults,
        "brightness" => write_brightness_defaults,
        "location" => write_location_defaults,
        "nightlight" => write_nightlight_defaults,
        "tailscale" => write_tailscale_defaults,
        "weather" => write_weather_defaults,
        _ => return None,
    };
    Some(export_flat_schema_section(section, merged, default, writer))
}

#[cfg(test)]
mod tests {
    use super::*;
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
    fn flat_typed_sections_match_default_writers_without_merged_overlay() {
        let default = serialize_default_config();
        let empty = Toml::Table(toml::map::Map::new());
        let cases: [(&str, fn() -> Toml)] = [
            ("accessibility", write_accessibility_defaults),
            ("audio", write_audio_defaults),
            ("backdrop", write_backdrop_defaults),
            ("battery", write_battery_defaults),
            ("brightness", write_brightness_defaults),
            ("location", write_location_defaults),
            ("nightlight", write_nightlight_defaults),
            ("storage", write_storage_defaults),
            ("tailscale", write_tailscale_defaults),
            ("weather", write_weather_defaults),
        ];
        for (section, writer) in cases {
            let got = export_typed_schema_section(section, &empty, &default)
                .unwrap_or_else(|| panic!("section {section} should be typed"));
            assert_eq!(got, writer(), "section {section}");
        }
    }
}
