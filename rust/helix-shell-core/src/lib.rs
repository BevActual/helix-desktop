//! State machines the shell UI shows. Nothing here opens a window or a bus connection.

pub mod activity;
pub mod agent_confirm;
mod assistant;
pub mod audio;
pub mod bluetooth;
pub mod brightness;
mod confirm;
pub mod confirmer;
mod display;
pub mod dock;
pub mod glass;
pub mod launcher;
pub mod network;
pub mod niri;
pub mod notifications;
pub mod power;
pub mod quick;
pub mod quick_insert;
pub mod shell_surface;
pub mod surfaces;
pub mod tray;

pub use agent_confirm::{ConfirmRequest, ConfirmationQueue, Kind as ConfirmKind};
pub use assistant::{Phase, Session, Spawn, View};
pub use confirm::{Confirmation, ARMING_MS};
pub use confirmer::{Confirmer, ConfirmerView, TierView};
pub use display::{friendly_client_name, sanitize_for_display};
pub use shell_surface::{Escape, InputRegion, Plan, ShellSurface, BAR_HEIGHT};
