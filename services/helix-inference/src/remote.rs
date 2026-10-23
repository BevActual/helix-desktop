//! Talk, summarise, vision and code answered by a model on another machine or a hosted service, through an OpenAI-compatible `/chat/completions`.
//! Used only for what the person routed there (`inference-routing.toml`); the Act slot never comes here, no tools or output grammar are sent, and a
//! failure is an error to the caller, never a quiet retry somewhere else. The key is read here and goes no further than the backend it belongs to.
//!
//! A key is only sent over `https`, or `http` to a computer on the local network: never in the clear across the internet.

use std::io::{BufRead, BufReader};
use std::path::PathBuf;
use std::sync::mpsc;
use std::time::Duration;

use serde_json::{json, Value};

use crate::backend::*;
use crate::routing::{BackendInfo, RoutingTable};
use crate::server::{absorb, chat_messages};

pub struct OpenAiRemote;

fn state_dir() -> PathBuf {
    let var = |name: &str| std::env::var(name).ok().filter(|v| !v.is_empty());
    if let Some(helix) = var("HELIX_STATE_HOME") {
        return PathBuf::from(helix).join("helix");
    }
    if let Some(xdg) = var("XDG_STATE_HOME") {
        return PathBuf::from(xdg).join("helix");
    }
    var("HOME")
        .map(|h| PathBuf::from(h).join(".local/state/helix"))
        .unwrap_or_default()
}

/// The key for a backend: `<state>/secrets/backend-<ref>`, where the ref is the backend's `api_key_ref` or else its id. Nothing if there is none.
fn key_for(backend: &BackendInfo) -> Option<String> {
    let reference = if backend.api_key_ref.trim().is_empty() {
        backend.id.trim()
    } else {
        backend.api_key_ref.trim()
    };
    let plain = !reference.is_empty()
        && reference.len() <= 64
        && reference
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '-' | '_' | '.'));
    if !plain {
        return None;
    }
    let text = std::fs::read_to_string(
        state_dir()
            .join("secrets")
            .join(format!("backend-{reference}")),
    )
    .ok()?;
    let text = text.trim();
    (!text.is_empty()).then(|| text.to_string())
}

/// `host` of an `http(s)://host[:port]/...` URL.
fn host_of(url: &str) -> Option<&str> {
    let rest = url.split_once("://")?.1;
    let authority = rest.split(['/', '?', '#']).next()?;
    let authority = authority.rsplit_once('@').map_or(authority, |(_, h)| h);
    if let Some(v6) = authority.strip_prefix('[') {
        return v6.split(']').next();
    }
    Some(authority.rsplit_once(':').map_or(authority, |(h, port)| {
        if port.bytes().all(|b| b.is_ascii_digit()) {
            h
        } else {
            authority
        }
    }))
}

/// A name or address on this computer or its own network: loopback, private ranges, link-local, `.local`, a bare host name.
pub(crate) fn is_local_network(host: &str) -> bool {
    let host = host.to_ascii_lowercase();
    if host == "localhost"
        || host.ends_with(".local")
        || host.ends_with(".lan")
        || host.ends_with(".home.arpa")
        || !host.contains('.') && !host.contains(':')
    {
        return true;
    }
    if let Ok(ip) = host.parse::<std::net::IpAddr>() {
        return match ip {
            std::net::IpAddr::V4(v4) => v4.is_loopback() || v4.is_private() || v4.is_link_local(),
            std::net::IpAddr::V6(v6) => {
                v6.is_loopback()
                    || (v6.segments()[0] & 0xfe00) == 0xfc00
                    || (v6.segments()[0] & 0xffc0) == 0xfe80
            }
        };
    }
    false
}

/// The URL to post to, if this backend may be used: `https`, or `http` only to the local network.
fn endpoint(base_url: &str) -> Result<String, String> {
    let base = base_url.trim().trim_end_matches('/');
    let host = host_of(base).ok_or("the backend's address is not a web address")?;
    if base.starts_with("https://") || (base.starts_with("http://") && is_local_network(host)) {
        return Ok(format!("{base}/chat/completions"));
    }
    Err(
        "a backend on the internet must use https (a computer on your own network may use http)"
            .into(),
    )
}

