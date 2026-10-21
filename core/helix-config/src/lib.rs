//! The settings catalog, ordered lists, shell preferences and config migration. Pure data and validation.

pub mod catalog;
pub mod clock_prefs;
pub mod color_spec;
mod config_merge;
mod config_settings_migrate;
pub mod control_center_shortcuts;
pub mod dock_prefs;
pub mod dynamic_desktop_catalog;
pub mod idle_prefs;
pub mod inference_routing;
pub mod keybinds;
pub mod notification_prefs;
pub mod notification_rules;
pub mod ordered_catalog;
pub mod ordered_lists;
pub mod palette_names;
pub mod settings;
mod settings_defaults_generated;

// Modules that live in other crates but are named `crate::<module>` throughout this crate.
pub use helix_base::display;
