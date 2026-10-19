//! Drives an agent as a subprocess: launches it, speaks newline-framed JSON-RPC 2.0 on its standard input and output, answers the agent's
//! requests through a [`Handler`], and reports what it does as [`Event`]s.
//!
//! The agent is not trusted. A line that is not a JSON-RPC message, or is larger than [`MAX_LINE`], ends the session with a protocol error.
//! Requests from the agent are answered by the host's handler, at most [`MAX_AGENT_REQUESTS`] at a time; one the crate does not know is
//! answered "not supported". The queue of events is bounded, so an agent that floods updates is slowed down rather than allowed to fill memory.

use std::collections::HashMap;
use std::future::Future;
use std::path::PathBuf;
use std::pin::Pin;
use std::process::Stdio;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde_json::{json, Value};
use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};
use tokio::process::Command;
use tokio::sync::{mpsc, oneshot, Semaphore};

use crate::types::*;

/// The longest line accepted from the agent, in bytes.
pub const MAX_LINE: usize = 4 * 1024 * 1024;
/// How many of the agent's requests the host is asked to answer at once.
pub const MAX_AGENT_REQUESTS: usize = 16;
const EVENT_QUEUE: usize = 1024;
const STDERR_LINE: usize = 8 * 1024;

pub type BoxFuture<'a, T> = Pin<Box<dyn Future<Output = T> + Send + 'a>>;

/// What answers the agent's requests: reads and writes of files, terminals, and permission. The host decides each one.
pub trait Handler: Send + Sync + 'static {
    fn handle(&self, request: AgentRequest) -> BoxFuture<'_, Result<Value, RpcError>>;
}

/// A handler that refuses everything. The default for a host that offers the agent nothing.
pub struct RefuseAll;

impl Handler for RefuseAll {
    fn handle(&self, _request: AgentRequest) -> BoxFuture<'_, Result<Value, RpcError>> {
        Box::pin(async { Err(RpcError::refused("The host does not allow that.")) })
    }
}

#[derive(Debug)]
pub enum Event {
    Update {
        session_id: String,
        update: SessionUpdate,
    },
    /// A line the agent wrote to its error output.
    Stderr(String),
    /// The agent broke the protocol; the session is over.
    ProtocolError(String),
    /// The agent's process ended.
    Exited(String),
}

#[derive(Debug, Clone, PartialEq)]
pub enum Error {
    /// The agent answered a request with an error.
    Rpc(RpcError),
    /// The agent broke the protocol.
    Protocol(String),
    /// The agent's process ended before answering.
    Exited(String),
    Timeout,
    Io(String),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Rpc(e) => write!(f, "The agent said: {}", e.message),
            Error::Protocol(m) => write!(f, "The agent broke the protocol: {m}"),
            Error::Exited(m) => write!(f, "The agent stopped: {m}"),
            Error::Timeout => write!(f, "The agent did not answer in time."),
            Error::Io(m) => write!(f, "{m}"),
        }
    }
}

impl std::error::Error for Error {}

/// How to start the agent.
#[derive(Clone, Debug, Default)]
pub struct Spawn {
    pub command: String,
    pub args: Vec<String>,
    pub cwd: Option<PathBuf>,
    /// Added to the agent's environment.
    pub env: Vec<(String, String)>,
    /// Start from an empty environment instead of this process's.
    pub clear_env: bool,
}

type Pending = Arc<Mutex<HashMap<u64, oneshot::Sender<Result<Value, Error>>>>>;

struct Shared {
    pending: Pending,
    /// Why the session is over, once it is.
    closed: Mutex<Option<Error>>,
}

impl Shared {
    fn close(&self, error: Error) {
        let mut closed = self.closed.lock().expect("closed");
        if closed.is_none() {
            *closed = Some(error.clone());
        }
        drop(closed);
        for (_, waiter) in self.pending.lock().expect("pending").drain() {
            let _ = waiter.send(Err(error.clone()));
        }
    }

    fn closed(&self) -> Option<Error> {
        self.closed.lock().expect("closed").clone()
    }
}

pub struct Client {
    writer: mpsc::Sender<String>,
    shared: Arc<Shared>,
    next_id: AtomicU64,
    kill: mpsc::Sender<()>,
}

