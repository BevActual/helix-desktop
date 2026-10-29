//! Other coding agents in Mobius, through the Agent Client Protocol (`helix-acp`). See `docs/design/mobius-acp.md`.
//!
//! An agent runs as a subprocess in the chat's project folder. What it says streams into the thread like any reply, and what it does shows
//! as the same cards Tools mode uses. **Everything it asks of the computer comes through Mobius and waits for the person**: a file it
//! wants to write is a diff to apply or skip (with a restore point, so it can be undone), a command is shown and run in the project
//! sandbox only when approved, a file it wants to read must be inside the project and not a hidden or credential file, and its own
//! permission questions are put to the person as they are. Mobius never reads the agent's stored sign-in.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use helix_acp::{
    AgentRequest, BoxFuture, Client, ClientCapabilities, ClientInfo, ContentBlock, Event,
    FsCapabilities, Handler, PermissionKind, PermissionOption, PermissionOutcome, RpcError,
    SessionUpdate, Spawn, StopReason, ToolKind, ToolStatus,
};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use tauri::{AppHandle, Emitter, Manager};
use tokio::sync::{oneshot, watch};

use crate::{edits, terminal};

const READ_LIMIT_BYTES: u64 = 1024 * 1024;
const WRITE_LIMIT_CHARS: usize = 200_000;
const TERMINAL_KEEP: usize = 64 * 1024;
const REVIEW_SECONDS: u64 = 600;

// ---- which agents -------------------------------------------------------------------------------------------------------------------

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct AgentDef {
    pub id: String,
    pub title: String,
    pub command: String,
    #[serde(default)]
    pub args: Vec<String>,
    #[serde(default)]
    pub custom: bool,
}

#[derive(Default, Serialize, Deserialize)]
struct Config {
    #[serde(default)]
    custom: Vec<AgentDef>,
    /// Agents the person has said may run on this computer.
    #[serde(default)]
    trusted: Vec<String>,
}

fn config_path() -> PathBuf {
    crate::data_home().join("helix/mobius/acp-agents.json")
}

fn load_config() -> Config {
    std::fs::read_to_string(config_path())
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default()
}

fn save_config(config: &Config) -> Result<(), String> {
    let path = config_path();
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    std::fs::write(
        path,
        serde_json::to_string_pretty(config).map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())
}

fn builtin() -> Vec<AgentDef> {
    let def = |id: &str, title: &str, command: &str| AgentDef {
        id: id.into(),
        title: title.into(),
        command: command.into(),
        args: vec!["acp".into()],
        custom: false,
    };
    vec![
        def("opencode", "OpenCode", "opencode"),
        def("hermes", "Hermes", "hermes"),
    ]
}

/// The program on `PATH` (or at an absolute path), if there is one the person can run.
pub fn find_program(command: &str) -> Option<PathBuf> {
    use std::os::unix::fs::PermissionsExt;
    let runnable = |p: &Path| {
        p.is_file()
            && std::fs::metadata(p)
                .map(|m| m.permissions().mode() & 0o111 != 0)
                .unwrap_or(false)
    };
    if command.contains('/') {
        let p = PathBuf::from(command);
        return runnable(&p).then_some(p);
    }
    std::env::split_paths(&std::env::var_os("PATH")?)
        .map(|d| d.join(command))
        .find(|p| runnable(p))
}

#[derive(Serialize)]
pub struct AgentView {
    pub id: String,
    pub title: String,
    pub command_line: String,
    pub found: bool,
    pub trusted: bool,
    pub custom: bool,
}

pub fn agents() -> Vec<AgentDef> {
    let mut all = builtin();
    all.extend(load_config().custom);
    all
}

pub fn list() -> Vec<AgentView> {
    let config = load_config();
    let mut all = builtin();
    all.extend(config.custom.iter().cloned());
    all.into_iter()
        .map(|a| AgentView {
            found: find_program(&a.command).is_some(),
            trusted: config.trusted.contains(&a.id),
            command_line: std::iter::once(a.command.clone())
                .chain(a.args.iter().cloned())
                .collect::<Vec<_>>()
                .join(" "),
            custom: a.custom,
            id: a.id,
            title: a.title,
        })
        .collect()
}

fn slug(title: &str) -> String {
    let s: String = title
        .to_lowercase()
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
        .collect();
    s.trim_matches('-').chars().take(30).collect()
}

/// Adds an agent the person typed: a name and the command that starts it speaking ACP.
pub fn add_custom(title: &str, command: &str, args: &[String]) -> Result<AgentDef, String> {
    let title = title.trim();
    if title.is_empty() || title.chars().count() > 40 || title.chars().any(char::is_control) {
        return Err("Give the agent a short name.".into());
    }
    let command = command.trim();
    if command.is_empty()
        || command.len() > 200
        || command.chars().any(|c| c.is_whitespace() || c.is_control())
    {
        return Err(
            "The command is the program to run, with no spaces. Put its options in the arguments."
                .into(),
        );
    }
    if args.len() > 8
        || args
            .iter()
            .any(|a| a.len() > 200 || a.chars().any(char::is_control))
    {
        return Err("Use at most eight short arguments.".into());
    }
    let mut config = load_config();
    let base = format!("custom-{}", slug(title));
    let mut id = base.clone();
    let mut n = 2;
    while builtin()
        .iter()
        .chain(config.custom.iter())
        .any(|a| a.id == id)
    {
        id = format!("{base}-{n}");
        n += 1;
    }
    if config.custom.len() >= 12 {
        return Err("That is plenty of agents. Remove one first.".into());
    }
    let def = AgentDef {
        id,
        title: title.to_string(),
        command: command.to_string(),
        args: args.to_vec(),
        custom: true,
    };
    config.custom.push(def.clone());
    save_config(&config)?;
    Ok(def)
}

pub fn remove_custom(id: &str) -> Result<(), String> {
    let mut config = load_config();
    config.custom.retain(|a| a.id != id);
    config.trusted.retain(|t| t != id);
    save_config(&config)
}