fn request_body(request: &ChatRequest) -> Result<String, String> {
    if !request.json_schema.is_empty() {
        return Err("a model on another machine cannot be given an output grammar".into());
    }
    Ok(json!({
        "model": request.model,
        "messages": chat_messages(&request.messages, &request.images)?,
        "stream": true,
        "stream_options": {"include_usage": true},
        "max_tokens": request.max_tokens,
        "temperature": request.temperature,
    })
    .to_string())
}

enum Piece {
    Data(String),
    Done,
    Failed(String),
}

impl RemoteChat for OpenAiRemote {
    fn chat(
        &self,
        request: &ChatRequest,
        on_delta: &mut dyn FnMut(&str),
        cancelled: &dyn Fn() -> bool,
    ) -> Outcome {
        let fail = |error: String| Outcome {
            error,
            ..Outcome::default()
        };
        let routing = RoutingTable::load();
        let Some(backend) = routing.backend(&request.backend).cloned() else {
            return fail(format!("no such backend: {}", request.backend));
        };
        let url = match endpoint(&backend.base_url) {
            Ok(url) => url,
            Err(error) => return fail(error),
        };
        let body = match request_body(request) {
            Ok(body) => body,
            Err(error) => return fail(error),
        };
        let key = key_for(&backend);
        let timeout = Duration::from_secs(
            std::env::var("HELIX_REMOTE_TIMEOUT_SECONDS")
                .ok()
                .and_then(|v| v.parse().ok())
                .unwrap_or(120),
        );
        // The request runs on a thread so cancelling does not have to wait for the network; the thread ends when the connection does.
        let (send, receive) = mpsc::channel::<Piece>();
        std::thread::spawn(move || {
            let agent: ureq::Agent = ureq::Agent::config_builder()
                .timeout_global(Some(timeout))
                .http_status_as_error(false)
                .build()
                .into();
            let mut call = agent
                .post(&url)
                .header("Content-Type", "application/json")
                .header("Accept", "text/event-stream");
            if let Some(key) = &key {
                call = call.header("Authorization", &format!("Bearer {key}"));
            }
            let mut response = match call.send(body.as_str()) {
                Ok(response) => response,
                Err(error) => {
                    let _ = send.send(Piece::Failed(format!(
                        "the backend did not answer: {error}"
                    )));
                    return;
                }
            };
            let status = response.status().as_u16();
            if status != 200 {
                let text = response.body_mut().read_to_string().unwrap_or_default();
                let message = serde_json::from_str::<Value>(&text)
                    .ok()
                    .and_then(|v| {
                        v.pointer("/error/message")
                            .and_then(Value::as_str)
                            .map(str::to_string)
                    })
                    .unwrap_or_else(|| format!("the backend refused the request ({status})"));
                let _ = send.send(Piece::Failed(message));
                return;
            }
            for line in BufReader::new(response.body_mut().as_reader()).lines() {
                let Ok(line) = line else {
                    let _ = send.send(Piece::Failed("the connection to the backend broke".into()));
                    return;
                };
                if let Some(data) = line.strip_prefix("data:") {
                    let data = data.trim_start();
                    if data == "[DONE]" {
                        break;
                    }
                    if send.send(Piece::Data(data.to_string())).is_err() {
                        return; // nobody is listening any more
                    }
                }
            }
            let _ = send.send(Piece::Done);
        });
        let mut outcome = Outcome::default();
        let (mut finish, mut spoke) = (String::new(), false);
        let result = loop {
            if cancelled() {
                outcome.cancelled = true;
                break Ok(());
            }
            match receive.recv_timeout(Duration::from_millis(100)) {
                Ok(Piece::Data(data)) => {
                    absorb(&data, &mut outcome, &mut finish, &mut spoke, on_delta)
                }
                Ok(Piece::Done) | Err(mpsc::RecvTimeoutError::Disconnected) => break Ok(()),
                Ok(Piece::Failed(error)) => break Err(error),
                Err(mpsc::RecvTimeoutError::Timeout) => {}
            }
        };
        match result {
            Ok(()) => {
                outcome.ok = !outcome.cancelled;
                outcome.truncated = finish == "length";
            }
            Err(error) => outcome.error = error,
        }
        // The audit line: who asked, where it went, what it cost. Never the prompt or the reply.
        eprintln!(
            "helix-inference: remote chat client={} backend={} model={} prompt_tokens={} completion_tokens={} ok={}",
            request.client, backend.id, request.model, outcome.prompt_tokens, outcome.completion_tokens, outcome.ok
        );
        outcome
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The tests that point the state folder somewhere set a process-wide variable, so they take turns.
    static STATE: std::sync::Mutex<()> = std::sync::Mutex::new(());
    fn state_lock() -> std::sync::MutexGuard<'static, ()> {
        STATE.lock().unwrap_or_else(|e| e.into_inner())
    }

