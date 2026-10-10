//! Mobius — Helix AI hub. Talks via `org.helix.Inference` (local) or OpenAI-compat LAN (this process).
//! Desktop act hands off to the bar Assistant (`Shell1.AssistantSubmit`). Never holds Agent Surface grants.

mod lan;

use std::path::PathBuf;
use std::sync::{Arc, Mutex};

use futures_util::StreamExt;
use helix_shell_core::apps::assistant::MAX_REQUEST_CHARS;
use helix_shell_core::chat::{valid_id, Chat, ChatError, ChatView, Conversation, Outcome, Summary, SYSTEM_PROMPT};
use helix_shell_core::inference_routing::{self, BackendKind, Routing};
use helix_shell_core::markdown::Block;
use helix_shell_core::personas::{self, Persona, PersonaStore};
use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager, WebviewUrl, WebviewWindowBuilder};
use zbus::zvariant::{Array, Signature, Value};

const WINDOW: &str = "chat";
const BUS_NAME: &str = "org.helix.Inference";
const OBJECT_PATH: &str = "/org/helix/Inference";
const INTERFACE: &str = "org.helix.Inference1";
const SHELL_BUS: &str = "org.helix.Shell";
const SHELL_PATH: &str = "/org/helix/Shell";
const SHELL_IFACE: &str = "org.helix.Shell1";
const MAX_CONVERSATIONS: usize = 200;

struct State {
    chat: Chat,
    /// Last backend label shown after a turn (local / LAN name).
    last_backend: String,
}

type Shared = Arc<Mutex<State>>;

fn shared(app: &AppHandle) -> Shared {
    app.state::<Shared>().inner().clone()
}

// Files -------------------------------------------------------------------------------------------------------------------------------

fn data_home() -> PathBuf {
    std::env::var_os("XDG_DATA_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share")))
        .unwrap_or_else(|| PathBuf::from("."))
}

/// Mobius transcripts. Legacy `helix/chats` is still read; new saves go here.
fn data_dir() -> PathBuf {
    data_home().join("helix/mobius/chats")
}

fn legacy_data_dir() -> PathBuf {
    data_home().join("helix/chats")
}

fn now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_secs())
}

fn new_id() -> String {
    use std::io::Read;
    let mut bytes = [0u8; 8];
    if std::fs::File::open("/dev/urandom")
        .and_then(|mut f| f.read_exact(&mut bytes))
        .is_err()
    {
        bytes = now().to_be_bytes();
    }
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn save(conversation: &Conversation) {
    use std::io::Write;
    use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
    if conversation.messages.is_empty() {
        return;
    }
    let dir = data_dir();
    if std::fs::DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(&dir)
        .is_err()
    {
        return;
    }
    let path = dir.join(format!("{}.json", conversation.id));
    let temporary = path.with_extension("tmp");
    let written = std::fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(0o600)
        .open(&temporary)
        .and_then(|mut f| f.write_all(conversation.to_json().as_bytes()));
    if written.is_ok() {
        let _ = std::fs::rename(&temporary, &path);
    }
}

fn load_from(dir: &PathBuf, id: &str) -> Option<Conversation> {
    if !valid_id(id) {
        return None;
    }
    let text = std::fs::read_to_string(dir.join(format!("{id}.json"))).ok()?;
    Conversation::from_json(&text).filter(|c| c.id == id)
}

fn load(id: &str) -> Option<Conversation> {
    load_from(&data_dir(), id).or_else(|| load_from(&legacy_data_dir(), id))
}

fn summaries() -> Vec<Summary> {
    let mut seen = std::collections::HashSet::new();
    let mut all: Vec<Summary> = Vec::new();
    for dir in [data_dir(), legacy_data_dir()] {
        for entry in std::fs::read_dir(dir).into_iter().flatten().flatten() {
            let name = match entry.file_name().into_string() {
                Ok(n) => n,
                Err(_) => continue,
            };
            let Some(id) = name.strip_suffix(".json") else { continue };
            if !seen.insert(id.to_string()) {
                continue;
            }
            if let Some(c) = load(id) {
                if !c.messages.is_empty() {
                    all.push(Summary {
                        id: c.id,
                        title: c.title,
                        updated: c.updated,
                    });
                }
            }
        }
    }
    all.sort_by(|a, b| b.updated.cmp(&a.updated).then(a.id.cmp(&b.id)));
    all.truncate(MAX_CONVERSATIONS);
    all
}

// The model service -------------------------------------------------------------------------------------------------------------------

async fn proxy(connection: &zbus::Connection) -> zbus::Result<zbus::Proxy<'static>> {
    zbus::Proxy::new(connection, BUS_NAME, OBJECT_PATH, INTERFACE).await
}