fn message(value: Value) -> String {
    // serde_json writes one line: it escapes newlines inside strings, which the framing needs
    value.to_string()
}

impl Client {
    /// Launches the agent. Events arrive on the returned receiver until the session ends.
    pub fn spawn(
        spec: Spawn,
        handler: Arc<dyn Handler>,
    ) -> Result<(Client, mpsc::Receiver<Event>), Error> {
        let mut command = Command::new(&spec.command);
        command
            .args(&spec.args)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .kill_on_drop(true);
        if let Some(cwd) = &spec.cwd {
            command.current_dir(cwd);
        }
        if spec.clear_env {
            command.env_clear();
        }
        command.envs(spec.env.iter().map(|(k, v)| (k.as_str(), v.as_str())));
        let mut child = command
            .spawn()
            .map_err(|e| Error::Io(format!("The agent could not be started: {e}")))?;
        let (mut stdin, stdout, stderr) =
            match (child.stdin.take(), child.stdout.take(), child.stderr.take()) {
                (Some(i), Some(o), Some(e)) => (i, o, e),
                _ => return Err(Error::Io("The agent's pipes could not be opened.".into())),
            };

        let shared = Arc::new(Shared {
            pending: Arc::new(Mutex::new(HashMap::new())),
            closed: Mutex::new(None),
        });
        let (events, receiver) = mpsc::channel::<Event>(EVENT_QUEUE);
        let (writer, mut to_write) = mpsc::channel::<String>(256);
        let (kill, mut killed) = mpsc::channel::<()>(1);

        // writing: one task owns the agent's input, so lines never interleave
        let write_shared = shared.clone();
        tokio::spawn(async move {
            while let Some(line) = to_write.recv().await {
                let mut bytes = line.into_bytes();
                bytes.push(b'\n');
                if stdin.write_all(&bytes).await.is_err() || stdin.flush().await.is_err() {
                    write_shared.close(Error::Exited("its input closed".into()));
                    break;
                }
            }
        });

        // the agent's own logging
        let log_events = events.clone();
        tokio::spawn(async move {
            let mut lines = BufReader::new(stderr).lines();
            while let Ok(Some(line)) = lines.next_line().await {
                let short: String = line.chars().take(STDERR_LINE).collect();
                let _ = log_events.try_send(Event::Stderr(short));
            }
        });

        // reading
        let read_shared = shared.clone();
        let read_events = events.clone();
        let read_writer = writer.clone();
        let slots = Arc::new(Semaphore::new(MAX_AGENT_REQUESTS));
        tokio::spawn(read_loop(
            stdout,
            read_shared,
            read_events,
            read_writer,
            handler,
            slots,
        ));

        // the process: ends it on request, and says when it ended
        let exit_shared = shared.clone();
        let exit_events = events;
        tokio::spawn(async move {
            let status = tokio::select! {
                status = child.wait() => status.map(|s| s.to_string()).unwrap_or_else(|e| e.to_string()),
                _ = killed.recv() => {
                    let _ = child.kill().await;
                    "it was stopped".to_string()
                }
            };
            exit_shared.close(Error::Exited(status.clone()));
            let _ = exit_events.send(Event::Exited(status)).await;
        });

        Ok((
            Client {
                writer,
                shared,
                next_id: AtomicU64::new(1),
                kill,
            },
            receiver,
        ))
    }

    async fn call(
        &self,
        method: &str,
        params: Value,
        timeout: Option<Duration>,
    ) -> Result<Value, Error> {
        if let Some(error) = self.shared.closed() {
            return Err(error);
        }
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let (waiter, answer) = oneshot::channel();
        self.shared
            .pending
            .lock()
            .expect("pending")
            .insert(id, waiter);
        // closed between the check and the insert: the answer would never come
        if let Some(error) = self.shared.closed() {
            self.shared.pending.lock().expect("pending").remove(&id);
            return Err(error);
        }
        let line = message(json!({"jsonrpc": "2.0", "id": id, "method": method, "params": params}));
        if self.writer.send(line).await.is_err() {
            self.shared.pending.lock().expect("pending").remove(&id);
            return Err(self
                .shared
                .closed()
                .unwrap_or(Error::Exited("its input closed".into())));
        }
        let result = match timeout {
            Some(limit) => match tokio::time::timeout(limit, answer).await {
                Ok(r) => r,
                Err(_) => {
                    self.shared.pending.lock().expect("pending").remove(&id);
                    return Err(Error::Timeout);
                }
            },
            None => answer.await,
        };
        result.unwrap_or_else(|_| {
            Err(self
                .shared
                .closed()
                .unwrap_or(Error::Exited("it ended".into())))
        })
    }

