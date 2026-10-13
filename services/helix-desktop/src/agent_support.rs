//! Shared Agent Surface helpers (audit, dbus signals, JSON result shape).

use std::path::Path;

use helix_shell_core::{AuditRecord, RiskTier};
use serde_json::json;
use zbus::names::{BusName, UniqueName};
use zbus::Connection;

use crate::state::{PendingConfirm, State};

pub const OBJECT_PATH: &str = "/org/helix/Desktop";
const ACTIVITY_INTERFACE: &str = "org.helix.Desktop.AgentSurfaceActivity1";

pub fn is_helix_settings(exe: &str) -> bool {
    let path = Path::new(exe);
    if path == Path::new("/usr/local/bin/helix-settings")
        || path == Path::new("/usr/bin/helix-settings")
    {
        return true;
    }
    match std::env::var("HELIX_MANAGER_EXE") {
        Ok(extra) if !extra.is_empty() => path == Path::new(&extra),
        _ => false,
    }
}

pub fn denial_json(reason: &str) -> String {
    json!({"trust": "trusted", "error": reason}).to_string()
}

pub fn action_result_json(trust: &str, ok: bool, body: &str) -> String {
    let mut value = json!({"trust": trust});
    if ok {
        value["content"] = serde_json::Value::String(body.to_string());
    } else {
        value["error"] = serde_json::Value::String(body.to_string());
    }
    value.to_string()
}

pub fn require_registered_desktop(state: &State, sender: &str) -> zbus::fdo::Result<()> {
    match &state.desktop {
        Some(owner) if owner == sender => Ok(()),
        _ => Err(zbus::fdo::Error::AccessDenied(
            "Only the registered shell may confirm agent actions".into(),
        )),
    }
}

fn bus_destination(name: Option<&str>) -> Option<BusName<'_>> {
    name.and_then(|text| UniqueName::try_from(text).ok().map(BusName::from))
}

pub async fn emit_activity_changed(conn: &Connection, _desktop: Option<&str>) {
    let _ = conn.emit_signal(
        None::<&str>,
        OBJECT_PATH,
        ACTIVITY_INTERFACE,
        "Changed",
        &(),
    );
}

pub async fn emit_activity_pending_changed(conn: &Connection, desktop: Option<&str>) {
    let destination = bus_destination(desktop);
    let _ = conn.emit_signal(
        destination.as_deref(),
        OBJECT_PATH,
        ACTIVITY_INTERFACE,
        "PendingChanged",
        &(),
    );
}

pub async fn append_audit(state: &mut State, record: AuditRecord) {
    if let Err(error) = state.audit.append(&record) {
        tracing::warn!(%error, "audit append failed");
    }
}

pub fn grant_change_record(client: &str, outcome: &str, tiers: &[String]) -> AuditRecord {
    AuditRecord {
        client: client.to_string(),
        tool: "grant-change".into(),
        args: tiers.to_vec(),
        tier: RiskTier::Confirm,
        decision: "changed".into(),
        outcome: outcome.into(),
        tainted: false,
    }
}

pub fn audit_record(
    pending: &PendingConfirm,
    tier: RiskTier,
    decision: &str,
    outcome: &str,
) -> AuditRecord {
    let args = pending
        .argv
        .iter()
        .chain(pending.literal.iter())
        .cloned()
        .collect();
    AuditRecord {
        client: pending.client.clone(),
        tool: if pending.tool.is_empty() {
            "grant-request".into()
        } else {
            pending.tool.clone()
        },
        args,
        tier,
        decision: decision.into(),
        outcome: outcome.into(),
        tainted: pending.tainted,
    }
}

pub async fn finish_pending_denied(state: &mut State, pending: PendingConfirm, operation: u64, reason: &str) {
    let record = audit_record(&pending, RiskTier::Confirm, "cancelled", "stopped");
    append_audit(state, record).await;
    let conn = state.bus.as_ref().expect("bus connected");
    crate::agent::Agent::notify_confirmation_ended(conn, operation).await;
    crate::agent_activity::AgentActivity::notify_changed(conn).await;
    crate::agent::Agent::notify_completed(conn, operation, false, &denial_json(reason)).await;
    crate::agent_activity::AgentActivity::notify_pending_changed(conn).await;
}

pub async fn confirmer_disconnected(state: &mut State) {
    state.confirmer = None;
    let waiting: Vec<(u64, PendingConfirm)> = state.pending.drain().collect();
    let unavailable = denial_json("the confirmation dialog is unavailable");
    let conn = state.bus.clone();
    let desktop = state.desktop.clone();
    for (operation, pending) in waiting {
        let record = audit_record(&pending, RiskTier::Confirm, "denied", "confirmer disconnected");
        if let Some(conn) = &conn {
            if let Err(error) = state.audit.append(&record) {
                tracing::warn!(%error, "audit append failed");
            }
            crate::agent::Agent::notify_confirmation_ended(conn, operation).await;
            crate::agent::Agent::notify_completed(conn, operation, false, &unavailable).await;
        }
    }
    if let Some(conn) = &conn {
        emit_activity_changed(conn, desktop.as_deref()).await;
        emit_activity_pending_changed(conn, desktop.as_deref()).await;
    }
}
