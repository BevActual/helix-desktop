//! Agent1 confirmation queue. Everything except the operation id and kind is influenced by the
//! requesting agent and is sanitized before a dialog can show it. The service enforces deadlines;
//! this queue bounds what a flood can do to the person.

use std::collections::VecDeque;

use serde_json::Value;

use crate::display::{friendly_client_name, sanitize_for_display};

pub const MAX_PENDING: usize = 8;
pub const MAX_PER_CLIENT: usize = 3;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Action,
    Grant,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ConfirmRequest {
    pub operation: u64,
    pub kind: Kind,
    /// Verified identity from the service, shortened for display.
    pub client: String,
    /// Short name from the full identity before shortening.
    pub client_name: String,
    /// Self-declared app name (grants).
    pub label: String,
    pub tool: String,
    /// The agent's own words; displayed as a quotation, never as a fact.
    pub reason: String,
    pub args: Vec<String>,
    pub tiers: Vec<String>,
    /// The client read private content since the person last approved something.
    pub tainted: bool,
}

impl ConfirmRequest {
    /// Returns `None` for a request that is malformed rather than a display problem.
    pub fn from_json(details: &Value) -> Option<Self> {
        let object = details.as_object()?;
        let operation = object.get("operation").and_then(json_u64)?;
        let raw_client = object.get("client").and_then(Value::as_str)?;
        let client = sanitize_for_display(raw_client, 240);
        if client.is_empty() {
            return None;
        }
        // Name comes from the whole identity: shortening first would leave a path fragment.
        let client_name = friendly_client_name(raw_client, 40);
        let kind = if object.get("kind").and_then(Value::as_str) == Some("grant") {
            Kind::Grant
        } else {
            Kind::Action
        };
        let label = string_field(object, "label", 64);
        let tool = string_field(object, "tool", 64);
        let reason = string_field(object, "reason", 200);
        let args = string_list(object, "args", 8, 120);
        let tiers = string_list(object, "tiers", 5, 24);
        let tainted = object
            .get("tainted")
            .and_then(Value::as_bool)
            .unwrap_or(false);
        if kind == Kind::Action && tool.is_empty() {
            return None;
        }
        if kind == Kind::Grant && tiers.is_empty() {
            return None;
        }
        Some(Self {
            operation,
            kind,
            client,
            client_name,
            label,
            tool,
            reason,
            args,
            tiers,
            tainted,
        })
    }
}

fn json_u64(value: &Value) -> Option<u64> {
    match value {
        Value::Number(number) => {
            if let Some(unsigned) = number.as_u64() {
                Some(unsigned)
            } else {
                number
                    .as_i64()
                    .filter(|&signed| signed >= 0)
                    .map(|signed| signed as u64)
            }
        }
        _ => None,
    }
}

fn string_field(object: &serde_json::Map<String, Value>, key: &str, max_chars: usize) -> String {
    object
        .get(key)
        .and_then(Value::as_str)
        .map(|text| sanitize_for_display(text, max_chars))
        .unwrap_or_default()
}

fn string_list(
    object: &serde_json::Map<String, Value>,
    key: &str,
    max_items: usize,
    max_chars: usize,
) -> Vec<String> {
    let Some(Value::Array(items)) = object.get(key) else {
        return Vec::new();
    };
    items
        .iter()
        .filter_map(Value::as_str)
        .take(max_items)
        .map(|text| sanitize_for_display(text, max_chars))
        .collect()
}

/// `tiers` is meaningful for grants only: the tiers the person approved (a subset of what was asked).
pub type Responder = Box<dyn FnMut(u64, bool, Vec<String>) + Send>;

pub struct ConfirmationQueue {
    responder: Responder,
    pending: VecDeque<ConfirmRequest>,
    /// Fired when the request shown to the person changes.
    pub on_changed: Option<Box<dyn FnMut() + Send>>,
}

impl ConfirmationQueue {
    pub fn new(responder: Responder) -> Self {
        Self {
            responder,
            pending: VecDeque::new(),
            on_changed: None,
        }
    }

    pub fn front(&self) -> Option<&ConfirmRequest> {
        self.pending.front()
    }

    pub fn size(&self) -> usize {
        self.pending.len()
    }

    pub fn enqueue(&mut self, request: ConfirmRequest) {
        let from_client = self
            .pending
            .iter()
            .filter(|pending| pending.client == request.client)
            .count();
        if self.pending.len() >= MAX_PENDING || from_client >= MAX_PER_CLIENT {
            (self.responder)(request.operation, false, Vec::new());
            return;
        }
        let becomes_front = self.pending.is_empty();
        self.pending.push_back(request);
        if becomes_front {
            self.fire_changed();
        }
    }