pub fn set_trusted(id: &str, on: bool) -> Result<(), String> {
    if !agents().iter().any(|a| a.id == id) {
        return Err("No such agent.".into());
    }
    let mut config = load_config();
    config.trusted.retain(|t| t != id);
    if on {
        config.trusted.push(id.to_string());
    }
    save_config(&config)
}

// ---- pure helpers -------------------------------------------------------------------------------------------------------------------

/// One word for a shell: plain when it is made only of safe characters, otherwise single-quoted.
pub fn shell_quote(word: &str) -> String {
    if !word.is_empty()
        && word
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || "_@%+=:,./-".contains(c))
    {
        word.to_string()
    } else {
        format!("'{}'", word.replace('\'', "'\\''"))
    }
}

/// A command and its arguments as the one line shown to the person and run in the sandbox.
pub fn command_line(command: &str, args: &[String]) -> String {
    std::iter::once(command)
        .chain(args.iter().map(String::as_str))
        .map(shell_quote)
        .collect::<Vec<_>>()
        .join(" ")
}

/// A path the agent gave (absolute, as the protocol says) as a file inside the project: where it is and its path in the folder. Refuses
/// anything outside, hidden, a link, or a file that looks like it holds credentials.
pub fn project_file(
    root: &Path,
    given: &str,
    must_exist: bool,
) -> Result<(PathBuf, String), String> {
    let path = Path::new(given);
    if !path.is_absolute() {
        return Err("Give an absolute path inside the project folder.".into());
    }
    let root = root
        .canonicalize()
        .map_err(|_| "The project folder is not available.".to_string())?;
    let resolved = if must_exist {
        path.canonicalize()
            .map_err(|_| "That file does not exist.".to_string())?
    } else {
        let parent = path
            .parent()
            .ok_or("Give a file path.")?
            .canonicalize()
            .map_err(|_| "The file's folder does not exist.".to_string())?;
        parent.join(path.file_name().ok_or("Give a file path.")?)
    };
    let relative = resolved
        .strip_prefix(&root)
        .map_err(|_| "That path is outside the project folder.".to_string())?;
    let relative = relative
        .to_str()
        .ok_or("That path is not valid text.")?
        .to_string();
    let target = edits::resolve(&root, &relative)?;
    Ok((target, relative))
}

/// The lines asked for: from the 1-based `line`, at most `limit` of them.
pub fn slice_lines(text: &str, line: Option<u32>, limit: Option<u32>) -> String {
    if line.is_none() && limit.is_none() {
        return text.to_string();
    }
    let skip = line.unwrap_or(1).saturating_sub(1) as usize;
    let take = limit.map_or(usize::MAX, |l| l as usize);
    text.split_inclusive('\n').skip(skip).take(take).collect()
}

/// The option that answers the person's yes or no. Only a one-time choice is ever picked: Mobius does not grant a permanent allow for them.
pub fn choose_option(options: &[PermissionOption], approve: bool) -> PermissionOutcome {
    let wanted = if approve {
        PermissionKind::AllowOnce
    } else {
        PermissionKind::RejectOnce
    };
    options
        .iter()
        .find(|o| o.kind == wanted)
        .map_or(PermissionOutcome::Cancelled, |o| {
            PermissionOutcome::Selected(o.id.clone())
        })
}

fn state_word(status: ToolStatus) -> &'static str {
    match status {
        ToolStatus::Pending => "running",
        ToolStatus::InProgress => "running",
        ToolStatus::Completed => "completed",
        ToolStatus::Failed => "failed",
    }
}

fn kind_word(kind: ToolKind) -> &'static str {
    match kind {
        ToolKind::Read => "read",
        ToolKind::Edit => "edit",
        ToolKind::Delete => "delete",
        ToolKind::Move => "move",
        ToolKind::Search => "search",
        ToolKind::Execute => "execute",
        ToolKind::Think => "think",
        ToolKind::Fetch => "fetch",
        ToolKind::SwitchMode => "switch_mode",
        ToolKind::Other => "agent_step",
    }
}

/// What a tool call shows in its card: its text, the files a diff names, and a note for a terminal.
pub fn tool_output(content: &[Value]) -> String {
    let mut out = Vec::new();
    for item in content {
        match item.get("type").and_then(Value::as_str) {
            Some("content") => {
                if let Some(text) = item.pointer("/content/text").and_then(Value::as_str) {
                    out.push(text.to_string());
                }
            }
            Some("diff") => out.push(format!(
                "Changed {}",
                item.get("path").and_then(Value::as_str).unwrap_or("a file")
            )),
            Some("terminal") => out.push("(ran in a terminal)".to_string()),
            _ => {}
        }
    }
    out.join("\n")
}

pub fn plan_text(entries: &[helix_acp::PlanEntry]) -> String {
    entries
        .iter()
        .map(|e| {
            format!(
                "[{}] {}",
                if e.status == "completed" { "x" } else { " " },
                e.content
            )
        })
        .collect::<Vec<_>>()
        .join("\n")
}

/// A path as the person reads it: inside the project it is relative to the folder, so `notes.txt` and not `/home/me/work/project/notes.txt`.
pub fn short_path(folder: &Path, path: &str) -> String {
    Path::new(path)
        .strip_prefix(folder)
        .ok()
        .and_then(Path::to_str)
        .filter(|p| !p.is_empty())
        .unwrap_or(path)
        .to_string()
}

/// What a card says for a tool call: its title and the files it touches.
pub fn tool_input(folder: &Path, title: &str, locations: &[helix_acp::ToolLocation]) -> String {
    let mut text = title.to_string();
    for l in locations.iter().take(5) {
        text.push_str(&format!(
            "\n{}{}",
            short_path(folder, &l.path),
            l.line.map(|n| format!(":{n}")).unwrap_or_default()
        ));
    }
    text
}

