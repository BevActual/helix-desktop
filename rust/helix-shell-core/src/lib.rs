//! State machines the shell UI shows. Nothing here opens a window or a bus connection.

mod assistant;
pub mod audio;
mod confirm;
mod display;
pub mod launcher;
pub mod shell_surface;
pub mod surfaces;

pub use assistant::{Phase, Session, Spawn, View};
pub use confirm::{Confirmation, ARMING_MS};
pub use display::sanitize_for_display;
pub use shell_surface::{Escape, InputRegion, Plan, ShellSurface, BAR_HEIGHT};
