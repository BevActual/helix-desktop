//! Runs the client against a pretend agent (`fake_agent.py`) that behaves, and misbehaves, as the scenario says. Needs `python3`.

use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use helix_acp::*;
use serde_json::{json, Value};
use tokio::sync::mpsc::Receiver;

fn agent(scenario: &str, log: Option<&PathBuf>) -> Spawn {
    let script = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fake_agent.py");
    let mut env = vec![("FAKE".to_string(), scenario.to_string())];
    if let Some(log) = log {
        env.push(("FAKE_LOG".to_string(), log.to_string_lossy().to_string()));
    }
    Spawn {
        command: "python3".into(),
        args: vec![script.to_string_lossy().to_string()],
        cwd: None,
        env,
        clear_env: false,
    }
}

fn info() -> ClientInfo {
    ClientInfo {
        name: "helix-test".into(),
        title: "Helix test".into(),
        version: "0".into(),
    }
}

fn caps() -> ClientCapabilities {
    ClientCapabilities {
        fs: FsCapabilities {
            read_text_file: true,
            write_text_file: false,
        },
        terminal: false,
    }
}

/// Answers like a small host: allows once, reads one known file, and delays when asked to.
struct Host {
    seen: Mutex<Vec<AgentRequest>>,
    delay: Duration,
}

impl Handler for Host {
    fn handle(&self, request: AgentRequest) -> BoxFuture<'_, Result<Value, RpcError>> {
        self.seen.lock().unwrap().push(request.clone());
        let delay = self.delay;
        Box::pin(async move {
            tokio::time::sleep(delay).await;
            match request {
                AgentRequest::RequestPermission(p) => {
                    Ok(PermissionOutcome::Selected(p.options[0].id.clone()).to_value())
                }
                AgentRequest::ReadTextFile(r) if r.path == "/work/a.rs" => {
                    Ok(json!({"content": "fn main() {}"}))
                }
                AgentRequest::ReadTextFile(_) => {
                    Err(RpcError::new(RpcError::RESOURCE_NOT_FOUND, "No such file"))
                }
                _ => Err(RpcError::refused("not in this test")),
            }
        })
    }
}

fn host(delay_ms: u64) -> Arc<Host> {
    Arc::new(Host {
        seen: Mutex::new(Vec::new()),
        delay: Duration::from_millis(delay_ms),
    })
}

async fn start(
    scenario: &str,
    log: Option<&PathBuf>,
    h: Arc<Host>,
) -> (Client, Receiver<Event>, InitializeResponse) {
    let (client, events) = Client::spawn(agent(scenario, log), h).expect("the agent starts");
    let hello = client.initialize(info(), caps()).await.expect("initialize");
    (client, events, hello)
}

async fn drain(events: &mut Receiver<Event>, until_empty_for_ms: u64) -> Vec<Event> {
    let mut out = Vec::new();
    while let Ok(Some(e)) =
        tokio::time::timeout(Duration::from_millis(until_empty_for_ms), events.recv()).await
    {
        out.push(e);
    }
    out
}

fn updates(events: &[Event]) -> Vec<&SessionUpdate> {
    events
        .iter()
        .filter_map(|e| {
            if let Event::Update { update, .. } = e {
                Some(update)
            } else {
                None
            }
        })
        .collect()
}

fn text_of(update: &SessionUpdate) -> Option<String> {
    match update {
        SessionUpdate::AgentMessage(ContentBlock::Text { text })
        | SessionUpdate::AgentThought(ContentBlock::Text { text }) => Some(text.clone()),
        _ => None,
    }
}