/// The change an agent's whole-file write would make, for the person to review: the path, then only the lines that differ with three lines
/// of context either side (`+ ` added, `- ` removed, `  ` unchanged, and `  ⋯` where lines were left out). A new file lists its lines.
pub fn edit_diff(relative: &str, old: Option<&str>, new: &str) -> String {
    use similar::{ChangeTag, TextDiff};
    let mut out = format!(
        "{}{}\n",
        if old.is_none() { "new file " } else { "" },
        relative
    );
    let diff = TextDiff::from_lines(old.unwrap_or(""), new);
    for (n, group) in diff.grouped_ops(3).iter().enumerate() {
        if n > 0 {
            out.push_str("  ⋯\n");
        }
        for op in group {
            for change in diff.iter_changes(op) {
                let sign = match change.tag() {
                    ChangeTag::Insert => "+ ",
                    ChangeTag::Delete => "- ",
                    ChangeTag::Equal => "  ",
                };
                out.push_str(sign);
                out.push_str(change.value().trim_end_matches('\n'));
                out.push('\n');
            }
        }
    }
    out
}

/// A reply built from an agent's chunks. Its thoughts are wrapped in `<think>` so the thread folds them like any reasoning.
#[derive(Default)]
pub struct Stream {
    thinking: bool,
    pub text: String,
}

impl Stream {
    pub fn thought(&mut self, chunk: &str) -> String {
        let mut out = String::new();
        if !self.thinking {
            self.thinking = true;
            out.push_str("<think>");
        }
        out.push_str(chunk);
        self.text.push_str(&out);
        out
    }

    pub fn message(&mut self, chunk: &str) -> String {
        let mut out = self.close();
        out.push_str(chunk);
        self.text.push_str(chunk);
        out
    }

    /// Ends an open thought. What it returns is to be shown too.
    pub fn close(&mut self) -> String {
        if self.thinking {
            self.thinking = false;
            self.text.push_str("</think>\n\n");
            "</think>\n\n".to_string()
        } else {
            String::new()
        }
    }
}

// ---- running one -------------------------------------------------------------------------------------------------------------------

struct Terminal {
    output: Arc<Mutex<String>>,
    exit: Arc<Mutex<Option<Option<i32>>>>,
    kill: watch::Sender<bool>,
}

struct Turn {
    folder: PathBuf,
    generation: u64,
    operation: u64,
    stream: Stream,
    tools: HashMap<String, (String, String)>,
    cancel: watch::Receiver<bool>,
}

struct Live {
    agent: String,
    folder: PathBuf,
    chat: String,
    client: Arc<Client>,
    session: String,
    turn: Arc<Mutex<Option<Turn>>>,
    alive: Arc<std::sync::atomic::AtomicBool>,
    /// Updates the event pump has handled, to compare with how many the agent sent.
    handled: Arc<std::sync::atomic::AtomicU64>,
}

#[derive(Default)]
pub struct Runtime {
    live: tokio::sync::Mutex<Option<Live>>,
    selected: Mutex<Option<String>>,
    pending: Mutex<HashMap<String, oneshot::Sender<bool>>>,
    cancel: Mutex<Option<watch::Sender<bool>>>,
}

impl Runtime {
    pub fn selected(&self) -> Option<String> {
        self.selected.lock().expect("selected").clone()
    }

    pub fn select(&self, id: Option<String>) {
        *self.selected.lock().expect("selected") = id;
    }

    /// The person's answer to a card that is waiting.
    pub fn review(&self, id: &str, approved: bool) -> Result<(), String> {
        let waiter = self
            .pending
            .lock()
            .expect("pending")
            .remove(id)
            .ok_or("That is no longer waiting for review.")?;
        waiter
            .send(approved)
            .map_err(|_| "The agent stopped waiting.".to_string())
    }

    /// Whether a card is one of the agent's (and so answered here, not by Tools mode).
    pub fn is_waiting(&self, id: &str) -> bool {
        self.pending.lock().expect("pending").contains_key(id)
    }

    pub fn cancel(&self) {
        if let Some(cancel) = self.cancel.lock().expect("cancel").as_ref() {
            let _ = cancel.send(true);
        }
        self.pending.lock().expect("pending").clear();
    }
}

/// Answers what the agent asks of the computer. Each request that changes or runs something waits for the person.
struct Bridge {
    app: AppHandle,
    folder: PathBuf,
    turn: Arc<Mutex<Option<Turn>>>,
    terminals: Arc<Mutex<HashMap<String, Terminal>>>,
    next: std::sync::atomic::AtomicU64,
}

impl Bridge {
    fn generation(&self) -> Option<(u64, watch::Receiver<bool>)> {
        self.turn
            .lock()
            .expect("turn")
            .as_ref()
            .map(|t| (t.generation, t.cancel.clone()))
    }

    fn chat_id(&self) -> String {
        crate::shared(&self.app)
            .lock()
            .expect("state")
            .chat
            .conversation
            .id
            .clone()
    }

    fn card(&self, id: &str, tool: &str, input: &str, state: &str, output: &str) {
        if let Some((generation, _)) = self.generation() {
            crate::agent::activity(&self.app, generation, id, tool, input, state, output);
        }
    }

    /// Shows a card that needs a yes or no and waits for it. `Ok(false)` is "no", an error is the turn being stopped.
    async fn ask(&self, id: &str, tool: &str, input: &str, note: &str) -> Result<bool, RpcError> {
        let (_, mut cancel) = self
            .generation()
            .ok_or_else(|| RpcError::refused("There is no turn to ask about."))?;
        let (sender, receiver) = oneshot::channel();
        self.app
            .state::<Runtime>()
            .pending
            .lock()
            .expect("pending")
            .insert(id.to_string(), sender);
        self.card(id, tool, input, "waiting", note);
        tokio::select! {
            biased;
            _ = cancel.changed() => Err(RpcError::refused("The turn was stopped.")),
            answer = tokio::time::timeout(Duration::from_secs(REVIEW_SECONDS), receiver) => Ok(answer.ok().and_then(Result::ok).unwrap_or(false)),
        }
    }

    fn unique(&self, prefix: &str) -> String {
        format!(
            "{prefix}-{}",
            self.next.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        )
    }

    async fn read(&self, request: helix_acp::ReadTextFile) -> Result<Value, RpcError> {
        let (target, _) =
            project_file(&self.folder, &request.path, true).map_err(RpcError::refused)?;
        let meta = std::fs::metadata(&target)
            .map_err(|_| RpcError::new(RpcError::RESOURCE_NOT_FOUND, "No such file."))?;
        if !meta.is_file() || meta.len() > READ_LIMIT_BYTES {
            return Err(RpcError::refused(
                "That is not a text file small enough to read.",
            ));
        }
        let text = std::fs::read_to_string(&target)
            .map_err(|_| RpcError::refused("That file is not readable text."))?;
        Ok(json!({"content": slice_lines(&text, request.line, request.limit)}))
    }

