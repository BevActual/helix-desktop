//! State machines the shell UI shows. Nothing here opens a window or a bus connection.
//!
//! Modules are grouped by domain (`shell`, `desktop`, `agent`, `apps`). Public paths at the
//! crate root stay stable (`helix_shell_core::audio`, …) so callers do not churn.

pub mod agent;
pub mod apps;

/// True inside a nested test session (`tools/nested/run.sh` sets `HELIX_NESTED_INNER`). The test session has its own bus and compositor but
/// shares the real user's systemd and login manager, so anything that would reach those (start or restart a unit, suspend, power off)
/// must not happen from it.
pub fn nested_test_session() -> bool {
    std::env::var_os("HELIX_NESTED_INNER").is_some()
}

pub mod desktop;
pub mod shell;

pub use agent::activity;
pub use agent::agent_confirm;
pub use agent::agent_msg;
pub use agent::agent_status;
pub use agent::approvals;
pub use agent::confirm;
pub use agent::confirmer;
pub use agent::display;
pub use agent::audit_log;
pub use agent::grant_store;
pub use agent::grants;
pub use agent::peer;
pub use agent::risk;
pub use peer::client_identity;
pub use audit_log::{AuditLog, AuditRecord};
pub use grant_store::{
    evaluate, result_is_untrusted, Decision, GrantStore, TaintTracker, Verdict,
};
pub use risk::{RiskPolicy, Tier as RiskTier};
pub use agent_confirm::{ConfirmRequest, ConfirmationQueue, Kind as ConfirmKind};
pub use apps::assistant;
pub use apps::chat;
pub use apps::inference_routing;
pub use apps::markdown;
pub use apps::mobius_lab;
pub use apps::model_catalog;
pub use apps::monitor;
pub use apps::personas;
pub use apps::software;
pub use assistant::{Phase, Session, Spawn, View};
pub use confirm::{Confirmation, ARMING_MS};
pub use confirmer::{Confirmer, ConfirmerView, TierView};
pub use display::{friendly_client_name, sanitize_for_display};
pub use desktop::audio;
pub use desktop::bluetooth;
pub use desktop::brightness;
pub use desktop::catalog;
pub use desktop::capture;
pub use desktop::compositor_config;
pub use desktop::compositor_ipc;
pub use agent::shell_ipc;
pub use desktop::settings;
pub use settings::{
    shell_polkit_agent_enabled, ConfigPaths, ConfigStore, BOOTSTRAP as SETTINGS_BOOTSTRAP,
};
pub use desktop::dock_prefs;
pub use desktop::idle_prefs;
pub use desktop::keybinds;
pub use desktop::network;
pub use desktop::pairing;
pub use desktop::power;
pub use desktop::quick;
pub use desktop::shortcuts;
pub use desktop::wallhaven;
pub use shell::agent_desk;
pub use shell::desktop_actions;
pub use shell::bar_surface;
pub use shell::bar_visibility;
pub use shell::calendar;
pub use shell::dbusmenu;
pub use shell::dock;
pub use shell::glass;
pub use shell::identity;
pub use shell::island;
pub use shell::launcher;
pub use shell::niri;
pub use shell::notifications;
pub use shell::panel_surface;
pub use shell::panels_ipc;
pub use shell::quick_insert;
pub use shell::shell_surface;
pub use shell::surfaces;
pub use shell::tray;
pub use bar_surface::{BarSurface, BAR_PLAN};
pub use panel_surface::{PanelSurface, PANEL_CLOSED, PANEL_OPEN};
pub use shell_surface::{Escape, InputRegion, Plan, ShellSurface, BAR_HEIGHT};