    #[test]
    fn a_key_only_goes_over_https_or_to_the_local_network() {
        assert!(endpoint("https://openrouter.ai/api/v1").is_ok());
        assert!(endpoint("http://192.168.1.20:8080/v1").is_ok());
        assert!(endpoint("http://localhost:11434/v1").is_ok());
        assert!(endpoint("http://my-desktop.local:8080/v1").is_ok());
        assert!(endpoint("http://studio:8080/v1").is_ok());
        assert!(endpoint("http://[fd00::5]:8080/v1").is_ok());
        assert!(endpoint("http://api.example.com/v1").is_err());
        assert!(endpoint("http://8.8.8.8/v1").is_err());
        assert!(endpoint("ftp://x/v1").is_err());
        assert!(endpoint("not a url").is_err());
        // Credentials in the address do not change which host it is.
        assert!(endpoint("http://192.168.1.2@evil.example.com/v1").is_err());
        assert_eq!(
            endpoint("https://x.test/v1/").unwrap(),
            "https://x.test/v1/chat/completions"
        );
    }

    #[test]
    fn a_grammar_is_not_sent_to_another_machine() {
        let request = ChatRequest {
            messages: json!([{"role": "user", "content": "hi"}]),
            json_schema: "{}".into(),
            ..ChatRequest::default()
        };
        assert!(request_body(&request).is_err());
    }

    #[test]
    fn keys_are_found_by_reference_or_by_backend_id() {
        let _turn = state_lock();
        let dir = std::env::temp_dir().join(format!("helix-remote-keys-{}", std::process::id()));
        let secrets = dir.join("helix/secrets");
        std::fs::create_dir_all(&secrets).unwrap();
        std::fs::write(secrets.join("backend-openrouter"), "sk-test\n").unwrap();
        std::env::set_var("HELIX_STATE_HOME", &dir);
        let by_id = BackendInfo {
            id: "openrouter".into(),
            ..BackendInfo::default()
        };
        let by_ref = BackendInfo {
            id: "other".into(),
            api_key_ref: "openrouter".into(),
            ..BackendInfo::default()
        };
        let none = BackendInfo {
            id: "lan".into(),
            ..BackendInfo::default()
        };
        let sneaky = BackendInfo {
            id: "x".into(),
            api_key_ref: "../../etc/passwd".into(),
            ..BackendInfo::default()
        };
        assert_eq!(key_for(&by_id).as_deref(), Some("sk-test"));
        assert_eq!(key_for(&by_ref).as_deref(), Some("sk-test"));
        assert_eq!(key_for(&none), None);
        assert_eq!(
            key_for(&sneaky),
            None,
            "a reference is a short plain name, never a path"
        );
        std::env::remove_var("HELIX_STATE_HOME");
        let _ = std::fs::remove_dir_all(&dir);
    }

    // --- against a scripted server --------------------------------------------------------------------------------------------------

    const STUB: &str = r#"
import json, sys, time
from http.server import BaseHTTPRequestHandler, HTTPServer
log = sys.argv[1]
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        open(log, "w").write(json.dumps({"path": self.path, "auth": self.headers.get("Authorization"), "body": json.loads(body)}))
        if self.headers.get("Authorization") != "Bearer sk-test":
            data = json.dumps({"error": {"message": "bad key"}}).encode()
            self.send_response(401); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data); return
        self.send_response(200); self.send_header("Content-Type", "text/event-stream"); self.end_headers()
        slow = json.loads(body).get("model") == "slow"
        for piece in ["Hel", "lo ", "there"]:
            self.wfile.write(("data: " + json.dumps({"choices": [{"delta": {"content": piece}}]}) + "\n\n").encode()); self.wfile.flush()
            if slow: time.sleep(5)
        self.wfile.write(("data: " + json.dumps({"choices": [{"delta": {}, "finish_reason": "stop"}], "usage": {"prompt_tokens": 7, "completion_tokens": 3}}) + "\n\n").encode())
        self.wfile.write(b"data: [DONE]\n\n"); self.wfile.flush()
