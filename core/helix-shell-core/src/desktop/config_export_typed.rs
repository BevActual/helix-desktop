//! Typed `parseConfigTable` → `writeTable` slices for `config_export` (milestone C tail).
//!
//! Every `SCHEMA_EXPORT_SECTIONS` table is exported here (`storage` uses an explicit struct;
//! others use filtered merge until ported field-by-field). `bar` stays in `config_export_rust`.

use toml::Value as Toml;

use super::config_export_defaults::{schema_section_default_writer, write_storage_defaults};
use super::config_export_rust::export_filtered_schema_section;

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

/// Export one schema section. `bar` is handled only in `config_export_rust`.
pub(crate) fn export_typed_schema_section(
    section: &str,
    merged: &Toml,
    default: &Toml,
) -> Option<Toml> {
    if section == "storage" {
        return Some(export_storage_section_from_merged(merged));
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
