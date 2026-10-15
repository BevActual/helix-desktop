//! Facade over the Helix core crates. The code lives in `helix-base`, `helix-agent`, `helix-config`, `helix-config-export`,
//! `helix-models`, `helix-compositor`, `helix-shell-model`, `helix-chat`, `helix-software-model` and `helix-monitor-model`; this crate keeps
//! the old paths (`helix_shell_core::audio`, `helix_shell_core::desktop::settings`, ...) working. New code should depend on the crate it
//! uses (docs/design/shell-core-split.md).

/// Same paths as before the split; each module now lives in its own crate.
pub mod agent {
    pub use helix_agent::{activity, agent_confirm, agent_msg, agent_status, approvals, assistant_core, audit_log, confirm, confirmer, grant_store, grants, peer, risk, shell_ipc, tool_schema};
    pub use helix_base::{display};
}
/// Same paths as before the split; each module now lives in its own crate.
pub mod apps {
    pub use helix_agent::{assistant};
    pub use helix_chat::{chat, markdown, mobius_lab, model_catalog, personas};
    pub use helix_config::{inference_routing};
    pub use helix_monitor_model::{monitor};
    pub use helix_software_model::{software};
}

pub use helix_base::nested_test_session;

/// Same paths as before the split; each module now lives in its own crate.
pub mod desktop {
    pub use helix_compositor::{capture, compositor_config, compositor_ipc, shortcuts};
    pub use helix_config::{catalog, color_spec, control_center_shortcuts, dock_prefs, dynamic_desktop_catalog, idle_prefs, keybinds, notification_rules, ordered_catalog, ordered_lists, palette_names, settings};
    pub use helix_config_export::{config_export};
    pub use helix_models::{audio, bluetooth, brightness, network, pairing, power, quick, wallhaven};
}
/// Same paths as before the split; each module now lives in its own crate.
pub mod shell {
    pub use helix_compositor::{agent_desk, niri};
    pub use helix_shell_model::{bar_surface, bar_visibility, calendar, dbusmenu, desktop_actions, dock, glass, identity, island, launcher, notifications, panel_surface, panels_ipc, quick_insert, shell_surface, surfaces, tray};
}

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
