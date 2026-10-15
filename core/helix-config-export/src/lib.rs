//! Effective config export: turns the settings catalog and a config file into the TOML the shell consumes.

pub mod config_export;
mod config_export_defaults;
mod config_export_key_chord;
mod config_export_rust;
mod config_export_serialize;
mod config_export_typed;
mod config_export_typed_scalar;
mod config_export_typed_shell;

// Modules that live in other crates but are named `crate::<module>` throughout this crate.
pub use helix_config::settings;
