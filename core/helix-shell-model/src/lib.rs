//! State machines the shell draws: bar and panel surfaces, dock, tray, launcher, notifications, island.

pub mod ask;
pub mod bar_surface;
pub mod bar_visibility;
pub mod calendar;
pub mod dbusmenu;
pub mod desktop_actions;
pub mod dock;
pub mod glass;
pub mod identity;
pub mod island;
pub mod launcher;
pub mod notifications;
pub mod panel_surface;
pub mod panels_ipc;
pub mod pointer;
pub mod privacy;
pub mod quick_insert;
pub mod shell_surface;
pub mod surfaces;
pub mod tray;
pub mod ui_scale;
pub mod writing;

// Modules that live in other crates but are named `crate::<module>` throughout this crate.
pub use helix_base::display;
pub use helix_compositor::agent_desk;
pub use helix_compositor::niri;