HTTPServer(("127.0.0.1", int(sys.argv[2])), H).serve_forever()
"#;

    /// Runs the scripted server and points the routing and the key at it; returns what is needed to stop and inspect.
    fn stage(name: &str) -> (std::process::Child, PathBuf, PathBuf) {
        let dir = std::env::temp_dir().join(format!("helix-remote-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let secrets = dir.join("helix/secrets");
        std::fs::create_dir_all(&secrets).unwrap();
        std::fs::write(secrets.join("backend-lan"), "sk-test").unwrap();
        let port = std::net::TcpListener::bind("127.0.0.1:0")
            .unwrap()
            .local_addr()
            .unwrap()
            .port();
        std::fs::write(
            dir.join("helix/inference-routing.toml"),
            format!("[[backends]]\nid = \"lan\"\nkind = \"openai\"\nbase_url = \"http://127.0.0.1:{port}/v1\"\n[[backends]]\nid = \"nokey\"\nkind = \"openai\"\nbase_url = \"http://127.0.0.1:{port}/v1\"\n"),
        )
        .unwrap();
        std::env::set_var("HELIX_STATE_HOME", &dir);
        let log = dir.join("seen.json");
        let child = std::process::Command::new("python3")
            .args(["-c", STUB, &log.to_string_lossy(), &port.to_string()])
            .spawn()
            .unwrap();
        for _ in 0..100 {
            if std::net::TcpStream::connect(("127.0.0.1", port)).is_ok() {
                break;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
        (child, dir, log)
    }

    fn ask(backend: &str, model: &str) -> ChatRequest {
        ChatRequest {
            client: "test".into(),
            backend: backend.into(),
            model: model.into(),
            messages: json!([{"role": "user", "content": "hi"}]),
            ..ChatRequest::default()
        }
    }

    #[test]
    fn a_remote_model_streams_with_its_key_and_reports_its_cost() {
        let _turn = state_lock();
        if std::process::Command::new("python3")
            .arg("--version")
            .output()
            .is_err()
        {
            return;
        }
        let (mut stub, dir, log) = stage("stream");
        let mut pieces = String::new();
        let out = OpenAiRemote.chat(&ask("lan", "big"), &mut |d| pieces.push_str(d), &|| false);
        assert!(out.ok, "{}", out.error);
        assert_eq!(pieces, "Hello there");
        assert_eq!(out.text, "Hello there");
        assert_eq!((out.prompt_tokens, out.completion_tokens), (7, 3));
        let seen: Value = serde_json::from_str(&std::fs::read_to_string(&log).unwrap()).unwrap();
        assert_eq!(seen["path"], "/v1/chat/completions");
        assert_eq!(seen["auth"], "Bearer sk-test");
        assert_eq!(seen["body"]["model"], "big");
        assert_eq!(seen["body"]["stream"], true);
        assert!(seen["body"].get("grammar").is_none() && seen["body"].get("tools").is_none());
        // A backend without a key is refused by the server, and that is said plainly.
        let out = OpenAiRemote.chat(&ask("nokey", "big"), &mut |_| {}, &|| false);
        assert!(!out.ok && out.error == "bad key", "{}", out.error);
        // A backend that is not configured is an error, not a fall back to something else.
        let out = OpenAiRemote.chat(&ask("elsewhere", "big"), &mut |_| {}, &|| false);
        assert!(out.error.contains("no such backend"));
        let _ = stub.kill();
        let _ = stub.wait();
        std::env::remove_var("HELIX_STATE_HOME");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn cancelling_a_remote_answer_returns_at_once() {
        let _turn = state_lock();
        if std::process::Command::new("python3")
            .arg("--version")
            .output()
            .is_err()
        {
            return;
        }
        let (mut stub, dir, _log) = stage("cancel");
        let started = std::time::Instant::now();
        let heard = std::cell::Cell::new(false);
        let out = OpenAiRemote.chat(&ask("lan", "slow"), &mut |_| heard.set(true), &|| {
            heard.get()
        });
        assert!(out.cancelled && !out.ok && out.text == "Hel", "{out:?}");
        assert!(
            started.elapsed() < Duration::from_secs(3),
            "did not wait for the slow server"
        );
        let _ = stub.kill();
        let _ = stub.wait();
        std::env::remove_var("HELIX_STATE_HOME");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