    async fn write(&self, request: helix_acp::WriteTextFile) -> Result<Value, RpcError> {
        if request.content.chars().count() > WRITE_LIMIT_CHARS {
            return Err(RpcError::refused(
                "That file is larger than Mobius reviews (200,000 characters).",
            ));
        }
        let (target, relative) =
            project_file(&self.folder, &request.path, false).map_err(RpcError::refused)?;
        let before = match std::fs::metadata(&target) {
            Ok(m) if m.is_file() && m.len() <= READ_LIMIT_BYTES => Some(
                std::fs::read_to_string(&target)
                    .map_err(|_| RpcError::refused("That file is not readable text."))?,
            ),
            Ok(_) => {
                return Err(RpcError::refused(
                    "That is not a text file small enough to change.",
                ))
            }
            Err(_) => None,
        };
        if before.as_deref() == Some(request.content.as_str()) {
            return Ok(Value::Null);
        }
        let diff = edit_diff(&relative, before.as_deref(), &request.content);
        let id = self.unique("acp-edit");
        let shown: String = diff.chars().take(12_000).collect();
        let allowed = self
            .ask(
                &id,
                "edit_file",
                &shown,
                &format!("The agent wants to write {relative}."),
            )
            .await?;
        if !allowed {
            self.card(
                &id,
                "edit_file",
                &shown,
                "declined",
                "The person skipped this change. The file was not touched.",
            );
            return Err(RpcError::refused("The person declined this change."));
        }
        let checkpoint = edits::Checkpoint {
            path: target,
            before,
            after: request.content.clone(),
            after_present: true,
        };
        edits::apply(&checkpoint, &crate::edit_store(&self.chat_id(), &id))
            .map_err(RpcError::refused)?;
        self.card(
            &id,
            "edit_file",
            &shown,
            "completed",
            &format!("Wrote {relative}."),
        );
        Ok(Value::Null)
    }

    async fn permission(&self, request: helix_acp::RequestPermission) -> Result<Value, RpcError> {
        let id = format!(
            "acp-perm-{}",
            request
                .tool_call
                .id
                .chars()
                .filter(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_')
                .collect::<String>()
        );
        let title = request
            .tool_call
            .title
            .clone()
            .unwrap_or_else(|| "A step".to_string());
        let kind = request
            .tool_call
            .kind
            .map(kind_word)
            .unwrap_or("agent_step");
        let locations = request.tool_call.locations.clone().unwrap_or_default();
        let input = format!("{}\n({kind})", tool_input(&self.folder, &title, &locations));
        let one_time = request
            .options
            .iter()
            .any(|o| o.kind == PermissionKind::AllowOnce);
        let note = if one_time {
            "The agent asks to do this. Allow it once, or skip."
        } else {
            "The agent offers only a permanent allow, which Mobius does not grant. Skip it, or run the agent yourself."
        };
        let approved = self.ask(&id, "agent_permission", &input, note).await?;
        let outcome = choose_option(&request.options, approved && one_time);
        self.card(
            &id,
            "agent_permission",
            &input,
            if approved && one_time {
                "completed"
            } else {
                "declined"
            },
            if approved && one_time {
                "Allowed once."
            } else {
                "Not allowed."
            },
        );
        Ok(outcome.to_value())
    }

    async fn create_terminal(&self, request: helix_acp::CreateTerminal) -> Result<Value, RpcError> {
        let script = command_line(&request.command, &request.args);
        terminal::safe_input(&script, 2000).map_err(RpcError::refused)?;
        if let Some(cwd) = &request.cwd {
            // commands run in the project folder; a folder inside it is the same folder to the sandbox, so only that is accepted
            let inside = Path::new(cwd)
                .canonicalize()
                .ok()
                .and_then(|c| self.folder.canonicalize().ok().map(|f| c.starts_with(f)))
                .unwrap_or(false);
            if !inside {
                return Err(RpcError::refused("Commands run in the project folder."));
            }
        }
        let id = self.unique("acp-term");
        if !self
            .ask(
                &id,
                "terminal",
                &script,
                "The agent wants to run this command in the project sandbox (no network).",
            )
            .await?
        {
            self.card(
                &id,
                "terminal",
                &script,
                "declined",
                "The person skipped this command. It was not run.",
            );
            return Err(RpcError::refused(
                "The person declined to run this command.",
            ));
        }
        let output = Arc::new(Mutex::new(String::new()));
        let exit = Arc::new(Mutex::new(None));
        let (kill, kill_receiver) = watch::channel(false);
        self.terminals.lock().expect("terminals").insert(
            id.clone(),
            Terminal {
                output: output.clone(),
                exit: exit.clone(),
                kill,
            },
        );
        let (app, folder, card_id, line) = (
            self.app.clone(),
            self.folder.clone(),
            id.clone(),
            script.clone(),
        );
        let generation = self.generation().map(|(g, _)| g);
        tokio::spawn(async move {
            let progress = |text: String| {
                *output.lock().expect("output") = text
                    .chars()
                    .rev()
                    .take(TERMINAL_KEEP)
                    .collect::<Vec<_>>()
                    .into_iter()
                    .rev()
                    .collect();
                if let Some(g) = generation {
                    crate::agent::activity(&app, g, &card_id, "terminal", &line, "running", &text);
                }
            };
            let result = terminal::run(
                &folder,
                &helix_task::Policy::careful(),
                &line,
                kill_receiver,
                progress,
            )
            .await;
            let (code, state, text) = match result {
                Ok(o) => (
                    o.code,
                    if o.code == Some(0) && !o.stopped && !o.limited {
                        "completed"
                    } else {
                        "failed"
                    },
                    format!("Exit: {:?}\n{}", o.code, o.text),
                ),
                Err(e) => (None, "failed", e),
            };
            *output.lock().expect("output") = text.chars().take(TERMINAL_KEEP).collect();
            *exit.lock().expect("exit") = Some(code);
            if let Some(g) = generation {
                crate::agent::activity(&app, g, &card_id, "terminal", &line, state, &text);
            }
        });
        Ok(json!({"terminalId": id}))
    }

    fn with_terminal<T>(&self, id: &str, f: impl FnOnce(&Terminal) -> T) -> Result<T, RpcError> {
        let terminals = self.terminals.lock().expect("terminals");
        terminals
            .get(id)
            .map(f)
            .ok_or_else(|| RpcError::new(RpcError::RESOURCE_NOT_FOUND, "No such terminal."))
    }
}

impl Handler for Bridge {
    fn handle(&self, request: AgentRequest) -> BoxFuture<'_, Result<Value, RpcError>> {
        Box::pin(async move {
            match request {
                AgentRequest::ReadTextFile(r) => self.read(r).await,
                AgentRequest::WriteTextFile(w) => self.write(w).await,
                AgentRequest::RequestPermission(p) => self.permission(p).await,
                AgentRequest::CreateTerminal(t) => self.create_terminal(t).await,
                AgentRequest::TerminalOutput(t) => self.with_terminal(&t.terminal_id, |term| {
                    let output = term.output.lock().expect("output").clone();
                    let exit = *term.exit.lock().expect("exit");
                    json!({"output": output, "truncated": false, "exitStatus": exit.map(|code| json!({"exitCode": code, "signal": Value::Null}))})
                }),
                AgentRequest::WaitForTerminalExit(t) => {
                    for _ in 0..(REVIEW_SECONDS * 20) {
                        if let Some(code) = self.with_terminal(&t.terminal_id, |term| *term.exit.lock().expect("exit"))? {
                            return Ok(json!({"exitCode": code, "signal": Value::Null}));
                        }
                        tokio::time::sleep(Duration::from_millis(50)).await;
                    }
                    Err(RpcError::refused("The command is still running."))
                }
                AgentRequest::KillTerminal(t) => self.with_terminal(&t.terminal_id, |term| {
                    let _ = term.kill.send(true);
                    json!({})
                }),
                AgentRequest::ReleaseTerminal(t) => {
                    if let Some(term) = self.terminals.lock().expect("terminals").remove(&t.terminal_id) {
                        let _ = term.kill.send(true);
                    }
                    Ok(json!({}))
                }
            }
        })
    }
}

