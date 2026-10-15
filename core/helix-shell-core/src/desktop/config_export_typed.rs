//! Typed `parseConfigTable` → `writeTable` slices for `config_export` (milestone C tail).
//!
//! Sections move here incrementally; until a section is ported, `config_export_rust` keeps
//! using `merge_export_subtree` on `write_*_defaults`.

use toml::Value as Toml;

use super::config_export_defaults::write_storage_defaults;

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

pub(crate) fn export_storage_section_from_merged(merged: &Toml) -> Toml {
    let merged_section = merged
        .get("storage")
        .map(|section| StorageExport::from_merged_section(section).write_export_table())
        .unwrap_or_else(|| StorageExport::export_defaults().write_export_table());
    merged_section
}

#[cfg(test)]
mod tests {
    use super::*;

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
}