#[tokio::test(flavor = "multi_thread")]
async fn a_whole_turn_reports_everything_and_answers_the_agents_requests() {
    let h = host(0);
    let (client, mut events, hello) = start("turn", None, h.clone()).await;
    assert_eq!(hello.agent_info.as_ref().unwrap().name, "fake-agent");
    assert_eq!(hello.auth_methods[0].id, "login");
    let session = client.new_session("/work").await.unwrap();
    assert_eq!(session.session_id, "s1");
    let stop = client
        .prompt("s1", vec![ContentBlock::text("run the tests")])
        .await
        .unwrap();
    assert_eq!(stop, StopReason::EndTurn);
    let seen = drain(&mut events, 300).await;
    let all = updates(&seen);
    assert!(matches!(all[0], SessionUpdate::AgentThought(_)), "{all:?}");
    assert_eq!(text_of(all[1]).as_deref(), Some("Hello"));
    let SessionUpdate::ToolCall(call) = all[2] else {
        panic!("{:?}", all[2])
    };
    assert_eq!(
        (call.id.as_str(), call.kind, call.status),
        ("t1", ToolKind::Execute, ToolStatus::Pending)
    );
    assert_eq!(
        call.locations[0],
        ToolLocation {
            path: "/work/a.rs".into(),
            line: Some(3)
        }
    );
    assert!(
        matches!(all[3], SessionUpdate::ToolCallUpdate(u) if u.status == Some(ToolStatus::InProgress))
    );
    assert!(
        matches!(all[4], SessionUpdate::ToolCallUpdate(u) if u.status == Some(ToolStatus::Completed))
    );
    // the agent was answered: its file read got the content, and its permission request got the chosen option
    assert_eq!(text_of(all[5]).as_deref(), Some("read:fn main() {}"));
    assert!(
        text_of(all[6]).unwrap().contains("\"selected\"")
            && text_of(all[6]).unwrap().contains("yes")
    );
    assert!(matches!(all[7], SessionUpdate::Plan(p) if p[0].content == "Run tests"));
    assert_eq!(
        *all[8],
        SessionUpdate::Usage {
            used: 1200,
            size: 200_000
        }
    );
    assert!(
        matches!(all[9], SessionUpdate::Other(v) if v["sessionUpdate"] == "some_future_update"),
        "an update it does not know is kept, not an error"
    );
    assert_eq!(h.seen.lock().unwrap().len(), 2);
    client.close().await;
}

#[tokio::test(flavor = "multi_thread")]
async fn a_turn_can_be_cancelled_and_the_agent_says_so() {
    let (client, mut events, _) = start("cancel", None, host(0)).await;
    client.new_session("/work").await.unwrap();
    let client = Arc::new(client);
    let waiting = {
        let c = client.clone();
        tokio::spawn(async move { c.prompt("s1", vec![ContentBlock::text("go")]).await })
    };
    // the first word of the turn arrives, then the person stops it
    let first = tokio::time::timeout(Duration::from_secs(5), events.recv())
        .await
        .unwrap()
        .unwrap();
    assert!(matches!(first, Event::Update { .. }));
    client.cancel("s1");
    assert_eq!(
        tokio::time::timeout(Duration::from_secs(5), waiting)
            .await
            .unwrap()
            .unwrap()
            .unwrap(),
        StopReason::Cancelled
    );
    client.close().await;
}