// ---- events into the thread --------------------------------------------------------------------------------------------------------

fn push_text(app: &AppHandle, generation: u64, operation: u64, text: &str) {
    if text.is_empty() {
        return;
    }
    let changed = crate::shared(app)
        .lock()
        .expect("state")
        .chat
        .delta(operation, text);
    if changed {
        crate::emit_view(app);
    }
    let _ = generation;
}

fn handle_update(app: &AppHandle, turn: &Arc<Mutex<Option<Turn>>>, update: SessionUpdate) {
    let mut guard = turn.lock().expect("turn");
    let Some(t) = guard.as_mut() else { return };
    let (generation, operation) = (t.generation, t.operation);
    let folder = t.folder.clone();
    match update {
        SessionUpdate::AgentMessage(ContentBlock::Text { text }) => {
            let shown = t.stream.message(&text);
            drop(guard);
            push_text(app, generation, operation, &shown);
        }
        SessionUpdate::AgentThought(ContentBlock::Text { text }) => {
            let shown = t.stream.thought(&text);
            drop(guard);
            push_text(app, generation, operation, &shown);
        }
        SessionUpdate::ToolCall(call) => {
            let input = tool_input(&folder, &call.title, &call.locations);
            t.tools.insert(
                call.id.clone(),
                (kind_word(call.kind).to_string(), input.clone()),
            );
            drop(guard);
            crate::agent::activity(
                app,
                generation,
                &format!("acp-call-{}", call.id),
                kind_word(call.kind),
                &input,
                state_word(call.status),
                &tool_output(&call.content),
            );
        }
        SessionUpdate::ToolCallUpdate(u) => {
            let known = t.tools.get(&u.id).cloned();
            let (kind, input) = known.unwrap_or_else(|| {
                (
                    u.kind.map(kind_word).unwrap_or("agent_step").to_string(),
                    u.title.clone().unwrap_or_default(),
                )
            });
            let input = match (&u.title, &u.locations) {
                (Some(title), locations) => {
                    tool_input(&folder, title, locations.as_deref().unwrap_or(&[]))
                }
                _ => input,
            };
            t.tools.insert(u.id.clone(), (kind.clone(), input.clone()));
            drop(guard);
            let output = u.content.as_deref().map(tool_output).unwrap_or_default();
            crate::agent::activity(
                app,
                generation,
                &format!("acp-call-{}", u.id),
                &kind,
                &input,
                u.status.map(state_word).unwrap_or("running"),
                &output,
            );
        }
        SessionUpdate::Plan(entries) => {
            drop(guard);
            let done = !entries.is_empty() && entries.iter().all(|e| e.status == "completed");
            crate::agent::activity(
                app,
                generation,
                "acp-plan",
                "planning",
                "",
                if done { "completed" } else { "running" },
                &plan_text(&entries),
            );
        }
        _ => {}
    }
}

fn fail_turn(app: &AppHandle, turn: &Arc<Mutex<Option<Turn>>>, message: &str) {
    let taken = turn.lock().expect("turn").take();
    if let Some(t) = taken {
        crate::agent::ended_early(app, t.generation, message, false);
    }
}

async fn pump(
    app: AppHandle,
    mut events: tokio::sync::mpsc::Receiver<Event>,
    turn: Arc<Mutex<Option<Turn>>>,
    alive: Arc<std::sync::atomic::AtomicBool>,
    handled: Arc<std::sync::atomic::AtomicU64>,
) {
    while let Some(event) = events.recv().await {
        match event {
            Event::Update { update, .. } => {
                handle_update(&app, &turn, update);
                handled.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            }
            Event::Stderr(line) => eprintln!("MOBIUS agent: {line}"),
            Event::ProtocolError(message) => {
                alive.store(false, std::sync::atomic::Ordering::Relaxed);
                fail_turn(
                    &app,
                    &turn,
                    &format!("The agent broke the protocol: {message}."),
                );
            }
            Event::Exited(status) => {
                alive.store(false, std::sync::atomic::Ordering::Relaxed);
                fail_turn(&app, &turn, &format!("The agent stopped: {status}."));
            }
        }
    }
}

