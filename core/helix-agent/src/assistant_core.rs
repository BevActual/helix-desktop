//! The decisions of the Helix assistant that need no D-Bus: reading the model's reply and deciding what happens to it
//! (port of C++ `src/assistant/assistant_core.*`). Everything a model says is a suggestion; the Agent Surface service
//! still decides what is allowed. This only decides what the assistant shows the person first.

use std::collections::BTreeSet;

use serde_json::{json, Value};

use super::risk::{RiskPolicy, Tier};
use super::tool_schema::{ToolHints, DECLINE_TOOL};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReplyKind {
    Call,
    Decline,
    Invalid,
}

#[derive(Debug, Clone)]
pub struct Reply {
    pub kind: ReplyKind,
    pub tool: String,
    pub args: Vec<String>,
    /// The model's own words: why it declined, or why it chose this tool.
    pub reason: String,
    /// [`ReplyKind::Invalid`] only.
    pub error: String,
}

impl Reply {
    fn invalid(error: &str) -> Self {
        Self {
            kind: ReplyKind::Invalid,
            tool: String::new(),
            args: Vec::new(),
            reason: String::new(),
            error: error.to_string(),
        }
    }

    pub fn call(tool: &str, args: Vec<String>) -> Self {
        Self {
            kind: ReplyKind::Call,
            tool: tool.to_string(),
            args,
            reason: String::new(),
            error: String::new(),
        }
    }
}

/// `text` is the model's constrained reply: `{"tool": ..., "args": [...], "reason"?: ...}`. `decline` is a reply, never a call.
pub fn parse_reply(text: &str) -> Reply {
    let Ok(Value::Object(object)) = serde_json::from_str::<Value>(text) else {
        return Reply::invalid("the model's reply was not a tool call");
    };
    let Some(tool) = object.get("tool").and_then(Value::as_str) else {
        return Reply::invalid("the model's reply was not a tool call");
    };
    let mut reply = Reply::call(tool, Vec::new());
    if let Some(reason) = object.get("reason").and_then(Value::as_str) {
        reply.reason = reason.to_string();
    }
    if let Some(args) = object.get("args") {
        let Some(list) = args.as_array() else {
            return Reply::invalid("arguments were not a list");
        };
        for arg in list {
            let Some(text) = arg.as_str() else {
                return Reply::invalid("an argument was not text");
            };
            reply.args.push(text.to_string());
        }
    }
    if reply.tool == DECLINE_TOOL {
        reply.kind = ReplyKind::Decline;
        reply.args.clear(); // a decline has no arguments to forward, whatever was sent
    }
    reply
}

/// Measured in the evaluation (`tests/eval/desktop_requests.json`), plus the format of a call.
const EXAMPLES: [(&str, &str); 8] = [
    (
        "Set the log level to debug.",
        r#"{"tool":"log-level-set","args":["debug"]}"#,
    ),
    (
        "Make the bar hide itself unless I move the mouse to the edge.",
        r#"{"tool":"bar-auto-hide-set","args":["smart"]}"#,
    ),
    (
        "Reload the configuration.",
        r#"{"tool":"config-reload","args":[]}"#,
    ),
    (
        "Turn the on-screen popups off.",
        r#"{"tool":"osd-disable","args":[]}"#,
    ),
    (
        "Set the output audio profile to Music.",
        r#"{"tool":"effects-profile-set","args":["output","Music"]}"#,
    ),
    (
        "Order me a pizza.",
        r#"{"tool":"decline","args":[],"reason":"I can only control this desktop; I cannot order food."}"#,
    ),
    (
        "Pair my phone over Bluetooth.",
        r#"{"tool":"decline","args":[],"reason":"I can only read Bluetooth status here; I cannot pair devices."}"#,
    ),
    (
        "Turn the heating up.",
        r#"{"tool":"decline","args":[],"reason":"I can only control this desktop, not the house."}"#,
    ),
];