async fn shell_proxy(connection: &zbus::Connection) -> zbus::Result<zbus::Proxy<'static>> {
    zbus::Proxy::new(connection, SHELL_BUS, SHELL_PATH, SHELL_IFACE).await
}

fn shell_offline() -> String {
    "Helix Shell is not running. Open the session bar and try again.".into()
}

fn chat_args(
    messages: &[(&'static str, String)],
    task: &str,
    model: &str,
) -> Result<std::collections::HashMap<&'static str, Value<'static>>, zbus::Error> {
    let variant = Signature::try_from("v").map_err(|e| zbus::Error::Failure(e.to_string()))?;
    let (string, any) = (
        Signature::try_from("s").map_err(|e| zbus::Error::Failure(e.to_string()))?,
        variant.clone(),
    );
    let mut list = Array::new(&variant);
    for (role, content) in messages {
        let mut entry = zbus::zvariant::Dict::new(&string, &any);
        entry
            .append(
                Value::from("role".to_string()),
                Value::Value(Box::new(Value::from((*role).to_string()))),
            )
            .map_err(zbus::Error::from)?;
        entry
            .append(
                Value::from("content".to_string()),
                Value::Value(Box::new(Value::from(content.clone()))),
            )
            .map_err(zbus::Error::from)?;
        list.append(Value::Value(Box::new(Value::Dict(entry))))
            .map_err(zbus::Error::from)?;
    }
    let mut args = std::collections::HashMap::new();
    args.insert("messages", Value::Array(list));
    args.insert("task", Value::from(task.to_string()));
    if !model.is_empty() {
        args.insert("model", Value::from(model.to_string()));
    }
    args.insert("temperature", Value::F64(0.6));
    args.insert("max_tokens", Value::I64(1024));
    args.insert("priority", Value::from("interactive".to_string()));
    Ok(args)
}

static CONNECTION: tokio::sync::OnceCell<zbus::Connection> = tokio::sync::OnceCell::const_new();

async fn connection() -> zbus::Result<zbus::Connection> {
    CONNECTION
        .get_or_try_init(zbus::Connection::session)
        .await
        .cloned()
}

#[derive(Serialize, Clone)]
struct Streamed {
    blocks: Vec<Block>,
}

#[derive(Serialize, Clone)]
struct HubStatus {
    inference: &'static str,
    backend: String,
    left_device: bool,
}

fn emit_view(app: &AppHandle) {
    let view = shared(app).lock().expect("state").chat.view();
    let _ = app.emit("chat-view", view);
}

fn emit_list(app: &AppHandle) {
    let _ = app.emit("chat-list", summaries());
}

fn emit_personas(app: &AppHandle) {
    let _ = app.emit("personas-view", personas::load());
}

fn emit_routing(app: &AppHandle) {
    let _ = app.emit("routing-view", inference_routing::load());
}

/// What a turn's ending means for the page and the disk.
fn ended(app: &AppHandle, outcome: Outcome) {
    eprintln!("MOBIUS turn ended {outcome:?}");
    {
        let shared = shared(app);
        let state = shared.lock().expect("state");
        save(&state.chat.conversation);
    }
    emit_view(app);
    emit_list(app);
}

/// Follows the model service's streaming signals for as long as the app runs (they are sent to the requester only).
fn follow_signals(app: AppHandle) {
    tauri::async_runtime::spawn(async move {
        loop {
            let Ok(connection) = connection().await else {
                tokio::time::sleep(std::time::Duration::from_secs(2)).await;
                continue;
            };
            let Ok(proxy) = proxy(&connection).await else { continue };
            let (Ok(mut deltas), Ok(mut done)) = (
                proxy.receive_signal("Delta").await,
                proxy.receive_signal("Completed").await,
            ) else {
                tokio::time::sleep(std::time::Duration::from_secs(2)).await;
                continue;
            };
            loop {
                tokio::select! {
                    Some(message) = deltas.next() => {
                        if let Ok((operation, text)) = message.body().deserialize::<(u64, String)>() {
                            let blocks = {
                                let shared = shared(&app);
                                let mut state = shared.lock().expect("state");
                                state.chat.delta(operation, &text).then(|| state.chat.last_blocks())
                            };
                            if let Some(blocks) = blocks {
                                let _ = app.emit("chat-stream", Streamed { blocks });
                            }
                        }
                    }
                    Some(message) = done.next() => {
                        if let Ok((operation, ok, result)) = message.body().deserialize::<(u64, bool, String)>() {
                            let outcome = shared(&app).lock().expect("state").chat.complete(operation, ok, &result);
                            if let Some(outcome) = outcome {
                                ended(&app, outcome);
                            }
                        }
                    }
                    else => break,
                }
            }
        }
    });
}

fn power_saver_active() -> bool {
    if let Ok(text) = std::fs::read_to_string("/sys/firmware/acpi/platform_profile") {
        let t = text.trim();
        if t == "low-power" || t == "cool" {
            return true;
        }
    }
    if let Ok(output) = std::process::Command::new("powerprofilesctl").arg("get").output() {
        if let Ok(text) = String::from_utf8(output.stdout) {
            return text.trim() == "power-saver";
        }
    }
    false
}

fn resolve_route(routing: &Routing, persona: &Persona) -> (String, String, bool, String) {
    let task = if persona.task.is_empty() {
        "chat".to_string()
    } else {
        persona.task.clone()
    };
    let profile = routing.route(&task);
    let backend_id = if !persona.backend.is_empty() {
        persona.backend.clone()
    } else {
        profile.backend.clone()
    };
    let model = if !persona.model.is_empty() {
        persona.model.clone()
    } else {
        profile.model.clone()
    };
    let backend = routing.backend(&backend_id);
    let is_local = match backend {
        Some(b) => b.kind == BackendKind::Local,
        None => backend_id.is_empty() || backend_id == "local",
    };
    let label = backend
        .map(|b| {
            if b.label.is_empty() {
                b.id.clone()
            } else {
                b.label.clone()
            }
        })
        .unwrap_or_else(|| {
            if is_local {
                "On this computer".into()
            } else {
                backend_id.clone()
            }
        });
    (task, model, is_local, label)
}

// Commands ----------------------------------------------------------------------------------------------------------------------------

#[tauri::command]
fn chat_view(app: AppHandle) -> ChatView {
    shared(&app).lock().expect("state").chat.view()
}

#[tauri::command]
fn chat_list() -> Vec<Summary> {
    summaries()
}

#[tauri::command]
async fn chat_status(app: AppHandle) -> HubStatus {
    let backend = shared(&app).lock().expect("state").last_backend.clone();
    let left_device = !backend.is_empty() && backend != "On this computer" && !backend.starts_with("local");
    let Ok(connection) = connection().await else {
        return HubStatus {
            inference: "offline",
            backend,
            left_device,
        };
    };
    let Ok(proxy) = proxy(&connection).await else {
        return HubStatus {
            inference: "offline",
            backend,
            left_device,
        };
    };
    let inference = match proxy.call_method("GetSnapshot", &()).await {
        Ok(_) => "ready",
        Err(_) => "offline",
    };
    HubStatus {
        inference,
        backend,
        left_device,
    }
}

fn error_text(error: &ChatError) -> &'static str {
    match error {
        ChatError::Empty => "Write something to send.",
        ChatError::TooLong => "That message is too long.",
        ChatError::Busy => "Wait for the reply to finish, or stop it.",
        ChatError::Full => "This chat is full. Start a new one.",
    }
}

#[tauri::command]
async fn chat_send(app: AppHandle, text: String) -> Result<(), String> {
    let store = personas::load();
    let persona = store.active_persona().clone();
    let prompt = if persona.system_prompt.trim().is_empty() {
        SYSTEM_PROMPT
    } else {
        persona.system_prompt.as_str()
    };
    let routing = inference_routing::load();
    let (task, model, is_local, label) = resolve_route(&routing, &persona);

    let turn = shared(&app)
        .lock()
        .expect("state")
        .chat
        .send_with_system(&text, now(), prompt)
        .map_err(|e| error_text(&e).to_string())?;
    {
        let shared = shared(&app);
        let mut state = shared.lock().expect("state");
        state.last_backend = label.clone();
    }
    emit_view(&app);

    if !is_local {
        if !routing.allow_remote_on_battery_saver && power_saver_active() {
            let shared = shared(&app);
            shared.lock().expect("state").chat.failed_to_start(
                turn.generation,
                "Battery saver is on. Enable “Allow remote on battery saver” in Models & routing, or use a local route.",
            );
            emit_view(&app);
            return Ok(());
        }
        let profile_backend = routing.route(&task).backend;
        let backend_key = if persona.backend.is_empty() {
            profile_backend.as_str()
        } else {
            persona.backend.as_str()
        };
        let backend = routing.backend(backend_key).cloned();
        let Some(backend) = backend.filter(|b| b.kind == BackendKind::Openai) else {
            let shared = shared(&app);
            shared.lock().expect("state").chat.failed_to_start(
                turn.generation,
                "This persona’s route is not a local or OpenAI-compatible backend.",
            );
            emit_view(&app);
            return Ok(());
        };
        let generation = turn.generation;
        let messages = turn.messages.clone();
        let model = model.clone();
        let base = backend.base_url.clone();
        let result = lan::chat_completions(&base, &model, &messages).await;
        let fake_op = generation.saturating_mul(1_000_003).saturating_add(7);
        {
            let shared = shared(&app);
            let mut state = shared.lock().expect("state");
            let _ = state.chat.started(generation, fake_op);
        }
        match result {
            Ok(text) => {
                let outcome = {
                    let shared = shared(&app);
                    let mut state = shared.lock().expect("state");
                    let _ = state.chat.delta(fake_op, &text);
                    state.chat.complete(
                        fake_op,
                        true,
                        &serde_json::json!({"ok": true, "text": text, "backend": label, "task": task})
                            .to_string(),
                    )
                };
                if let Some(outcome) = outcome {
                    ended(&app, outcome);
                }
            }
            Err(reason) => {
                let outcome = {
                    let shared = shared(&app);
                    let mut state = shared.lock().expect("state");
                    state.chat.complete(
                        fake_op,
                        false,
                        &serde_json::json!({"ok": false, "error": reason}).to_string(),
                    )
                };
                if let Some(outcome) = outcome {
                    ended(&app, outcome);
                } else {
                    emit_view(&app);
                }
            }
        }
        return Ok(());
    }

    let outcome = async {
        let connection = connection().await.map_err(|e| e.to_string())?;
        let proxy = proxy(&connection).await.map_err(|e| e.to_string())?;
        let args = chat_args(&turn.messages, &task, &model).map_err(|e| e.to_string())?;
        let operation: u64 = proxy
            .call("Action", &("chat", args))
            .await
            .map_err(|e| e.to_string())?;
        let (outcome, stop) = {
            let shared = shared(&app);
            let mut state = shared.lock().expect("state");
            let outcome = state.chat.started(turn.generation, operation);
            (outcome, state.chat.stop_requested())
        };
        if stop && outcome.is_none() {
            let _ = proxy.call_method("Cancel", &(operation,)).await;
        }
        Ok::<_, String>(outcome)
    }
    .await;
    match outcome {
        Ok(Some(outcome)) => ended(&app, outcome),
        Ok(None) => {}
        Err(reason) => {
            eprintln!("MOBIUS could not start: {reason}");
            shared(&app).lock().expect("state").chat.failed_to_start(
                turn.generation,
                "The assistant is not running. It starts when you use it; try again in a moment.",
            );
            emit_view(&app);
        }
    }
    Ok(())
}

/// Hands `text` to the bar Assistant (Agent Surface path). Does not call the model.
#[tauri::command]
async fn chat_act_on_desktop(text: String) -> Result<(), String> {
    let trimmed = text.trim_matches(|c: char| c.is_ascii_whitespace());
    if trimmed.is_empty() {
        return Err("Write something to act on.".into());
    }
    if trimmed.len() > MAX_REQUEST_CHARS {
        return Err(format!(
            "Keep it under {MAX_REQUEST_CHARS} characters for desktop actions."
        ));
    }
    let connection = connection().await.map_err(|_| shell_offline())?;
    let proxy = shell_proxy(&connection).await.map_err(|_| shell_offline())?;
    proxy
        .call_method("AssistantSubmit", &(trimmed,))
        .await
        .map_err(|error| {
            let detail = error.to_string();
            if detail.contains("Name has no owner") || detail.contains("ServiceUnknown") {
                shell_offline()
            } else {
                format!("Could not hand off to the desktop assistant: {detail}")
            }
        })?;
    Ok(())
}

/// Opens a shell panel (`assistant` or `agents`) so Mobius can jump to oversight or the act surface.
#[tauri::command]
async fn chat_open_shell_panel(id: String) -> Result<(), String> {
    if id != "assistant" && id != "agents" {
        return Err("unknown panel".into());
    }
    let connection = connection().await.map_err(|_| shell_offline())?;
    let proxy = shell_proxy(&connection).await.map_err(|_| shell_offline())?;
    proxy
        .call_method("OpenPanel", &(id.as_str(),))
        .await
        .map_err(|_| shell_offline())?;
    Ok(())
}

#[tauri::command]
async fn chat_stop(app: AppHandle) {
    let operation = shared(&app).lock().expect("state").chat.stop();
    if let Some(operation) = operation {
        if let Ok(connection) = connection().await {
            if let Ok(proxy) = proxy(&connection).await {
                let _ = proxy.call_method("Cancel", &(operation,)).await;
            }
        }
    }
}

fn switch_to(app: &AppHandle, next: Conversation) -> Result<(), String> {
    {
        let shared = shared(app);
        let mut state = shared.lock().expect("state");
        if state.chat.is_streaming() {
            return Err("Wait for the reply to finish, or stop it.".into());
        }
        save(&state.chat.conversation);
        state.chat = Chat::new(next);
    }
    emit_view(app);
    emit_list(app);
    Ok(())
}

#[tauri::command]
fn chat_new(app: AppHandle) -> Result<(), String> {
    let empty = shared(&app)
        .lock()
        .expect("state")
        .chat
        .conversation
        .messages
        .is_empty();
    if empty {
        return Ok(());
    }
    switch_to(&app, Conversation::new(&new_id(), now()).ok_or("no id")?)
}

#[tauri::command]
fn chat_open(app: AppHandle, id: String) -> Result<(), String> {
    let conversation = load(&id).ok_or("That chat could not be opened.")?;
    switch_to(&app, conversation)
}

#[tauri::command]
fn chat_delete(app: AppHandle, id: String) -> Result<(), String> {
    if !valid_id(&id) {
        return Err("not a chat".into());
    }
    let current = shared(&app)
        .lock()
        .expect("state")
        .chat
        .conversation
        .id
        .clone();
    if current == id {
        let shared = shared(&app);
        let mut state = shared.lock().expect("state");
        if state.chat.is_streaming() {
            return Err("Stop the reply first.".into());
        }
        state.chat = Chat::new(Conversation::new(&new_id(), now()).ok_or("no id")?);
    }
    let _ = std::fs::remove_file(data_dir().join(format!("{id}.json")));
    let _ = std::fs::remove_file(legacy_data_dir().join(format!("{id}.json")));
    emit_view(&app);
    emit_list(&app);
    Ok(())
}

#[tauri::command]
fn chat_copy(text: String) -> Result<(), String> {
    use std::io::Write;
    let text: String = text.chars().take(100_000).collect();
    let mut child = std::process::Command::new("wl-copy")
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn()
        .map_err(|_| "Copying is not available here.".to_string())?;
    if let Some(mut stdin) = child.stdin.take() {
        let _ = stdin.write_all(text.as_bytes());
    }
    Ok(())
}

#[tauri::command]
fn layout_reporting() -> bool {
    std::env::var_os("HELIX_UI_LAYOUT_LOG").is_some()
}

#[tauri::command]
fn report_layout(report: serde_json::Value) {
    let Some(path) = std::env::var_os("HELIX_UI_LAYOUT_LOG") else {
        return;
    };
    let path = PathBuf::from(path);
    let text = report.to_string();
    if text.len() > 64 * 1024 {
        return;
    }
    let temporary = path.with_extension("tmp");
    if std::fs::write(&temporary, &text).is_ok() {
        let _ = std::fs::rename(&temporary, &path);
    }
}

// Personas & routing ------------------------------------------------------------------------------------------------------------------

#[tauri::command]
fn personas_get() -> PersonaStore {
    personas::load()
}

#[tauri::command]
fn personas_set_active(app: AppHandle, id: String) -> Result<(), String> {
    let mut store = personas::load();
    if !store.set_active(&id) {
        return Err("Unknown persona.".into());
    }
    personas::save(&store)?;
    emit_personas(&app);
    Ok(())
}

#[tauri::command]
fn personas_upsert(app: AppHandle, persona: Persona) -> Result<(), String> {
    let mut store = personas::load();
    store.upsert(persona)?;
    personas::save(&store)?;
    emit_personas(&app);
    Ok(())
}

#[tauri::command]
fn routing_get() -> Routing {
    inference_routing::load()
}

/// GGUF ids from `org.helix.Inference` (empty when the service is offline).
#[tauri::command]
async fn routing_list_models() -> Vec<String> {
    let Ok(connection) = connection().await else {
        return Vec::new();
    };
    let Ok(proxy) = proxy(&connection).await else {
        return Vec::new();
    };
    let Ok(reply) = proxy.call_method("ListModels", &()).await else {
        return Vec::new();
    };
    let Ok(rows) = reply
        .body()
        .deserialize::<Vec<std::collections::HashMap<String, zbus::zvariant::OwnedValue>>>()
    else {
        return Vec::new();
    };
    let mut ids = Vec::new();
    for row in rows {
        let Some(id) = row.get("id") else {
            continue;
        };
        let Ok(name) = id.downcast_ref::<&str>() else {
            continue;
        };
        ids.push((*name).to_string());
    }
    ids.sort_unstable();
    ids.dedup();
    ids
}

#[tauri::command]
fn routing_save(app: AppHandle, routing: Routing) -> Result<(), String> {
    // Act profile must stay local — never allow remote tool JSON.
    if let Some(act) = routing.profiles.get("act") {
        if !act.backend.is_empty() && act.backend != "local" {
            if let Some(b) = routing.backend(&act.backend) {
                if b.kind != BackendKind::Local {
                    return Err("The act profile must use a local backend.".into());
                }
            } else {
                return Err("The act profile must use a local backend.".into());
            }
        }
    }
    inference_routing::save(&routing)?;
    emit_routing(&app);
    Ok(())
}

#[tauri::command]
async fn open_settings_agents() -> Result<(), String> {
    // Best-effort: Settings Privacy & agents (grants). Prefer desktop launch.
    let _ = std::process::Command::new("helix-settings")
        .arg("--page")
        .arg("agents")
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn();
    Ok(())
}

fn main() {
    gtk::glib::set_prgname(Some("org.helix.Mobius"));
    if std::env::var_os("JSC_useJIT").is_none() {
        std::env::set_var("JSC_useJIT", "false");
    }
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            chat_view,
            chat_list,
            chat_status,
            chat_send,
            chat_act_on_desktop,
            chat_open_shell_panel,
            chat_stop,
            chat_new,
            chat_open,
            chat_delete,
            chat_copy,
            layout_reporting,
            report_layout,
            personas_get,
            personas_set_active,
            personas_upsert,
            routing_get,
            routing_list_models,
            routing_save,
            open_settings_agents,
        ])
        .setup(|app| {
            let fresh = Conversation::new(&new_id(), now()).expect("a fresh id is valid");
            app.manage::<Shared>(Arc::new(Mutex::new(State {
                chat: Chat::new(fresh),
                last_backend: "On this computer".into(),
            })));
            WebviewWindowBuilder::new(app, WINDOW, WebviewUrl::App("index.html".into()))
                .title("Mobius")
                .decorations(false)
                .transparent(true)
                .inner_size(980.0, 700.0)
                .min_inner_size(640.0, 460.0)
                .build()?;
            follow_signals(app.handle().clone());
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running Mobius");
}
