//! Who may do what, as the Settings app shows and edits it: the Agent Surface manager's view of grants and the audit. This is the *only* producer
//! of write requests for that page: a change can only name a client the last read listed and a tier that can be granted, and the request
//! always carries the complete new set of tiers (the service replaces, it does not merge). Everything an agent can influence (its
//! identity string, the tools and arguments in the audit) is sanitized before it reaches a view.

use serde::Serialize;
use serde_json::Value;

use crate::activity::{history_row, HistoryRow};
use crate::display::{friendly_client_name, sanitize_for_display};

/// The tiers a person can grant, weakest to strongest. `blocked` is never grantable.
pub const GRANTABLE: &[&str] = &["observe", "observe-private", "act", "confirm"];
pub const MAX_HISTORY: usize = 100;

/// What each tier lets an agent do, in the words the page uses.
pub fn describe(tier: &str) -> &'static str {
    match tier {
        "observe" => "See the state of your desktop: volume, network, what is open.",
        "observe-private" => "See private content: clipboard, notifications, window titles.",
        "act" => "Do things on its own: change a setting, open an app.",
        "confirm" => "Ask to do things that need your approval each time.",
        _ => "",
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Grants {
    clients: Vec<(String, Vec<String>)>,
    audit: Vec<Value>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Request {
    /// Replace this client's tiers with exactly these.
    SetGrant {
        client: String,
        tiers: Vec<String>,
    },
    Revoke {
        client: String,
    },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RequestError {
    UnknownClient,
    NotGrantable,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct TierSwitch {
    pub name: &'static str,
    pub on: bool,
    pub about: &'static str,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct ClientView {
    /// Opaque key handed back to the model. Never shown.
    pub id: String,
    pub name: String,
    /// The full verified identity (a path), apart from anything the client said about itself.
    pub identity: String,
    pub tiers: Vec<TierSwitch>,
    /// What this client has done, from the audit rows loaded.
    pub requests: usize,
    pub denied: usize,
    pub last: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct GrantsView {
    pub clients: Vec<ClientView>,
    /// Newest first.
    pub history: Vec<HistoryRow>,
}

fn rows(value: &Value) -> Vec<Value> {
    value.as_array().cloned().unwrap_or_default()
}

impl Grants {
    pub fn set_grants(&mut self, value: &Value) {
        self.clients = rows(value)
            .iter()
            .filter_map(|row| {
                let client = row.get("client")?.as_str()?.to_string();
                let held = row
                    .get("tiers")
                    .and_then(Value::as_array)
                    .map(|list| {
                        list.iter()
                            .filter_map(Value::as_str)
                            .map(str::to_string)
                            .collect()
                    })
                    .unwrap_or_default();
                Some((client, held))
            })
            .collect();
    }

    pub fn set_audit(&mut self, value: &Value) {
        self.audit = rows(value);
    }

    pub fn view(&self) -> GrantsView {
        let clients = self
            .clients
            .iter()
            .map(|(client, held)| {
                let mine: Vec<&Value> = self
                    .audit
                    .iter()
                    .filter(|r| r.get("client").and_then(Value::as_str) == Some(client))
                    .collect();
                let denied = mine
                    .iter()
                    .filter(|r| {
                        matches!(
                            r.get("decision").and_then(Value::as_str),
                            Some("denied" | "rejected")
                        )
                    })
                    .count();
                let last = mine
                    .last()
                    .and_then(|r| r.get("time"))
                    .and_then(Value::as_str)
                    .unwrap_or("");
                ClientView {
                    id: client.clone(),
                    name: friendly_client_name(client, 40),
                    identity: sanitize_for_display(client, 240),
                    tiers: GRANTABLE
                        .iter()
                        .map(|name| TierSwitch {
                            name,
                            on: held.iter().any(|t| t == name),
                            about: describe(name),
                        })
                        .collect(),
                    requests: mine.len(),
                    denied,
                    last: sanitize_for_display(last, 24),
                }
            })
            .collect();
        GrantsView {
            clients,
            history: self
                .audit
                .iter()
                .rev()
                .take(MAX_HISTORY)
                .map(history_row)
                .collect(),
        }
    }

    fn held(&self, client: &str) -> Result<&Vec<String>, RequestError> {
        self.clients
            .iter()
            .find(|(c, _)| c == client)
            .map(|(_, t)| t)
            .ok_or(RequestError::UnknownClient)
    }

    /// Switch one tier of one client on or off.
    pub fn set_tier(&self, client: &str, tier: &str, on: bool) -> Result<Request, RequestError> {
        let held = self.held(client)?;
        if !GRANTABLE.contains(&tier) {
            return Err(RequestError::NotGrantable);
        }
        let tiers: Vec<String> = GRANTABLE
            .iter()
            .filter(|name| {
                if **name == tier {
                    on
                } else {
                    held.iter().any(|t| t == **name)
                }
            })
            .map(|name| (*name).to_string())
            .collect();
        // Nothing left means no access at all, which is what revoking is.
        if tiers.is_empty() {
            Ok(Request::Revoke {
                client: client.to_string(),
            })
        } else {
            Ok(Request::SetGrant {
                client: client.to_string(),
                tiers,
            })
        }
    }

    pub fn revoke(&self, client: &str) -> Result<Request, RequestError> {
        self.held(client)?;
        Ok(Request::Revoke {
            client: client.to_string(),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn model() -> Grants {
        let mut g = Grants::default();
        g.set_grants(&json!([
            {"client": "exe:/usr/bin/helix-assistant", "tiers": ["observe", "act"]},
            {"client": "exe:/opt/other/agent.py", "tiers": ["observe"]},
            {"client": "exe:/opt/bare/agent", "tiers": []}
        ]));
        g.set_audit(&json!([
            {"time": "2026-10-07T10:00:00Z", "client": "exe:/usr/bin/helix-assistant", "tool": "volume", "args": ["set", "30"], "tier": "act", "decision": "allowed", "outcome": "ok"},
            {"time": "2026-10-07T10:00:05Z", "client": "exe:/opt/other/agent.py", "tool": "app-launch", "args": ["Browser"], "tier": "confirm", "decision": "denied", "outcome": "the user declined"},
            {"time": "2026-10-07T10:00:09Z", "client": "exe:/opt/other/agent.py", "tool": "status", "args": [], "tier": "observe", "decision": "allowed", "outcome": ""}
        ]));
        g
    }

    #[test]
    fn each_client_shows_every_grantable_tier_with_its_state_and_a_plain_description() {
        let view = model().view();
        assert_eq!(view.clients.len(), 3);
        let first = &view.clients[0];
        assert_eq!(first.name, "helix-assistant");
        let on: Vec<(&str, bool)> = first.tiers.iter().map(|t| (t.name, t.on)).collect();
        assert_eq!(
            on,
            [
                ("observe", true),
                ("observe-private", false),
                ("act", true),
                ("confirm", false)
            ]
        );
        assert!(first.tiers.iter().all(|t| !t.about.is_empty()));
        assert!(
            !first.tiers.iter().any(|t| t.name == "blocked"),
            "blocked is never offered"
        );
    }

    #[test]
    fn what_each_client_has_done_comes_from_the_audit() {
        let view = model().view();
        assert_eq!((view.clients[0].requests, view.clients[0].denied), (1, 0));
        assert_eq!((view.clients[1].requests, view.clients[1].denied), (2, 1));
        assert_eq!(view.clients[1].last, "2026-10-07T10:00:09Z");
        assert_eq!(view.clients[2].requests, 0);
        assert_eq!(view.clients[2].last, "");
        assert_eq!(view.history[0].tool, "status", "newest first");
        assert_eq!(view.history.len(), 3);
    }

    #[test]
    fn the_verified_identity_is_its_own_field() {
        let view = model().view();
        assert_eq!(view.clients[0].identity, "exe:/usr/bin/helix-assistant");
    }

    #[test]
    fn switching_a_tier_sends_the_complete_new_set_in_a_fixed_order() {
        let g = model();
        assert_eq!(
            g.set_tier("exe:/usr/bin/helix-assistant", "observe-private", true),
            Ok(Request::SetGrant {
                client: "exe:/usr/bin/helix-assistant".into(),
                tiers: vec!["observe".into(), "observe-private".into(), "act".into()]
            })
        );
        assert_eq!(
            g.set_tier("exe:/usr/bin/helix-assistant", "act", false),
            Ok(Request::SetGrant {
                client: "exe:/usr/bin/helix-assistant".into(),
                tiers: vec!["observe".into()]
            })
        );
        assert_eq!(
            g.set_tier("exe:/opt/bare/agent", "confirm", true),
            Ok(Request::SetGrant {
                client: "exe:/opt/bare/agent".into(),
                tiers: vec!["confirm".into()]
            })
        );
    }

    #[test]
    fn switching_the_last_tier_off_is_a_revoke() {
        assert_eq!(
            g_revoke_last(),
            Ok(Request::Revoke {
                client: "exe:/opt/other/agent.py".into()
            })
        );
    }

    fn g_revoke_last() -> Result<Request, RequestError> {
        model().set_tier("exe:/opt/other/agent.py", "observe", false)
    }

    #[test]
    fn switching_on_what_is_already_on_changes_nothing_about_the_set() {
        assert_eq!(
            model().set_tier("exe:/opt/other/agent.py", "observe", true),
            Ok(Request::SetGrant {
                client: "exe:/opt/other/agent.py".into(),
                tiers: vec!["observe".into()]
            })
        );
    }

    #[test]
    fn only_listed_clients_and_grantable_tiers_can_be_changed() {
        let g = model();
        assert_eq!(
            g.set_tier("exe:/nope", "observe", true),
            Err(RequestError::UnknownClient)
        );
        assert_eq!(
            g.set_tier("exe:/opt/other/agent.py", "blocked", true),
            Err(RequestError::NotGrantable)
        );
        assert_eq!(
            g.set_tier("exe:/opt/other/agent.py", "godmode", true),
            Err(RequestError::NotGrantable)
        );
        assert_eq!(
            g.set_tier("exe:/opt/other/agent.py", "", true),
            Err(RequestError::NotGrantable)
        );
        assert_eq!(g.revoke("exe:/nope"), Err(RequestError::UnknownClient));
        assert_eq!(
            g.revoke("exe:/opt/bare/agent"),
            Ok(Request::Revoke {
                client: "exe:/opt/bare/agent".into()
            })
        );
    }

    #[test]
    fn nothing_an_agent_controls_reaches_the_view_unsanitized() {
        let mut g = Grants::default();
        let hostile = "exe:/tmp/\u{202e}Settings\nAllowed";
        g.set_grants(&json!([{"client": hostile, "tiers": ["observe"]}]));
        g.set_audit(&json!([{"client": hostile, "tool": "t\u{202e}x", "args": ["a\nb\u{202e}"], "tier": "ob\u{202e}serve", "decision": "allowed", "outcome": "ok\u{202e}"}]));
        let v = g.view();
        for text in [
            &v.clients[0].name,
            &v.clients[0].identity,
            &v.history[0].who,
            &v.history[0].tool,
            &v.history[0].args,
            &v.history[0].outcome,
        ] {
            assert!(
                !text.contains('\u{202e}') && !text.contains('\n'),
                "{text:?}"
            );
        }
        assert_eq!(v.history[0].tier, "unknown");
    }

    #[test]
    fn malformed_rows_are_dropped_and_the_history_is_bounded() {
        let mut g = Grants::default();
        g.set_grants(&json!([{"tiers": ["act"]}, 5, {"client": "exe:/ok", "tiers": "no"}]));
        assert_eq!(g.view().clients.len(), 1);
        assert!(g.view().clients[0].tiers.iter().all(|t| !t.on));
        let many: Vec<Value> = (0..500).map(|i| json!({"tool": format!("t{i}")})).collect();
        g.set_audit(&Value::Array(many));
        assert_eq!(g.view().history.len(), MAX_HISTORY);
        assert_eq!(g.view().history[0].tool, "t499");
    }

    #[test]
    fn rejected_requests_count_as_denied_too_and_the_last_time_is_sanitized() {
        let mut g = Grants::default();
        g.set_grants(&json!([{"client": "exe:/a", "tiers": ["observe"]}]));
        g.set_audit(&json!([
            {"time": "t1", "client": "exe:/a", "decision": "rejected"},
            {"time": "t2", "client": "exe:/a", "decision": "denied"},
            {"time": "2026\u{202e}-10\nfake", "client": "exe:/a", "decision": "allowed"}
        ]));
        let c = &g.view().clients[0];
        assert_eq!((c.requests, c.denied), (3, 2));
        assert!(
            !c.last.contains('\u{202e}') && !c.last.contains('\n'),
            "{:?}",
            c.last
        );
    }
}