    fn notify(&self, method: &str, params: Value) {
        let _ = self.writer.try_send(message(
            json!({"jsonrpc": "2.0", "method": method, "params": params}),
        ));
    }

    /// Agrees a version and what each side can do. Fails if the agent speaks another version of the protocol.
    pub async fn initialize(
        &self,
        info: ClientInfo,
        capabilities: ClientCapabilities,
    ) -> Result<InitializeResponse, Error> {
        let request = InitializeRequest {
            protocol_version: PROTOCOL_VERSION,
            client_capabilities: capabilities,
            client_info: info,
        };
        let value = self
            .call(
                "initialize",
                serde_json::to_value(request).map_err(|e| Error::Io(e.to_string()))?,
                Some(Duration::from_secs(30)),
            )
            .await?;
        let response: InitializeResponse = serde_json::from_value(value).map_err(|e| {
            Error::Protocol(format!("its answer to initialize could not be read: {e}"))
        })?;
        if response.protocol_version != PROTOCOL_VERSION {
            return Err(Error::Protocol(format!(
                "it speaks protocol version {}, and this app speaks {PROTOCOL_VERSION}",
                response.protocol_version
            )));
        }
        Ok(response)
    }

    /// Asks the agent to sign in by one of the methods it listed. The agent does the signing in; this only starts it and waits.
    pub async fn authenticate(&self, method_id: &str) -> Result<(), Error> {
        self.call(
            "authenticate",
            json!({"methodId": method_id}),
            Some(Duration::from_secs(600)),
        )
        .await
        .map(|_| ())
    }

    pub async fn new_session(&self, cwd: &str) -> Result<NewSessionResponse, Error> {
        let request = NewSessionRequest {
            cwd: cwd.to_string(),
            mcp_servers: Vec::new(),
        };
        let value = self
            .call(
                "session/new",
                serde_json::to_value(request).map_err(|e| Error::Io(e.to_string()))?,
                Some(Duration::from_secs(60)),
            )
            .await?;
        serde_json::from_value(value).map_err(|e| {
            Error::Protocol(format!("its answer to session/new could not be read: {e}"))
        })
    }

    /// Sends a prompt and waits for the turn to end. What the agent does meanwhile arrives as events. A turn has no time limit; stop it with
    /// [`Client::cancel`].
    pub async fn prompt(
        &self,
        session_id: &str,
        prompt: Vec<ContentBlock>,
    ) -> Result<StopReason, Error> {
        let request = PromptRequest {
            session_id: session_id.to_string(),
            prompt,
        };
        let value = self
            .call(
                "session/prompt",
                serde_json::to_value(request).map_err(|e| Error::Io(e.to_string()))?,
                None,
            )
            .await?;
        let response: PromptResponse = serde_json::from_value(value).map_err(|e| {
            Error::Protocol(format!(
                "its answer to session/prompt could not be read: {e}"
            ))
        })?;
        Ok(response.stop_reason)
    }

    /// Asks the agent to stop the turn. The prompt's answer says `cancelled` when it has.
    pub fn cancel(&self, session_id: &str) {
        self.notify("session/cancel", json!({"sessionId": session_id}));
    }

    /// Ends the agent's process.
    pub async fn close(&self) {
        let _ = self.kill.send(()).await;
    }
}

fn protocol_error(shared: &Shared, events: &mpsc::Sender<Event>, reason: &str) {
    shared.close(Error::Protocol(reason.to_string()));
    let _ = events.try_send(Event::ProtocolError(reason.to_string()));
}