    pub fn answer_front(&mut self, approved: bool, tiers: Vec<String>) {
        let Some(answered) = self.pending.pop_front() else {
            return;
        };
        let (approved, tiers) = if answered.kind == Kind::Grant && approved {
            let kept: Vec<String> = tiers
                .into_iter()
                .filter(|tier| answered.tiers.iter().any(|asked| asked == tier))
                .collect();
            if kept.is_empty() {
                (false, kept)
            } else {
                (true, kept)
            }
        } else {
            (approved, tiers)
        };
        (self.responder)(answered.operation, approved, tiers);
        self.fire_changed();
    }

    pub fn drop_operation(&mut self, operation: u64) {
        let was_front = self
            .pending
            .front()
            .is_some_and(|request| request.operation == operation);
        self.pending
            .retain(|request| request.operation != operation);
        if was_front {
            self.fire_changed();
        }
    }

    pub fn deny_all(&mut self) {
        let waiting: Vec<_> = self.pending.drain(..).collect();
        for request in waiting {
            (self.responder)(request.operation, false, Vec::new());
        }
        self.fire_changed();
    }

    fn fire_changed(&mut self) {
        if let Some(on_changed) = self.on_changed.as_mut() {
            on_changed();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{ConfirmRequest, ConfirmationQueue, Kind, MAX_PENDING, MAX_PER_CLIENT};
    use serde_json::json;
    use std::sync::{Arc, Mutex};

    #[derive(Clone, Debug, PartialEq, Eq)]
    struct Answer {
        operation: u64,
        approved: bool,
        tiers: Vec<String>,
    }

    fn action(operation: u64, client: &str) -> ConfirmRequest {
        ConfirmRequest {
            operation,
            kind: Kind::Action,
            client: client.to_string(),
            client_name: "agent".to_string(),
            label: String::new(),
            tool: "app-launch".to_string(),
            reason: String::new(),
            args: Vec::new(),
            tiers: Vec::new(),
            tainted: false,
        }
    }

    fn queue_with_log() -> (ConfirmationQueue, Arc<Mutex<Vec<Answer>>>, Arc<Mutex<i32>>) {
        let answers = Arc::new(Mutex::new(Vec::new()));
        let log = answers.clone();
        let changes = Arc::new(Mutex::new(0));
        let changed = changes.clone();
        let mut queue = ConfirmationQueue::new(Box::new(move |operation, approved, tiers| {
            log.lock().expect("answers").push(Answer {
                operation,
                approved,
                tiers,
            });
        }));
        queue.on_changed = Some(Box::new(move || {
            *changed.lock().expect("changes") += 1;
        }));
        (queue, answers, changes)
    }

    #[test]
    fn parse_action_and_grant_and_rejects_malformed() {
        let request = ConfirmRequest::from_json(&json!({
            "operation": 7,
            "client": "exe:/x",
            "tool": "app-launch",
            "args": ["Settings"],
            "reason": "open it\nnow",
            "tainted": true
        }))
        .unwrap();
        assert_eq!(request.operation, 7);
        assert_eq!(request.kind, Kind::Action);
        assert_eq!(request.reason, "open it now");
        assert!(request.tainted);
        assert_eq!(request.args, vec!["Settings".to_string()]);

        let grant = ConfirmRequest::from_json(&json!({
            "operation": 8,
            "kind": "grant",
            "client": "exe:/x",
            "label": "My Agent",
            "tiers": ["observe", "act"]
        }))
        .unwrap();
        assert_eq!(grant.kind, Kind::Grant);
        assert_eq!(grant.tiers.len(), 2);
        assert_eq!(grant.label, "My Agent");

        assert!(ConfirmRequest::from_json(&json!([])).is_none());
        assert!(ConfirmRequest::from_json(&json!({"client": "exe:/x", "tool": "t"})).is_none());
        assert!(ConfirmRequest::from_json(&json!({"operation": 1, "tool": "t"})).is_none());
        assert!(ConfirmRequest::from_json(&json!({"operation": 1, "client": "c"})).is_none());
        assert!(ConfirmRequest::from_json(
            &json!({"operation": 1, "client": "c", "kind": "grant"})
        )
        .is_none());

        let many: Vec<String> = (0..40).map(|_| "x".repeat(500)).collect();
        let big = ConfirmRequest::from_json(&json!({
            "operation": 1,
            "client": "c",
            "tool": "t",
            "args": many
        }))
        .unwrap();
        assert_eq!(big.args.len(), 8);
        assert!(big.args[0].chars().count() <= 121); // 120 + possible ellipsis
    }

    #[test]
    fn client_name_comes_from_the_end_of_a_long_identity() {
        let long_id = format!(
            "exe:/usr/bin/python3.14#/home/someone/a/very/long/path/{}/server.py",
            "d".repeat(300)
        );
        let request = ConfirmRequest::from_json(&json!({
            "operation": 1,
            "client": long_id,
            "tool": "status"
        }))
        .unwrap();
        assert_eq!(request.client_name, "server.py");
        assert!(request.client.len() < long_id.len());
    }

    #[test]
    fn queue_bounds_per_client_and_total_and_drop_deny() {
        let (mut queue, answers, changes) = queue_with_log();

        queue.enqueue(action(1, "exe:/usr/bin/agent"));
        assert_eq!(*changes.lock().unwrap(), 1);
        assert_eq!(queue.front().unwrap().operation, 1);
        queue.enqueue(action(2, "exe:/other"));
        assert_eq!(*changes.lock().unwrap(), 1);
        assert_eq!(queue.size(), 2);

        queue.answer_front(true, Vec::new());
        {
            let answers = answers.lock().unwrap();
            assert_eq!(answers.len(), 1);
            assert_eq!(answers[0].operation, 1);
            assert!(answers[0].approved);
        }
        assert_eq!(queue.front().unwrap().operation, 2);
        assert_eq!(*changes.lock().unwrap(), 2);
        queue.answer_front(false, Vec::new());
        assert!(!answers.lock().unwrap()[1].approved);
        assert_eq!(queue.size(), 0);
        queue.answer_front(true, Vec::new());
        assert_eq!(answers.lock().unwrap().len(), 2);

        for i in 10..16 {
            queue.enqueue(action(i, "exe:/usr/bin/agent"));
        }
        assert_eq!(queue.size(), MAX_PER_CLIENT);
        let refused = answers
            .lock()
            .unwrap()
            .iter()
            .filter(|answer| answer.operation >= 10 && !answer.approved)
            .count();
        assert_eq!(refused, 3);
        for i in 20..30 {
            queue.enqueue(action(i, &format!("exe:/c{i}")));
        }
        assert_eq!(queue.size(), MAX_PENDING);

        answers.lock().unwrap().clear();
        queue.deny_all();
        assert_eq!(queue.size(), 0);
        let answers_now = answers.lock().unwrap().clone();
        assert_eq!(answers_now.len(), MAX_PENDING);
        assert!(answers_now.iter().all(|answer| !answer.approved));

        queue.enqueue(action(40, "exe:/usr/bin/agent"));
        queue.enqueue(action(41, "exe:/other"));
        queue.drop_operation(40);
        assert_eq!(queue.size(), 1);
        assert_eq!(queue.front().unwrap().operation, 41);
        queue.drop_operation(999);
        assert_eq!(queue.size(), 1);
    }

    #[test]
    fn grant_narrowing_intersects_requested_tiers() {
        let (mut queue, answers, _) = queue_with_log();
        let grant = ConfirmRequest {
            operation: 5,
            kind: Kind::Grant,
            client: "exe:/x".to_string(),
            client_name: "x".to_string(),
            label: String::new(),
            tool: String::new(),
            reason: String::new(),
            args: Vec::new(),
            tiers: vec!["observe".into(), "act".into()],
            tainted: false,
        };

        queue.enqueue(grant.clone());
        queue.answer_front(
            true,
            vec!["observe".into(), "confirm".into(), "blocked".into()],
        );
        {
            let answers = answers.lock().unwrap();
            assert!(answers[0].approved);
            assert_eq!(answers[0].tiers, vec!["observe".to_string()]);
        }

        let mut next = grant.clone();
        next.operation = 6;
        queue.enqueue(next);
        queue.answer_front(true, vec!["confirm".into()]);
        {
            let answers = answers.lock().unwrap();
            assert!(!answers[1].approved);
            assert!(answers[1].tiers.is_empty());
        }

        let mut denied = grant;
        denied.operation = 7;
        queue.enqueue(denied);
        queue.answer_front(false, vec!["observe".into()]);
        assert!(!answers.lock().unwrap()[2].approved);
    }
}