// ---- a turn --------------------------------------------------------------------------------------------------------------------------

fn info() -> ClientInfo {
    ClientInfo {
        name: "helix-mobius".into(),
        title: "Helix Mobius".into(),
        version: env!("CARGO_PKG_VERSION").into(),
    }
}

fn capabilities() -> ClientCapabilities {
    ClientCapabilities {
        fs: FsCapabilities {
            read_text_file: true,
            write_text_file: true,
        },
        terminal: true,
    }
}

type LiveParts = (
    Arc<Client>,
    String,
    Arc<Mutex<Option<Turn>>>,
    Arc<std::sync::atomic::AtomicU64>,
);

async fn ensure_live(
    app: &AppHandle,
    def: &AgentDef,
    folder: &Path,
    chat: &str,
) -> Result<LiveParts, String> {
    let runtime = app.state::<Runtime>();
    let mut live = runtime.live.lock().await;
    if let Some(l) = live.as_ref() {
        if l.agent == def.id
            && l.folder == folder
            && l.chat == chat
            && l.alive.load(std::sync::atomic::Ordering::Relaxed)
        {
            return Ok((
                l.client.clone(),
                l.session.clone(),
                l.turn.clone(),
                l.handled.clone(),
            ));
        }
    }
    if let Some(old) = live.take() {
        old.client.close().await;
    }
    let program = find_program(&def.command).ok_or_else(|| {
        format!(
            "{} is not installed: `{}` was not found.",
            def.title, def.command
        )
    })?;
    let turn: Arc<Mutex<Option<Turn>>> = Arc::new(Mutex::new(None));
    let bridge = Arc::new(Bridge {
        app: app.clone(),
        folder: folder.to_path_buf(),
        turn: turn.clone(),
        terminals: Arc::new(Mutex::new(HashMap::new())),
        next: std::sync::atomic::AtomicU64::new(1),
    });
    let spawn = Spawn {
        command: program.to_string_lossy().to_string(),
        args: def.args.clone(),
        cwd: Some(folder.to_path_buf()),
        env: Vec::new(),
        clear_env: false,
    };
    let (client, events) = Client::spawn(spawn, bridge).map_err(|e| e.to_string())?;
    let alive = Arc::new(std::sync::atomic::AtomicBool::new(true));
    let handled = Arc::new(std::sync::atomic::AtomicU64::new(0));
    tokio::spawn(pump(
        app.clone(),
        events,
        turn.clone(),
        alive.clone(),
        handled.clone(),
    ));
    client
        .initialize(info(), capabilities())
        .await
        .map_err(|e| e.to_string())?;
    let session = client
        .new_session(&folder.to_string_lossy())
        .await
        .map_err(|e| {
            format!("{e}. If it needs you to sign in, do that in a terminal first, then try again.")
        })?;
    let client = Arc::new(client);
    *live = Some(Live {
        agent: def.id.clone(),
        folder: folder.to_path_buf(),
        chat: chat.to_string(),
        client: client.clone(),
        session: session.session_id.clone(),
        turn: turn.clone(),
        alive,
        handled: handled.clone(),
    });
    Ok((client, session.session_id, turn, handled))
}

/// One message to the selected agent. What comes back streams into the thread; the turn ends with the agent's answer or when it is stopped.
pub async fn run(app: AppHandle, turn: helix_chat::chat::Turn) {
    let generation = turn.generation;
    let (cancel_sender, mut cancel) = watch::channel(false);
    *app.state::<Runtime>().cancel.lock().expect("cancel") = Some(cancel_sender);
    let result = drive(&app, &turn, &mut cancel).await;
    if let Err(error) = result {
        let stopped = *cancel.borrow();
        crate::agent::ended_early(&app, generation, &error, stopped);
    }
}

async fn drive(
    app: &AppHandle,
    turn: &helix_chat::chat::Turn,
    cancel: &mut watch::Receiver<bool>,
) -> Result<(), String> {
    let runtime = app.state::<Runtime>();
    let id = runtime.selected().ok_or("No agent is selected.")?;
    let def = agents()
        .into_iter()
        .find(|a| a.id == id)
        .ok_or("That agent is no longer in the list.")?;
    if !load_config().trusted.contains(&def.id) {
        return Err(format!(
            "You have not said {} may run on this computer. Turn that on in Settings → Agents.",
            def.title
        ));
    }
    let (folder, chat) = {
        let shared = crate::shared(app);
        let state = shared.lock().expect("state");
        (
            state.chat.conversation.workspace.clone(),
            state.chat.conversation.id.clone(),
        )
    };
    if folder.is_empty() {
        return Err("Choose a project folder first: the agent works in it.".into());
    }
    let folder = terminal::workspace(&folder)?;
    let prompt = turn
        .messages
        .iter()
        .rev()
        .find(|(role, _)| *role == "user")
        .map(|(_, text)| text.clone())
        .unwrap_or_default();
    let operation = turn.generation.saturating_mul(1_000_003).saturating_add(41);
    {
        let shared = crate::shared(app);
        let mut state = shared.lock().expect("state");
        let _ = state.chat.started(turn.generation, operation);
        state.turn_model = def.title.clone();
    }
    crate::emit_view(app);

    let (client, session, shared_turn, handled) = ensure_live(app, &def, &folder, &chat).await?;
    *shared_turn.lock().expect("turn") = Some(Turn {
        folder: folder.clone(),
        generation: turn.generation,
        operation,
        stream: Stream::default(),
        tools: HashMap::new(),
        cancel: cancel.clone(),
    });

    let sending = client.prompt(&session, vec![ContentBlock::text(prompt)]);
    tokio::pin!(sending);
    let stop = tokio::select! {
        result = &mut sending => result.map_err(|e| e.to_string()),
        _ = cancel.changed() => {
            client.cancel(&session);
            // the agent says `cancelled` when it has stopped; if it does not, it is ended
            match tokio::time::timeout(Duration::from_secs(10), &mut sending).await {
                Ok(result) => result.map_err(|e| e.to_string()),
                Err(_) => {
                    client.close().await;
                    Err("Stopped.".to_string())
                }
            }
        }
    };
    // the answer can arrive before the pump has shown the last words: wait until it has handled every update the agent sent
    for _ in 0..400 {
        if handled.load(std::sync::atomic::Ordering::SeqCst) >= client.updates_received() {
            break;
        }
        tokio::time::sleep(Duration::from_millis(5)).await;
    }
    let finished = shared_turn.lock().expect("turn").take();
    let Some(mut finished) = finished else {
        // the pump already ended the turn (the agent died or broke the protocol)
        return Ok(());
    };
    let stop = stop?;
    let closing = finished.stream.close();
    push_text(app, turn.generation, operation, &closing);
    let note = match stop {
        StopReason::EndTurn => "",
        StopReason::Cancelled => "\n\n(Stopped.)",
        StopReason::MaxTokens => "\n\n(The agent reached its length limit.)",
        StopReason::MaxTurnRequests => {
            "\n\n(The agent reached its limit of steps for one message.)"
        }
        StopReason::Refusal => "\n\n(The agent declined to answer.)",
    };
    if finished.stream.text.trim().is_empty() && note.is_empty() {
        push_text(
            app,
            turn.generation,
            operation,
            "(The agent finished without a message.)",
        );
    } else {
        push_text(app, turn.generation, operation, note);
    }
    let outcome = {
        let shared = crate::shared(app);
        let mut state = shared.lock().expect("state");
        state.chat.complete(
            operation,
            true,
            &json!({"ok": true, "backend": "agent", "task": "agent"}).to_string(),
        )
    };
    if let Some(outcome) = outcome {
        crate::ended(app, outcome);
    }
    Ok(())
}

