//! The Agent1 confirmer panel as a pure model. Owns the confirmation queue and the 700 ms arming
//! clock for whatever request is showing. Enter never approves. Closing denies everything still
//! pending. Grant checkboxes default to least privilege (`confirm` unchecked).

use serde::Serialize;
use serde_json::Value;

use crate::agent_confirm::{ConfirmRequest, ConfirmationQueue, Kind, Responder};
use crate::confirm::Confirmation;

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct TierView {
    pub name: String,
    pub checked: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct ConfirmerView {
    pub open: bool,
    pub kind: &'static str,
    pub client_name: String,
    pub detail: String,
    /// The client's full verified identity (a path), always shown apart from anything it wrote itself.
    pub identity: String,
    /// What would run could not be shown in full, so Allow is dead and the request can only be denied.
    pub incomplete: bool,
    pub aside: String,
    pub tainted: bool,
    pub waiting: u64,
    pub tiers: Vec<TierView>,
    pub armed: bool,
    pub operation: u64,
}

/// An argument as a person would type it: bare when it is a plain word, otherwise in double quotes with `"` and `\` escaped, so where one
/// argument ends and the next begins is always visible.
fn quote_arg(arg: &str) -> String {
    let plain = !arg.is_empty()
        && !arg
            .chars()
            .any(|c| c.is_whitespace() || matches!(c, '"' | '\\' | '\''));
    if plain {
        arg.to_string()
    } else {
        format!("\"{}\"", arg.replace('\\', "\\\\").replace('"', "\\\""))
    }
}

pub struct Confirmer {
    queue: ConfirmationQueue,
    arming: Confirmation,
    shown_operation: Option<u64>,
    /// Checkbox state for the grant currently showing. Cleared for actions.
    tiers: Vec<TierView>,
}

impl Confirmer {
    pub fn new(responder: Responder) -> Self {
        Self {
            queue: ConfirmationQueue::new(responder),
            arming: Confirmation::new(),
            shown_operation: None,
            tiers: Vec::new(),
        }
    }

    pub fn size(&self) -> usize {
        self.queue.size()
    }

    pub fn front(&self) -> Option<&ConfirmRequest> {
        self.queue.front()
    }

    /// Parse and enqueue a `ConfirmationRequested` payload. Malformed requests are ignored.
    pub fn requested(&mut self, details: &Value, now_ms: u64) {
        let Some(request) = ConfirmRequest::from_json(details) else {
            return;
        };
        self.queue.enqueue(request);
        self.sync_shown(now_ms);
    }

    pub fn ended(&mut self, operation: u64, now_ms: u64) {
        self.queue.drop_operation(operation);
        self.sync_shown(now_ms);
    }

    /// Enter never approves.
    pub fn enter(&mut self, _now_ms: u64) -> bool {
        false
    }

    /// Toggle a grant tier checkbox by name. Unknown names are ignored.
    pub fn set_tier(&mut self, operation: u64, name: &str, checked: bool) {
        if self.queue.front().map(|f| f.operation) != Some(operation) {
            return;
        }
        if let Some(tier) = self.tiers.iter_mut().find(|tier| tier.name == name) {
            tier.checked = checked;
        }
    }

    /// Restart the 700 ms arming clock from now. Used when the Overlay has just become visible so
    /// Allow is not already armed after a slow webview load.
    pub fn present(&mut self, now_ms: u64) {
        if self.queue.front().is_some() {
            self.arming.show(now_ms);
        }
    }

    /// Allow. Ignored until armed. For grants, empty selection is a no-op (does not answer).
    pub fn allow(&mut self, operation: u64, now_ms: u64) -> bool {
        let Some(front) = self.queue.front() else {
            return false;
        };
        // The click was made on what the person was looking at: if the front request has changed since, it is not for this one.
        if front.operation != operation {
            return false;
        }
        let grant = front.kind == Kind::Grant;
        if !grant && !front.args_complete {
            return false;
        }
        let tiers = if grant {
            let selected: Vec<String> = self
                .tiers
                .iter()
                .filter(|tier| tier.checked)
                .map(|tier| tier.name.clone())
                .collect();
            if selected.is_empty() {
                return false;
            }
            selected
        } else {
            Vec::new()
        };
        if !self.arming.activate(now_ms) {
            return false;
        }
        self.queue.answer_front(true, tiers);
        self.sync_shown(now_ms);
        true
    }

    /// Deny the front request only.
    pub fn deny(&mut self, operation: u64, now_ms: u64) -> bool {
        if self.queue.front().map(|f| f.operation) != Some(operation) {
            return false;
        }
        self.queue.answer_front(false, Vec::new());
        self.sync_shown(now_ms);
        true
    }

    /// Escape / close without answering: refuse everything still pending.
    pub fn close(&mut self, now_ms: u64) {
        self.queue.deny_all();
        self.sync_shown(now_ms);
    }

    pub fn view(&self, now_ms: u64) -> ConfirmerView {
        let Some(request) = self.queue.front() else {
            return ConfirmerView {
                open: false,
                kind: "action",
                client_name: String::new(),
                detail: String::new(),
                identity: String::new(),
                incomplete: false,
                aside: String::new(),
                tainted: false,
                waiting: 0,
                tiers: Vec::new(),
                armed: false,
                operation: 0,
            };
        };
        let grant = request.kind == Kind::Grant;
        let mut detail = request.tool.clone();
        for arg in &request.args {
            detail.push(' ');
            detail.push_str(&quote_arg(arg));
        }
        let incomplete = !grant && !request.args_complete;
        if incomplete {
            detail.push_str(" …");
        }
        let aside = if incomplete {
            "Not shown in full (too long or unusual), so it cannot be reviewed. It can only be denied.".to_string()
        } else if grant && !request.label.is_empty() {
            format!("It calls itself “{}”.", request.label)
        } else if !request.reason.is_empty() {
            format!("The assistant says: “{}”", request.reason)
        } else {
            String::new()
        };
        ConfirmerView {
            open: true,
            kind: if grant { "grant" } else { "action" },
            client_name: request.client_name.clone(),
            detail: if grant {
                "Choose what this app may do.".to_string()
            } else {
                detail
            },
            identity: request.client.clone(),
            incomplete,
            aside,
            tainted: request.tainted && !grant,
            waiting: self.queue.size().saturating_sub(1) as u64,
            tiers: self.tiers.clone(),
            armed: !incomplete && self.arming.armed(now_ms),
            operation: request.operation,
        }
    }

    fn sync_shown(&mut self, now_ms: u64) {
        let Some(front) = self.queue.front().cloned() else {
            self.shown_operation = None;
            self.tiers.clear();
            self.arming = Confirmation::new();
            return;
        };
        if self.shown_operation == Some(front.operation) {
            return;
        }
        self.shown_operation = Some(front.operation);
        self.arming.show(now_ms);
        self.tiers = if front.kind == Kind::Grant {
            front
                .tiers
                .iter()
                .map(|name| TierView {
                    // Least privilege: the tier that needs approval each time is opt-in.
                    checked: name != "confirm",
                    name: name.clone(),
                })
                .collect()
        } else {
            Vec::new()
        };
    }
}

#[cfg(test)]
mod tests {
    use super::Confirmer;
    use crate::confirm::ARMING_MS;
    use serde_json::json;
    use std::sync::{Arc, Mutex};

    type Answers = Arc<Mutex<Vec<(u64, bool, Vec<String>)>>>;

    fn confirmer() -> (Confirmer, Answers) {
        let log = Arc::new(Mutex::new(Vec::new()));
        let sink = log.clone();
        let panel = Confirmer::new(Box::new(move |operation, approved, tiers| {
            sink.lock().unwrap().push((operation, approved, tiers));
        }));
        (panel, log)
    }

    #[test]
    fn enter_never_approves_and_allow_arms_per_request() {
        let (mut panel, log) = confirmer();
        panel.requested(
            &json!({
                "operation": 1,
                "client": "exe:/usr/bin/agent",
                "tool": "app-launch",
                "args": ["Settings"]
            }),
            0,
        );
        assert!(panel.view(0).open);
        assert!(!panel.view(0).armed);
        assert!(!panel.enter(ARMING_MS));
        assert!(!panel.allow(1, ARMING_MS - 1));
        assert!(log.lock().unwrap().is_empty());
        assert!(panel.allow(1, ARMING_MS));
        assert_eq!(log.lock().unwrap()[0], (1, true, Vec::new()));
        assert!(!panel.view(ARMING_MS).open);
    }

    #[test]
    fn escape_denies_and_close_denies_all() {
        let (mut panel, log) = confirmer();
        panel.requested(&json!({"operation": 1, "client": "exe:/a", "tool": "t"}), 0);
        panel.requested(&json!({"operation": 2, "client": "exe:/b", "tool": "t"}), 0);
        assert!(panel.deny(1, 0));
        assert_eq!(log.lock().unwrap()[0], (1, false, Vec::new()));
        assert_eq!(panel.front().unwrap().operation, 2);
        panel.close(0);
        assert_eq!(log.lock().unwrap()[1], (2, false, Vec::new()));
        assert!(!panel.view(0).open);
    }

    #[test]
    fn grant_confirm_is_opt_in_and_empty_allow_is_noop() {
        let (mut panel, log) = confirmer();
        panel.requested(
            &json!({
                "operation": 5,
                "kind": "grant",
                "client": "exe:/x",
                "label": "My Agent",
                "tiers": ["observe", "confirm"]
            }),
            0,
        );
        let view = panel.view(0);
        assert_eq!(view.kind, "grant");
        assert_eq!(
            view.aside, "It calls itself “My Agent”.",
            "the identity is its own field, not part of the app's words"
        );
        assert_eq!(view.identity, "exe:/x");
        assert_eq!(
            view.tiers
                .iter()
                .map(|tier| (tier.name.as_str(), tier.checked))
                .collect::<Vec<_>>(),
            vec![("observe", true), ("confirm", false)]
        );
        panel.set_tier(5, "observe", false);
        assert!(!panel.allow(5, ARMING_MS));
        assert!(log.lock().unwrap().is_empty());
        panel.set_tier(5, "observe", true);
        assert!(panel.allow(5, ARMING_MS));
        assert_eq!(
            log.lock().unwrap()[0],
            (5, true, vec!["observe".to_string()])
        );
    }

    #[test]
    fn confirmation_ended_drops_a_stale_front() {
        let (mut panel, _) = confirmer();
        panel.requested(
            &json!({"operation": 40, "client": "exe:/a", "tool": "t"}),
            0,
        );
        panel.requested(
            &json!({"operation": 41, "client": "exe:/b", "tool": "t"}),
            0,
        );
        panel.ended(40, 100);
        assert_eq!(panel.front().unwrap().operation, 41);
        assert!(!panel.view(100).armed); // re-armed for the new front
        assert!(panel.view(100 + ARMING_MS).armed);
    }

    #[test]
    fn tainted_warning_only_on_actions() {
        let (mut panel, _) = confirmer();
        panel.requested(
            &json!({
                "operation": 1,
                "client": "exe:/a",
                "tool": "t",
                "tainted": true
            }),
            0,
        );
        assert!(panel.view(0).tainted);
        panel.deny(1, 0);
        panel.requested(
            &json!({
                "operation": 2,
                "kind": "grant",
                "client": "exe:/a",
                "tiers": ["observe"],
                "tainted": true
            }),
            0,
        );
        assert!(!panel.view(0).tainted);
    }

    fn action(operation: u64, args: serde_json::Value) -> serde_json::Value {
        json!({"operation": operation, "client": "exe:/usr/bin/agent", "tool": "clipboard-copy", "args": args})
    }

    #[test]
    fn arguments_are_shown_whole_and_quoted_where_they_could_run_together() {
        let (mut panel, _) = confirmer();
        panel.requested(
            &action(
                1,
                json!(["plain", "two words", "", "say \"hi\"", "back\\slash"]),
            ),
            0,
        );
        let view = panel.view(0);
        assert_eq!(
            view.detail,
            "clipboard-copy plain \"two words\" \"\" \"say \\\"hi\\\"\" \"back\\\\slash\""
        );
        assert!(!view.incomplete);
    }

    #[test]
    fn an_argument_that_could_not_be_shown_whole_can_only_be_denied() {
        for args in [
            json!(["x".repeat(401)]),
            json!([
                "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q"
            ]),
            json!(["ok", 5]),
            json!(["bidi\u{202e}text"]),
        ] {
            let (mut panel, log) = confirmer();
            panel.requested(&action(1, args), 0);
            let view = panel.view(ARMING_MS * 3);
            assert!(view.incomplete && !view.armed, "{view:?}");
            assert!(view.detail.ends_with('…'));
            assert!(!panel.allow(1, ARMING_MS * 3), "approval must be refused");
            assert!(log.lock().unwrap().is_empty());
            assert!(
                panel.deny(1, ARMING_MS * 3),
                "but denying is always possible"
            );
            assert_eq!(log.lock().unwrap()[0], (1, false, Vec::new()));
        }
    }

    #[test]
    fn four_hundred_characters_and_sixteen_arguments_are_still_reviewable() {
        let (mut panel, _) = confirmer();
        let args: Vec<String> = (0..16)
            .map(|i| format!("{i:02}{}", "y".repeat(398)))
            .collect();
        panel.requested(&action(1, json!(args)), 0);
        assert!(!panel.view(0).incomplete);
        assert!(panel.allow(1, ARMING_MS));
    }

    #[test]
    fn the_verified_identity_is_always_shown_apart_from_what_the_agent_says() {
        let (mut panel, _) = confirmer();
        panel.requested(
            &json!({"operation": 1, "client": "exe:/tmp/helix-settings", "tool": "t", "reason": "I am the real settings app"}),
            0,
        );
        let view = panel.view(0);
        assert_eq!(view.identity, "exe:/tmp/helix-settings");
        assert!(view.aside.contains("I am the real settings app"));
        assert!(
            !view.aside.contains("/tmp/"),
            "the aside is the agent's words only"
        );
    }

    #[test]
    fn a_decision_made_on_a_request_that_is_no_longer_in_front_does_nothing() {
        let (mut panel, log) = confirmer();
        panel.requested(&action(1, json!(["a"])), 0);
        panel.requested(&action(2, json!(["b"])), 0);
        panel.ended(1, 100);
        // The page still shows operation 1 and its Allow is clicked after 2 has armed.
        assert!(!panel.allow(1, 100 + ARMING_MS + 50));
        assert!(!panel.deny(1, 100 + ARMING_MS + 50));
        assert!(log.lock().unwrap().is_empty());
        assert!(panel.allow(2, 100 + ARMING_MS + 50));
        assert_eq!(log.lock().unwrap()[0], (2, true, Vec::new()));
    }

    #[test]
    fn a_tier_switch_for_another_request_is_ignored() {
        let (mut panel, _) = confirmer();
        panel.requested(&json!({"operation": 5, "kind": "grant", "client": "exe:/x", "tiers": ["observe", "confirm"]}), 0);
        panel.set_tier(9, "confirm", true);
        assert!(!panel
            .view(0)
            .tiers
            .iter()
            .any(|t| t.name == "confirm" && t.checked));
        panel.set_tier(5, "confirm", true);
        assert!(panel
            .view(0)
            .tiers
            .iter()
            .any(|t| t.name == "confirm" && t.checked));
    }
}