#[tokio::test(flavor = "multi_thread")]
async fn an_agent_that_dies_mid_turn_is_an_error_not_a_hang() {
    let (client, mut events, _) = start("crash", None, host(0)).await;
    client.new_session("/work").await.unwrap();
    let result = tokio::time::timeout(
        Duration::from_secs(5),
        client.prompt("s1", vec![ContentBlock::text("go")]),
    )
    .await
    .expect("it must not hang");
    assert!(matches!(result, Err(Error::Exited(_))), "{result:?}");
    let seen = drain(&mut events, 300).await;
    assert!(seen.iter().any(|e| matches!(e, Event::Exited(_))));
    assert!(
        matches!(client.new_session("/work").await, Err(Error::Exited(_))),
        "and later calls fail at once"
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn things_an_agent_must_not_write_end_the_session() {
    for scenario in ["bad_line", "huge_line", "nonrpc"] {
        let (client, mut events, _) = start(scenario, None, host(0)).await;
        let seen = drain(&mut events, 1500).await;
        assert!(
            seen.iter().any(|e| matches!(e, Event::ProtocolError(_))),
            "{scenario}: {seen:?}"
        );
        let later = client.new_session("/work").await;
        assert!(
            matches!(later, Err(Error::Protocol(_)) | Err(Error::Exited(_))),
            "{scenario}: {later:?}"
        );
        client.close().await;
    }
}

#[tokio::test(flavor = "multi_thread")]
async fn an_agent_on_another_protocol_version_is_refused() {
    let (client, _events) = Client::spawn(agent("wrongversion", None), host(0)).unwrap();
    let result = client.initialize(info(), caps()).await;
    assert!(
        matches!(result, Err(Error::Protocol(m)) if m.contains("version 2")),
        "wrong version"
    );
    client.close().await;
}

#[tokio::test(flavor = "multi_thread")]
async fn a_request_it_does_not_know_is_answered_as_not_supported() {
    let (client, mut events, _) = start("unknown", None, host(0)).await;
    client.new_session("/work").await.unwrap();
    client
        .prompt("s1", vec![ContentBlock::text("go")])
        .await
        .unwrap();
    let seen = drain(&mut events, 300).await;
    // the agent reports the code it was answered with: -32601, "method not found"
    assert!(
        updates(&seen)
            .iter()
            .filter_map(|u| text_of(u))
            .any(|t| t == "unknown:-32601"),
        "{seen:?}"
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn a_flood_of_updates_is_all_delivered_without_losing_any() {
    let (client, mut events, _) = start("flood", None, host(0)).await;
    client.new_session("/work").await.unwrap();
    let client = Arc::new(client);
    let turn = {
        let c = client.clone();
        tokio::spawn(async move { c.prompt("s1", vec![ContentBlock::text("go")]).await })
    };
    let mut count = 0;
    while count < 3000 {
        match tokio::time::timeout(Duration::from_secs(20), events.recv()).await {
            Ok(Some(Event::Update { .. })) => count += 1,
            Ok(Some(_)) => {}
            other => panic!("stopped at {count}: {other:?}"),
        }
    }
    assert_eq!(turn.await.unwrap().unwrap(), StopReason::EndTurn);
    // by the time the turn's answer came, every update had been counted, so a host can wait for exactly that many
    assert_eq!(client.updates_received(), 3000);
}

#[tokio::test(flavor = "multi_thread")]
async fn only_a_few_of_the_agents_requests_are_answered_at_once() {
    let (client, mut events, _) = start("burst", None, host(400)).await;
    client.new_session("/work").await.unwrap();
    client
        .prompt("s1", vec![ContentBlock::text("go")])
        .await
        .unwrap();
    let seen = drain(&mut events, 300).await;
    let last = updates(&seen)
        .into_iter()
        .filter_map(text_of)
        .next_back()
        .unwrap();
    let (ok, refused) = {
        let numbers: Vec<usize> = last
            .split(|c: char| !c.is_ascii_digit())
            .filter(|s| !s.is_empty())
            .map(|s| s.parse().unwrap())
            .collect();
        (numbers[0], numbers[1])
    };
    assert_eq!(ok + refused, 40);
    assert!((MAX_AGENT_REQUESTS..40).contains(&ok), "{last}");
    assert!(refused >= 10, "{last}");
}

#[tokio::test(flavor = "multi_thread")]
async fn a_host_that_offers_nothing_refuses_everything() {
    let (client, mut events) = Client::spawn(agent("turn", None), Arc::new(RefuseAll)).unwrap();
    client.initialize(info(), caps()).await.unwrap();
    client.new_session("/work").await.unwrap();
    client
        .prompt("s1", vec![ContentBlock::text("go")])
        .await
        .ok();
    let seen = drain(&mut events, 300).await;
    assert!(
        updates(&seen)
            .iter()
            .filter_map(|u| text_of(u))
            .any(|t| t.starts_with("read:The host does not allow that.")),
        "{seen:?}"
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn a_terminal_request_reaches_the_host_typed() {
    let h = host(0);
    let (client, _events, _) = start("terminal", None, h.clone()).await;
    client.new_session("/work").await.unwrap();
    client
        .prompt("s1", vec![ContentBlock::text("go")])
        .await
        .unwrap();
    let seen = h.seen.lock().unwrap();
    let AgentRequest::CreateTerminal(t) = &seen[0] else {
        panic!("{seen:?}")
    };
    assert_eq!(
        (t.command.as_str(), t.args.clone(), t.cwd.clone()),
        ("ls", vec!["-l".to_string()], Some("/work".to_string()))
    );
}

// ---- the published schema -----------------------------------------------------------------------------------------------------------

/// Checks `value` against the part of JSON Schema the published file uses: `$ref`, `allOf`, `oneOf`, `anyOf`, `const`, `enum`, `type`,
/// `required`, `properties` and `items`.
fn conforms(defs: &Value, node: &Value, value: &Value, path: &str) -> Result<(), String> {
    if let Some(reference) = node.get("$ref").and_then(Value::as_str) {
        let name = reference.rsplit('/').next().unwrap();
        return conforms(
            defs,
            defs.get(name)
                .ok_or(format!("{path}: no definition {name}"))?,
            value,
            path,
        );
    }
    if let Some(all) = node.get("allOf").and_then(Value::as_array) {
        for part in all {
            conforms(defs, part, value, path)?;
        }
    }
    for key in ["oneOf", "anyOf"] {
        if let Some(options) = node.get(key).and_then(Value::as_array) {
            let errors: Vec<String> = options
                .iter()
                .map(|o| conforms(defs, o, value, path))
                .filter_map(Result::err)
                .collect();
            if errors.len() == options.len() {
                return Err(format!(
                    "{path}: matches none of {key}: {}",
                    errors.join(" | ")
                ));
            }
        }
    }
    if let Some(constant) = node.get("const") {
        if constant != value {
            return Err(format!("{path}: expected {constant}, got {value}"));
        }
    }
    if let Some(allowed) = node.get("enum").and_then(Value::as_array) {
        if !allowed.contains(value) {
            return Err(format!("{path}: {value} is not one of {allowed:?}"));
        }
    }
    if let Some(kind) = node.get("type") {
        let kinds: Vec<&str> = match kind {
            Value::String(k) => vec![k.as_str()],
            Value::Array(list) => list.iter().filter_map(Value::as_str).collect(),
            _ => vec![],
        };
        let fits = |k: &str| match k {
            "string" => value.is_string(),
            "integer" => value.is_i64() || value.is_u64(),
            "number" => value.is_number(),
            "boolean" => value.is_boolean(),
            "array" => value.is_array(),
            "object" => value.is_object(),
            "null" => value.is_null(),
            _ => true,
        };
        if !kinds.is_empty() && !kinds.iter().any(|k| fits(k)) {
            return Err(format!("{path}: {value} is not of type {kinds:?}"));
        }
    }
    if let Some(object) = value.as_object() {
        for required in node
            .get("required")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
            .filter_map(Value::as_str)
        {
            if !object.contains_key(required) {
                return Err(format!("{path}: missing required `{required}`"));
            }
        }
        if let Some(properties) = node.get("properties").and_then(Value::as_object) {
            for (name, child) in properties {
                if let Some(present) = object.get(name) {
                    conforms(defs, child, present, &format!("{path}.{name}"))?;
                }
            }
        }
    }
    if let (Some(items), Some(list)) = (node.get("items"), value.as_array()) {
        for (i, item) in list.iter().enumerate() {
            conforms(defs, items, item, &format!("{path}[{i}]"))?;
        }
    }
    Ok(())
}

#[tokio::test(flavor = "multi_thread")]
async fn what_the_client_sends_has_the_shape_the_protocol_publishes() {
    let schema: Value = serde_json::from_str(include_str!("../schema/v1.json")).unwrap();
    let defs = schema["$defs"].clone();
    let log = std::env::temp_dir().join(format!("helix-acp-log-{}", std::process::id()));
    let _ = std::fs::remove_file(&log);
    let (client, mut events, _) = start("turn", Some(&log), host(0)).await;
    client.new_session("/work").await.unwrap();
    client
        .prompt(
            "s1",
            vec![
                ContentBlock::text("hello"),
                ContentBlock::ResourceLink {
                    uri: "file:///work/a.rs".into(),
                    name: "a.rs".into(),
                },
            ],
        )
        .await
        .unwrap();
    client.cancel("s1");
    drain(&mut events, 300).await;
    client.close().await;
    let lines: Vec<Value> = std::fs::read_to_string(&log)
        .unwrap()
        .lines()
        .map(|l| serde_json::from_str(l).unwrap())
        .collect();
    let mut checked = Vec::new();
    for line in &lines {
        let (definition, part) = match line.get("method").and_then(Value::as_str) {
            Some("initialize") => ("InitializeRequest", &line["params"]),
            Some("session/new") => ("NewSessionRequest", &line["params"]),
            Some("session/prompt") => ("PromptRequest", &line["params"]),
            Some("session/cancel") => ("CancelNotification", &line["params"]),
            None if line["id"] == 100 => ("RequestPermissionResponse", &line["result"]),
            None if line["id"] == 101 => ("ReadTextFileResponse", &line["result"]),
            _ => continue,
        };
        conforms(&defs, &defs[definition], part, definition)
            .unwrap_or_else(|e| panic!("{definition}: {e}\n{line}"));
        checked.push(definition);
    }
    for expected in [
        "InitializeRequest",
        "NewSessionRequest",
        "PromptRequest",
        "CancelNotification",
        "RequestPermissionResponse",
        "ReadTextFileResponse",
    ] {
        assert!(
            checked.contains(&expected),
            "never saw {expected}: {checked:?}"
        );
    }
    // and the checker itself can fail: a prompt without its session id does not conform
    assert!(conforms(
        &defs,
        &defs["PromptRequest"],
        &json!({"prompt": []}),
        "PromptRequest"
    )
    .is_err());
    assert!(conforms(&defs, &defs["StopReason"], &json!("whenever"), "StopReason").is_err());
}