/// Starts the agent and says what it is, without giving it anything to do: the Settings page's Test.
pub async fn test(def: &AgentDef) -> Result<Value, String> {
    let program = find_program(&def.command)
        .ok_or_else(|| format!("`{}` was not found on this computer.", def.command))?;
    let spawn = Spawn {
        command: program.to_string_lossy().to_string(),
        args: def.args.clone(),
        cwd: None,
        env: Vec::new(),
        clear_env: false,
    };
    let (client, _events) =
        Client::spawn(spawn, Arc::new(helix_acp::RefuseAll)).map_err(|e| e.to_string())?;
    let result = client.initialize(info(), capabilities()).await;
    client.close().await;
    let hello = result.map_err(|e| e.to_string())?;
    Ok(json!({
        "name": hello.agent_info.as_ref().map(|i| i.title.clone().unwrap_or_else(|| i.name.clone())).unwrap_or_default(),
        "version": hello.agent_info.as_ref().map(|i| i.version.clone()).unwrap_or_default(),
        "sign_in": hello.auth_methods.iter().map(|m| m.name.clone()).collect::<Vec<_>>(),
        "capabilities": hello.agent_capabilities,
    }))
}

pub fn announce(app: &AppHandle) {
    let _ = app.emit("acp-changed", app.state::<Runtime>().selected());
}

#[cfg(test)]
mod tests {
    use super::*;
    use helix_acp::{PlanEntry, ToolLocation};

