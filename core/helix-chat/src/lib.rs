//! Mobius: conversations, personas, the lab, model catalog and markdown blocks.

pub mod chat;
pub mod markdown;
pub mod memory;
pub mod mobius_lab;
pub mod model_catalog;
pub mod personas;

// Modules that live in other crates but are named `crate::<module>` throughout this crate.
pub use helix_base::display;
pub use helix_config::inference_routing;
