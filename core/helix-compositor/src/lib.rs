//! What the shell knows about the compositor: its config and IPC, window facts, capture, and the agent desk.

pub mod agent_desk;
pub mod capture;
pub mod compositor_config;
pub mod compositor_ipc;
pub mod niri;
pub mod shortcuts;

// Modules that live in other crates but are named `crate::<module>` throughout this crate.
pub use helix_base::display;