    fn scratch(tag: &str) -> PathBuf {
        static COUNT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let n = COUNT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let dir = std::env::temp_dir().join(format!("helix-acp-{tag}-{}-{n}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir.canonicalize().unwrap()
    }

    #[test]
    fn a_command_is_quoted_for_the_shell_so_an_argument_cannot_become_another_command() {
        assert_eq!(
            command_line("ls", &["-l".into(), "src/main.rs".into()]),
            "ls -l src/main.rs"
        );
        assert_eq!(
            command_line(
                "echo",
                &[
                    "a b".into(),
                    "it's".into(),
                    "$(rm -rf ~)".into(),
                    "x;y".into(),
                    String::new()
                ]
            ),
            "echo 'a b' 'it'\\''s' '$(rm -rf ~)' 'x;y' ''"
        );
        assert_eq!(shell_quote("plain-1.2/x"), "plain-1.2/x");
    }

    #[test]
    fn only_files_inside_the_project_that_are_not_hidden_or_secret_are_reachable() {
        let root = scratch("root");
        std::fs::write(root.join("a.txt"), "hi").unwrap();
        std::fs::write(root.join(".env"), "SECRET=1").unwrap();
        std::fs::create_dir_all(root.join(".git")).unwrap();
        std::fs::write(root.join(".git/config"), "x").unwrap();
        std::fs::write(root.join("id_rsa"), "key").unwrap();
        let outside = scratch("outside");
        std::fs::write(outside.join("o.txt"), "o").unwrap();
        std::os::unix::fs::symlink(outside.join("o.txt"), root.join("link.txt")).unwrap();
        let ok = project_file(&root, root.join("a.txt").to_str().unwrap(), true).unwrap();
        assert_eq!(ok.1, "a.txt");
        for (path, why) in [
            (root.join(".env"), "hidden"),
            (root.join(".git/config"), "hidden folder"),
            (root.join("id_rsa"), "credentials"),
            (outside.join("o.txt"), "outside"),
            (root.join("link.txt"), "a link to outside"),
            (root.join("../x.txt"), "parent"),
        ] {
            assert!(
                project_file(&root, path.to_str().unwrap(), true).is_err(),
                "{why}"
            );
        }
        assert!(
            project_file(&root, "relative.txt", true).is_err(),
            "the protocol says absolute"
        );
        assert!(project_file(&root, "/etc/passwd", true).is_err());
        // a file that does not exist yet can be named when its folder is inside the project
        assert_eq!(
            project_file(&root, root.join("new.txt").to_str().unwrap(), false)
                .unwrap()
                .1,
            "new.txt"
        );
        assert!(project_file(
            &root,
            root.join("missing/dir/new.txt").to_str().unwrap(),
            false
        )
        .is_err());
    }

    #[test]
    fn a_whole_file_write_is_reviewed_as_only_what_changed() {
        let old = (1..=20).map(|n| format!("line {n}\n")).collect::<String>();
        let new = old.replace("line 10\n", "line ten\n") + "line 21\n";
        let diff = edit_diff("a.txt", Some(&old), &new);
        let lines: Vec<&str> = diff.lines().collect();
        assert_eq!(lines[0], "a.txt");
        assert!(
            lines.contains(&"- line 10")
                && lines.contains(&"+ line ten")
                && lines.contains(&"+ line 21")
        );
        assert!(
            !lines.iter().any(|l| *l == "  line 1" || *l == "  line 2"),
            "far-away lines are left out: {diff}"
        );
        assert!(lines.contains(&"  ⋯"), "{diff}");
        assert!(
            lines.contains(&"  line 9") && lines.contains(&"  line 11"),
            "context stays"
        );
        let fresh = edit_diff("new.txt", None, "one\ntwo\n");
        assert_eq!(fresh, "new file new.txt\n+ one\n+ two\n");
        assert_eq!(edit_diff("same.txt", Some("x\n"), "x\n"), "same.txt\n");
    }

    #[test]
    fn lines_are_cut_as_asked() {
        let text = "a\nb\nc\nd\n";
        assert_eq!(slice_lines(text, None, None), text);
        assert_eq!(slice_lines(text, Some(2), Some(2)), "b\nc\n");
        assert_eq!(slice_lines(text, Some(4), None), "d\n");
        assert_eq!(slice_lines(text, None, Some(1)), "a\n");
    }

    #[test]
    fn only_a_one_time_answer_is_ever_given_for_the_person() {
        let option = |id: &str, kind| PermissionOption {
            id: id.into(),
            name: id.into(),
            kind,
        };
        let both = [
            option("a", PermissionKind::AllowOnce),
            option("b", PermissionKind::RejectOnce),
            option("c", PermissionKind::AllowAlways),
        ];
        assert_eq!(
            choose_option(&both, true),
            PermissionOutcome::Selected("a".into())
        );
        assert_eq!(
            choose_option(&both, false),
            PermissionOutcome::Selected("b".into())
        );
        let permanent = [
            option("c", PermissionKind::AllowAlways),
            option("d", PermissionKind::RejectAlways),
        ];
        assert_eq!(
            choose_option(&permanent, true),
            PermissionOutcome::Cancelled,
            "no one-time allow: never grant the permanent one"
        );
        assert_eq!(
            choose_option(&permanent, false),
            PermissionOutcome::Cancelled
        );
    }

    #[test]
    fn thoughts_fold_and_messages_follow_them() {
        let mut s = Stream::default();
        assert_eq!(s.thought("Let me look. "), "<think>Let me look. ");
        assert_eq!(s.thought("Then edit."), "Then edit.");
        assert_eq!(s.message("Done."), "</think>\n\nDone.");
        assert_eq!(s.message(" Really."), " Really.");
        assert_eq!(s.close(), "");
        assert_eq!(
            s.text,
            "<think>Let me look. Then edit.</think>\n\nDone. Really."
        );
        let mut t = Stream::default();
        t.thought("only thinking");
        assert_eq!(t.close(), "</think>\n\n");
    }

    #[test]
    fn tool_calls_show_their_files_and_output_in_plain_text() {
        let locations = vec![
            ToolLocation {
                path: "/p/a.rs".into(),
                line: Some(3),
            },
            ToolLocation {
                path: "/p/b.rs".into(),
                line: None,
            },
        ];
        assert_eq!(
            tool_input(Path::new("/p"), "Edit a", &locations),
            "Edit a\na.rs:3\nb.rs"
        );
        assert_eq!(
            tool_input(Path::new("/q"), "Edit a", &locations),
            "Edit a\n/p/a.rs:3\n/p/b.rs",
            "outside the project the full path stays"
        );
        let content = vec![
            json!({"type": "content", "content": {"type": "text", "text": "ok"}}),
            json!({"type": "diff", "path": "/p/a.rs", "oldText": "x", "newText": "y"}),
            json!({"type": "terminal", "terminalId": "t1"}),
        ];
        assert_eq!(
            tool_output(&content),
            "ok\nChanged /p/a.rs\n(ran in a terminal)"
        );
        let plan = vec![
            PlanEntry {
                content: "one".into(),
                priority: "high".into(),
                status: "completed".into(),
            },
            PlanEntry {
                content: "two".into(),
                priority: "low".into(),
                status: "pending".into(),
            },
        ];
        assert_eq!(plan_text(&plan), "[x] one\n[ ] two");
        assert_eq!(
            (
                state_word(ToolStatus::Pending),
                state_word(ToolStatus::Failed),
                kind_word(ToolKind::Execute),
                kind_word(ToolKind::Other)
            ),
            ("running", "failed", "execute", "agent_step")
        );
    }

    #[test]
    fn a_custom_agent_must_be_a_name_and_a_command_with_no_spaces() {
        let dir = scratch("cfg");
        std::env::set_var("XDG_DATA_HOME", &dir);
        assert!(add_custom("", "x", &[]).is_err());
        assert!(
            add_custom("My agent", "run it", &[]).is_err(),
            "options go in the arguments"
        );
        assert!(add_custom("My agent", "x", &vec!["a".to_string(); 9]).is_err());
        let a = add_custom("My agent", "my-agent", &["--acp".into()]).unwrap();
        assert_eq!(a.id, "custom-my-agent");
        assert_eq!(
            add_custom("My agent", "other", &[]).unwrap().id,
            "custom-my-agent-2"
        );
        assert!(list().iter().any(|v| v.id == "custom-my-agent"
            && !v.trusted
            && v.custom
            && v.command_line == "my-agent --acp"));
        set_trusted("custom-my-agent", true).unwrap();
        assert!(list()
            .iter()
            .any(|v| v.id == "custom-my-agent" && v.trusted));
        assert!(set_trusted("nope", true).is_err());
        remove_custom("custom-my-agent").unwrap();
        assert!(!list().iter().any(|v| v.id == "custom-my-agent"));
        std::env::remove_var("XDG_DATA_HOME");
    }
}