/// The conversation sent to the model: instructions and tool list, the few-shot examples, then the request.
pub fn build_messages(catalog: &str, request: &str) -> Value {
    let mut messages = vec![json!({
        "role": "system",
        "content": format!(
            "You control a Linux desktop. Answer with exactly one JSON tool call for the user's request, \
             choosing the single most suitable tool. If no tool does what was asked, answer with decline and say why: \
             never pick a status or read-only tool, or a different action, as a stand-in. Use only these tools:\n{catalog}"
        ),
    })];
    for (ask, answer) in EXAMPLES {
        messages.push(json!({"role": "user", "content": ask}));
        messages.push(json!({"role": "assistant", "content": answer}));
    }
    messages.push(json!({"role": "user", "content": request}));
    Value::Array(messages)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Step {
    /// Read-only: run it and show the answer.
    Run,
    /// Changes something: show "about to do X" and wait for a yes.
    AskFirst,
    /// Not something this client may do (unknown tool, blocked tier, no grant).
    Refuse,
    /// The model declined, or the reply was unusable: show why, do nothing.
    Explain,
}

pub fn step_for(policy: &RiskPolicy, granted: &BTreeSet<Tier>, reply: &Reply) -> Step {
    if reply.kind != ReplyKind::Call {
        return Step::Explain;
    }
    if !policy.knows(&reply.tool) {
        return Step::Refuse;
    }
    let tier = policy.tier_for(&reply.tool, &reply.args);
    if !granted.contains(&tier) {
        return Step::Refuse; // also covers `blocked`, which is never granted
    }
    match tier {
        Tier::Observe | Tier::ObservePrivate => Step::Run,
        _ => Step::AskFirst,
    }
}

/// What a person is shown before saying yes: the tool's plain-words description when there is one, then the exact call.
/// The description matters: `power-cycle` sounds like a restart, and the person must not be misled by a tool's name.
pub fn describe(reply: &Reply, hints: Option<&ToolHints>) -> String {
    let mut call = reply.tool.clone();
    for arg in &reply.args {
        call.push(' ');
        call.push_str(arg);
    }
    match hints.and_then(|h| h.find(&reply.tool)) {
        Some(hint) => format!("{hint} (`{call}`)"),
        None => format!("Run `{call}`"),
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModelChoice {
    pub id: String,
    pub error: String,
}

/// Which installed model to ask. `preferred` (the person's choice) wins if installed; otherwise the largest, the one the
/// measurements favour. `models` is `ListModels`' reply.
pub fn choose_model(models: &Value, preferred: &str) -> ModelChoice {
    let fail = |error: String| ModelChoice {
        id: String::new(),
        error,
    };
    let Some(list) = models.as_array().filter(|list| !list.is_empty()) else {
        return fail("no model is installed (put one in ~/.local/share/helix/models)".into());
    };
    let mut best: Option<(&str, u64)> = None;
    for model in list {
        let Some(id) = model.get("id").and_then(Value::as_str) else {
            continue;
        };
        if !preferred.is_empty() && id == preferred {
            return ModelChoice {
                id: preferred.to_string(),
                error: String::new(),
            };
        }
        let bytes = model.get("bytes").and_then(Value::as_u64).unwrap_or(0);
        if best.is_none_or(|(_, most)| bytes > most) {
            best = Some((id, bytes));
        }
    }
    if !preferred.is_empty() {
        return fail(format!("the model '{preferred}' is not installed"));
    }
    match best {
        Some((id, _)) => ModelChoice {
            id: id.to_string(),
            error: String::new(),
        },
        None => fail("no usable model is installed".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn everyday() -> BTreeSet<Tier> {
        [Tier::Observe, Tier::ObservePrivate, Tier::Act]
            .into_iter()
            .collect()
    }

    #[test]
    fn replies_are_read_strictly() {
        let call =
            parse_reply(r#"{"tool":"panel-open","args":["control-center"],"reason":"you asked"}"#);
        assert_eq!(call.kind, ReplyKind::Call);
        assert_eq!(
            (call.tool.as_str(), call.args.len(), call.reason.as_str()),
            ("panel-open", 1, "you asked")
        );
        for bad in [
            "not json",
            r#"{"args":[]}"#,
            r#"{"tool":"status","args":[1]}"#,
            r#"{"tool":"status","args":"x"}"#,
            "[]",
        ] {
            assert_eq!(parse_reply(bad).kind, ReplyKind::Invalid, "{bad}");
        }
    }

    #[test]
    fn a_decline_cannot_smuggle_arguments() {
        let policy = RiskPolicy::embedded();
        let decline = parse_reply(
            r#"{"tool":"decline","args":["session","reboot"],"reason":"no tool for that"}"#,
        );
        assert_eq!(decline.kind, ReplyKind::Decline);
        assert!(decline.args.is_empty());
        assert_eq!(decline.reason, "no tool for that");
        assert_eq!(step_for(&policy, &everyday(), &decline), Step::Explain);
        assert_eq!(
            step_for(&policy, &everyday(), &parse_reply("junk")),
            Step::Explain
        );
    }

    #[test]
    fn reading_runs_and_changing_is_shown_first() {
        let policy = RiskPolicy::embedded();
        let step =
            |text: &str, granted: &BTreeSet<Tier>| step_for(&policy, granted, &parse_reply(text));
        let observe: BTreeSet<Tier> = [Tier::Observe].into_iter().collect();
        assert_eq!(
            step(r#"{"tool":"status","args":[]}"#, &everyday()),
            Step::Run
        );
        assert_eq!(
            step(r#"{"tool":"volume-up","args":[]}"#, &everyday()),
            Step::AskFirst
        );
        assert_eq!(
            step(r#"{"tool":"session","args":["lock"]}"#, &everyday()),
            Step::AskFirst
        );
        assert_eq!(
            step(r#"{"tool":"session","args":["reboot"]}"#, &everyday()),
            Step::Refuse
        );
        assert_eq!(
            step(r#"{"tool":"made-up","args":[]}"#, &everyday()),
            Step::Refuse
        );
        assert_eq!(
            step(r#"{"tool":"volume-up","args":[]}"#, &observe),
            Step::Refuse
        );
        assert_eq!(step(r#"{"tool":"status","args":[]}"#, &observe), Step::Run);
        assert_eq!(
            step(r#"{"tool":"status","args":[]}"#, &BTreeSet::new()),
            Step::Refuse
        );
    }

    #[test]
    fn the_person_sees_the_exact_call() {
        let call = parse_reply(r#"{"tool":"panel-open","args":["control-center"]}"#);
        assert_eq!(describe(&call, None), "Run `panel-open control-center`");
        let hints = ToolHints::parse("[hints]\npower-cycle = \"Step to the next power profile\"\n")
            .unwrap();
        let restart = parse_reply(r#"{"tool":"power-cycle","args":["next"]}"#);
        assert_eq!(
            describe(&restart, Some(&hints)),
            "Step to the next power profile (`power-cycle next`)"
        );
        assert_eq!(
            describe(&call, Some(&hints)),
            "Run `panel-open control-center`"
        );
    }

    #[test]
    fn the_prompt_carries_tools_examples_and_the_request_last() {
        let messages = build_messages("- status\n", "Turn it up.");
        let list = messages.as_array().unwrap();
        assert_eq!(list.len(), 2 + 2 * EXAMPLES.len());
        assert_eq!(list[0]["role"], "system");
        assert!(list[0]["content"].as_str().unwrap().contains("- status"));
        assert_eq!(list.last().unwrap()["role"], "user");
        assert_eq!(list.last().unwrap()["content"], "Turn it up.");
    }

    #[test]
    fn the_examples_are_valid_replies_and_none_is_an_evaluation_case() {
        let declines = EXAMPLES
            .iter()
            .filter(|(_, answer)| parse_reply(answer).kind == ReplyKind::Decline)
            .count();
        assert!(
            declines >= 3,
            "the examples teach declining, not only acting"
        );
        for (ask, answer) in EXAMPLES {
            assert_ne!(parse_reply(answer).kind, ReplyKind::Invalid, "{ask}");
        }
        // An example that is also a test case would make the measurement meaningless.
        let root = concat!(env!("CARGO_MANIFEST_DIR"), "/../..");
        for file in ["desktop_requests.json", "desktop_requests_heldout.json"] {
            let text = std::fs::read_to_string(format!("{root}/tests/eval/{file}")).unwrap();
            let doc: Value = serde_json::from_str(&text).unwrap();
            for case in doc["cases"].as_array().unwrap() {
                let request = case["request"].as_str().unwrap();
                assert!(
                    EXAMPLES.iter().all(|(ask, _)| *ask != request),
                    "'{request}' is both a prompt example and a case in {file}"
                );
            }
        }
    }

    #[test]
    fn model_choice_prefers_the_persons_then_the_largest() {
        let models =
            json!([{"id": "small.gguf", "bytes": 1000}, {"id": "big.gguf", "bytes": 9000}]);
        assert_eq!(choose_model(&models, "").id, "big.gguf");
        assert_eq!(choose_model(&models, "small.gguf").id, "small.gguf");
        let missing = choose_model(&models, "missing.gguf");
        assert!(missing.id.is_empty() && !missing.error.is_empty());
        assert!(!choose_model(&json!([]), "").error.is_empty());
        assert!(!choose_model(&json!({"x": 1}), "").error.is_empty());
    }
}
