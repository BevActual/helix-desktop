//! What agents did, what they are waiting to do, who holds access, and the kill switch, as a pure
//! model over `AgentActivity1`. Every field an agent can influence (client, tool, arguments, reason,
//! outcome) is sanitized before it reaches a view. This is the only producer of write requests:
//! an operation can be stopped only if the last snapshot listed it pending, access can be revoked
//! only from a client the last snapshot listed.

use serde::Serialize;
use serde_json::Value;

use crate::display::{friendly_client_name, sanitize_for_display};

pub const MAX_ROWS: usize = 40;
const TIERS: &[&str] = &["observe", "observe-private", "act", "confirm", "blocked"];

fn text(value: &Value, key: &str) -> String {
    value
        .get(key)
        .and_then(Value::as_str)
        .unwrap_or("")
        .to_string()
}

fn tier(value: &Value) -> String {
    let raw = text(value, "tier");
    // Sanitizing could turn "ob<bidi>serve" into a real tier name, so anything not exactly a tier is "unknown".
    if TIERS.contains(&raw.as_str()) {
        raw
    } else {
        "unknown".to_string()
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct Pending {
    operation: u64,
    client: String,
    tool: String,
    tier: String,
    grant: bool,
    tainted: bool,
    reason: String,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Activity {
    enabled: Option<bool>,
    pending: Vec<Pending>,
    grants: Vec<(String, Vec<String>)>,
    rows: Vec<Value>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Request {
    Stop(u64),
    StopAll,
    Revoke(String),
    SetEnabled(bool),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RequestError {
    /// The operation is not pending (it finished or was never listed).
    NotPending,
    UnknownClient,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct PendingRow {
    pub operation: u64,
    pub who: String,
    pub tool: String,
    pub tier: String,
    pub grant: bool,
    pub tainted: bool,
    pub reason: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct GrantRow {
    /// Opaque key handed back to [`Activity::revoke`]. Never shown.
    pub id: String,
    pub who: String,
    pub tiers: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct HistoryRow {
    pub time: String,
    pub who: String,
    pub tool: String,
    pub args: String,
    pub tier: String,
    pub decision: String,
    pub outcome: String,
    pub tainted: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct ActivityView {
    pub available: bool,
    pub enabled: bool,
    /// Something is waiting on a person: the bar shows the agent indicator.
    pub busy: bool,
    pub pending: Vec<PendingRow>,
    pub grants: Vec<GrantRow>,
    /// Newest first.
    pub history: Vec<HistoryRow>,
}

/// One audit record as the person reads it: every field an agent can influence is sanitized and bounded.
pub fn history_row(row: &Value) -> HistoryRow {
    let args = row
        .get("args")
        .and_then(Value::as_array)
        .map(|list| {
            list.iter()
                .filter_map(Value::as_str)
                .collect::<Vec<_>>()
                .join(" ")
        })
        .unwrap_or_default();
    HistoryRow {
        time: sanitize_for_display(&text(row, "time"), 24),
        who: friendly_client_name(&text(row, "client"), 40),
        tool: sanitize_for_display(&text(row, "tool"), 40),
        args: sanitize_for_display(&args, 120),
        tier: tier(row),
        decision: sanitize_for_display(&text(row, "decision"), 24),
        outcome: sanitize_for_display(&text(row, "outcome"), 80),
        tainted: row.get("tainted").and_then(Value::as_bool).unwrap_or(false),
    }
}

impl Activity {
    pub fn set_enabled(&mut self, enabled: bool) {
        self.enabled = Some(enabled);
    }

    pub fn set_pending(&mut self, rows: &Value) {
        self.pending = rows
            .as_array()
            .map(|rows| {
                rows.iter()
                    .filter_map(|row| {
                        Some(Pending {
                            operation: row.get("operation")?.as_u64()?,
                            client: text(row, "client"),
                            tool: text(row, "tool"),
                            tier: tier(row),
                            grant: text(row, "kind") == "grant",
                            tainted: row.get("tainted").and_then(Value::as_bool).unwrap_or(false),
                            reason: text(row, "reason"),
                        })
                    })
                    .collect()
            })
            .unwrap_or_default();
    }

    pub fn set_grants(&mut self, rows: &Value) {
        self.grants = rows
            .as_array()
            .map(|rows| {
                rows.iter()
                    .filter_map(|row| {
                        let client = row.get("client")?.as_str()?.to_string();
                        let tiers = row
                            .get("tiers")
                            .and_then(Value::as_array)
                            .map(|list| {
                                list.iter()
                                    .filter_map(Value::as_str)
                                    .map(|name| sanitize_for_display(name, 24))
                                    .collect()
                            })
                            .unwrap_or_default();
                        Some((client, tiers))
                    })
                    .collect()
            })
            .unwrap_or_default();
    }

    pub fn set_history(&mut self, rows: &Value) {
        self.rows = rows.as_array().cloned().unwrap_or_default();
    }

    pub fn view(&self) -> ActivityView {
        let history = self
            .rows
            .iter()
            .rev()
            .take(MAX_ROWS)
            .map(history_row)
            .collect();
        ActivityView {
            available: self.enabled.is_some(),
            enabled: self.enabled.unwrap_or(true),
            busy: !self.pending.is_empty(),
            pending: self
                .pending
                .iter()
                .map(|p| PendingRow {
                    operation: p.operation,
                    who: friendly_client_name(&p.client, 40),
                    tool: sanitize_for_display(&p.tool, 40),
                    tier: p.tier.clone(),
                    grant: p.grant,
                    tainted: p.tainted,
                    reason: sanitize_for_display(&p.reason, 160),
                })
                .collect(),
            grants: self
                .grants
                .iter()
                .map(|(client, tiers)| GrantRow {
                    id: client.clone(),
                    who: friendly_client_name(client, 40),
                    tiers: tiers.clone(),
                })
                .collect(),
            history,
        }
    }

    pub fn stop(&self, operation: u64) -> Result<Request, RequestError> {
        if self.pending.iter().any(|p| p.operation == operation) {
            Ok(Request::Stop(operation))
        } else {
            Err(RequestError::NotPending)
        }
    }

    /// Always allowed: a stale snapshot must never stop a person from stopping everything.
    pub fn stop_all(&self) -> Request {
        Request::StopAll
    }

    pub fn revoke(&self, client: &str) -> Result<Request, RequestError> {
        if self.grants.iter().any(|(known, _)| known == client) {
            Ok(Request::Revoke(client.to_string()))
        } else {
            Err(RequestError::UnknownClient)
        }
    }

    pub fn set_agent_enabled(&self, enabled: bool) -> Request {
        Request::SetEnabled(enabled)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn model() -> Activity {
        let mut a = Activity::default();
        a.set_enabled(true);
        a.set_pending(&json!([
            {"operation": 7, "client": "exe:/usr/bin/helix-assistant", "tool": "app-launch",
             "tier": "confirm", "kind": "action", "tainted": true, "reason": "open\u{202e}gnp.exe the browser"},
            {"operation": 9, "client": "x", "tool": "grant-request", "tier": "confirm", "kind": "grant", "tainted": false}
        ]));
        a.set_grants(&json!([
            {"client": "exe:/usr/bin/helix-assistant", "tiers": ["observe", "act"]},
            {"client": "exe:/opt/other", "tiers": []}
        ]));
        a.set_history(&json!([
            {"time": "2026-10-07T01:00:00Z", "client": "exe:/usr/bin/helix-assistant", "tool": "volume",
             "args": ["set", "30"], "tier": "act", "decision": "allowed", "outcome": "ok", "tainted": false},
            {"time": "2026-10-07T01:00:05Z", "client": "exe:/usr/bin/helix-assistant", "tool": "app-launch",
             "args": ["Browser\n\u{202e}"], "tier": "confirm", "decision": "denied", "outcome": "the user declined", "tainted": true}
        ]));
        a
    }

    #[test]
    fn view_shows_pending_grants_and_newest_history_first() {
        let view = model().view();
        assert!(view.available && view.enabled && view.busy);
        assert_eq!(view.pending.len(), 2);
        assert_eq!(view.pending[0].operation, 7);
        assert!(view.pending[0].tainted);
        assert!(view.pending[1].grant);
        assert_eq!(view.grants.len(), 2);
        assert_eq!(view.history[0].tool, "app-launch");
        assert_eq!(view.history[1].args, "set 30");
    }

    #[test]
    fn nothing_an_agent_controls_reaches_a_view_unsanitized() {
        let view = model().view();
        assert!(!view.pending[0].reason.contains('\u{202e}'));
        assert!(!view.history[0].args.contains('\u{202e}'));
        assert!(!view.history[0].args.contains('\n'));
    }

    #[test]
    fn who_is_sanitized_for_pending_history_and_grants() {
        let hostile = "exe:/tmp/\u{202e}Settings\nAllowed";
        let mut a = Activity::default();
        a.set_pending(&json!([{"operation": 1, "client": hostile}]));
        a.set_grants(&json!([{"client": hostile, "tiers": []}]));
        a.set_history(&json!([{"client": hostile}]));
        let view = a.view();
        for who in [
            &view.pending[0].who,
            &view.grants[0].who,
            &view.history[0].who,
        ] {
            assert!(!who.contains('\u{202e}') && !who.contains('\n'), "{who}");
        }
    }

    #[test]
    fn a_tier_that_is_not_exactly_a_tier_is_unknown() {
        let mut a = model();
        a.set_history(&json!([{"tier": "ob\u{202e}serve", "tool": "t"}]));
        assert_eq!(a.view().history[0].tier, "unknown");
    }

    #[test]
    fn history_is_capped_to_the_most_recent_rows() {
        let mut a = Activity::default();
        let rows: Vec<Value> = (0..100).map(|i| json!({"tool": format!("t{i}")})).collect();
        a.set_history(&Value::Array(rows));
        let view = a.view();
        assert_eq!(view.history.len(), MAX_ROWS);
        assert_eq!(view.history[0].tool, "t99");
    }

    #[test]
    fn only_listed_operations_can_be_stopped() {
        let a = model();
        assert_eq!(a.stop(7), Ok(Request::Stop(7)));
        assert_eq!(a.stop(8), Err(RequestError::NotPending));
        let mut done = a.clone();
        done.set_pending(&json!([]));
        assert_eq!(done.stop(7), Err(RequestError::NotPending));
        assert!(!done.view().busy);
    }

    #[test]
    fn stop_all_is_never_refused() {
        assert_eq!(Activity::default().stop_all(), Request::StopAll);
    }

    #[test]
    fn only_listed_clients_can_be_revoked() {
        let a = model();
        assert_eq!(
            a.revoke("exe:/opt/other"),
            Ok(Request::Revoke("exe:/opt/other".into()))
        );
        assert_eq!(a.revoke("exe:/nope"), Err(RequestError::UnknownClient));
    }

    #[test]
    fn the_kill_switch_is_a_plain_toggle_and_defaults_to_on_until_known() {
        let a = Activity::default();
        assert!(!a.view().available);
        assert!(a.view().enabled);
        assert_eq!(a.set_agent_enabled(false), Request::SetEnabled(false));
    }

    #[test]
    fn malformed_rows_are_dropped_not_guessed() {
        let mut a = Activity::default();
        a.set_pending(&json!([{"tool": "x"}, {"operation": "7"}, 5]));
        assert!(a.view().pending.is_empty());
        a.set_grants(&json!([{"tiers": ["act"]}]));
        assert!(a.view().grants.is_empty());
    }
}