async fn read_loop(
    mut out: tokio::process::ChildStdout,
    shared: Arc<Shared>,
    events: mpsc::Sender<Event>,
    writer: mpsc::Sender<String>,
    handler: Arc<dyn Handler>,
    slots: Arc<Semaphore>,
) {
    let mut line: Vec<u8> = Vec::new();
    let mut chunk = vec![0u8; 64 * 1024];
    loop {
        let read = match out.read(&mut chunk).await {
            Ok(0) | Err(_) => return,
            Ok(n) => n,
        };
        for &byte in &chunk[..read] {
            if byte != b'\n' {
                line.push(byte);
                if line.len() > MAX_LINE {
                    protocol_error(&shared, &events, "it wrote a line longer than the limit");
                    return;
                }
                continue;
            }
            let text = std::mem::take(&mut line);
            if text.iter().all(u8::is_ascii_whitespace) {
                continue;
            }
            let Ok(value) = serde_json::from_slice::<Value>(&text) else {
                protocol_error(
                    &shared,
                    &events,
                    "it wrote something that is not JSON on its output",
                );
                return;
            };
            if !handle_line(value, &shared, &events, &writer, &handler, &slots).await {
                return;
            }
        }
    }
}

/// Acts on one message from the agent. `false` ends the session.
async fn handle_line(
    value: Value,
    shared: &Arc<Shared>,
    events: &mpsc::Sender<Event>,
    writer: &mpsc::Sender<String>,
    handler: &Arc<dyn Handler>,
    slots: &Arc<Semaphore>,
) -> bool {
    if value.get("jsonrpc").and_then(Value::as_str) != Some("2.0") || !value.is_object() {
        protocol_error(
            shared,
            events,
            "it wrote a message that is not JSON-RPC 2.0",
        );
        return false;
    }
    let method = value
        .get("method")
        .and_then(Value::as_str)
        .map(str::to_string);
    let id = value.get("id").cloned();
    match (method, id) {
        // an answer to one of our requests
        (None, Some(id)) => {
            let Some(id) = id.as_u64() else { return true };
            let waiter = shared.pending.lock().expect("pending").remove(&id);
            if let Some(waiter) = waiter {
                let answer = match (value.get("result"), value.get("error")) {
                    (_, Some(error)) => Err(Error::Rpc(
                        serde_json::from_value(error.clone()).unwrap_or_else(|_| {
                            RpcError::new(RpcError::INTERNAL, "The agent reported an error.")
                        }),
                    )),
                    (Some(result), None) => Ok(result.clone()),
                    (None, None) => Err(Error::Protocol(
                        "an answer had neither a result nor an error".into(),
                    )),
                };
                let _ = waiter.send(answer);
            }
            true
        }
        // a notification
        (Some(method), None) => {
            if method == "session/update" {
                let params = value.get("params").cloned().unwrap_or(Value::Null);
                if let (Some(session), Some(update)) = (
                    params.get("sessionId").and_then(Value::as_str),
                    params.get("update"),
                ) {
                    let event = Event::Update {
                        session_id: session.to_string(),
                        update: SessionUpdate::from_value(update),
                    };
                    // when the queue is full this waits, which slows the agent's output instead of letting it grow without bound
                    if events.send(event).await.is_err() {
                        return false;
                    }
                }
            }
            true
        }
        // a request the host must answer
        (Some(method), Some(id)) => {
            let params = value.get("params").cloned().unwrap_or(Value::Null);
            let writer = writer.clone();
            let respond = move |outcome: Result<Value, RpcError>| {
                let reply = match outcome {
                    Ok(result) => json!({"jsonrpc": "2.0", "id": id, "result": result}),
                    Err(error) => json!({"jsonrpc": "2.0", "id": id, "error": error}),
                };
                async move {
                    let _ = writer.send(message(reply)).await;
                }
            };
            let Ok(permit) = slots.clone().try_acquire_owned() else {
                respond(Err(RpcError::refused("Too many requests at once."))).await;
                return true;
            };
            match AgentRequest::parse(&method, &params) {
                None => respond(Err(RpcError::method_not_found(&method))).await,
                Some(Err(error)) => respond(Err(error)).await,
                Some(Ok(request)) => {
                    let handler = handler.clone();
                    tokio::spawn(async move {
                        let outcome = handler.handle(request).await;
                        respond(outcome).await;
                        drop(permit);
                    });
                }
            }
            true
        }
        (None, None) => {
            protocol_error(
                shared,
                events,
                "it wrote a message with neither a method nor an id",
            );
            false
        }
    }
}
