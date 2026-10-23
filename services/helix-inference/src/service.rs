//! The D-Bus face of the queue (`org.helix.Inference1`). The worker thread never touches D-Bus: everything it produces is
//! handed to a dispatcher task, where the operation-to-caller table lives. Replies go only to whoever asked.
//! Port of C++ `inference_service.*`.

use std::collections::{BTreeSet, HashMap};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde_json::{json, Map, Value as Json};
use tokio::sync::mpsc;
use zbus::message::Header;
use zbus::names::BusName;
use zbus::zvariant::{OwnedValue, Value};
use zbus::{interface, Connection};

use crate::backend::*;
use crate::pictures;
use crate::queue::{InferenceQueue, Limits};
use crate::routing::RoutingTable;

pub const BUS_NAME: &str = "org.helix.Inference";
pub const OBJECT_PATH: &str = "/org/helix/Inference";
pub const INTERFACE: &str = "org.helix.Inference1";

#[derive(Debug, zbus::DBusError)]
#[zbus(prefix = "org.helix.Inference.Error")]
pub enum InferenceError {
    #[zbus(error)]
    ZBus(zbus::Error),
    InvalidArgument(String),
    Failed(String),
}

fn invalid(message: impl Into<String>) -> InferenceError {
    InferenceError::InvalidArgument(message.into())
}

#[derive(Clone)]
pub struct Config {
    pub model_dir: String,
    pub idle_unload: Duration,
    pub limits: Limits,
}

struct Owner {
    sender: String,
    client: String,
}

enum Event {
    Delta(u64, String),
    Done(u64, Outcome),
    Loading(u64, String, f64),
    Progress(u64, f64, String),
}

type Owners = Arc<Mutex<HashMap<u64, Owner>>>;

pub struct Inner {
    config: Config,
    queue: Arc<InferenceQueue>,
    owners: Owners,
    clients: Arc<Mutex<HashMap<String, String>>>,
    connection: Connection,
}

pub fn outcome_json(outcome: &Outcome) -> Json {
    let mut result = json!({
        "ok": outcome.ok,
        "text": outcome.text,
        "error": outcome.error,
        "cancelled": outcome.cancelled,
        "truncated": outcome.truncated,
        "prompt_tokens": outcome.prompt_tokens,
        "cached_prompt_tokens": outcome.cached_prompt_tokens,
        "completion_tokens": outcome.completion_tokens,
        "language": outcome.language,
    });
    if !outcome.model.is_empty() {
        result["model"] = json!(outcome.model);
    }
    if let Some(cost) = outcome.cost_micro {
        result["cost_micro"] = json!(cost);
    }
    if !outcome.image_png.is_empty() {
        result["image"] = json!({
            "png": outcome.image_png,
            "model": outcome.image_model,
            "prompt": outcome.image_prompt,
            "width": outcome.image_width,
            "height": outcome.image_height,
            "seed": outcome.image_seed,
        });
    }
    result
}

fn from_variant(value: &Value<'_>) -> Json {
    match value {
        Value::Bool(v) => json!(*v),
        Value::U8(v) => json!(*v),
        Value::U16(v) => json!(*v),
        Value::U32(v) => json!(*v),
        Value::U64(v) => json!(*v),
        Value::I16(v) => json!(*v),
        Value::I32(v) => json!(*v),
        Value::I64(v) => json!(*v),
        Value::F64(v) => json!(*v),
        Value::Str(v) => json!(v.as_str()),
        Value::ObjectPath(v) => json!(v.as_str()),
        Value::Array(items) => Json::Array(items.iter().map(from_variant).collect()),
        Value::Dict(entries) => {
            let mut object = Map::new();
            for (key, nested) in entries.iter() {
                if let Value::Str(key) = key {
                    object.insert(key.to_string(), from_variant(nested));
                }
            }
            Json::Object(object)
        }
        Value::Value(inner) => from_variant(inner),
        _ => Json::Null,
    }
}

fn to_variant(value: &Json) -> Option<OwnedValue> {
    let wrapped = match value {
        Json::Null => return None,
        Json::Bool(v) => Value::from(*v),
        Json::Number(n) => match n.as_u64() {
            Some(u) => Value::from(u),
            None => match n.as_i64() {
                Some(i) => Value::from(i),
                None => Value::from(n.as_f64()?),
            },
        },
        Json::String(s) => Value::from(s.as_str()),
        Json::Array(items) => Value::from(items.iter().filter_map(to_variant).collect::<Vec<_>>()),
        Json::Object(map) => Value::from(
            map.iter()
                .filter_map(|(k, v)| to_variant(v).map(|v| (k.clone(), v)))
                .collect::<HashMap<String, OwnedValue>>(),
        ),
    };
    OwnedValue::try_from(wrapped).ok()
}

/// Whether the computer is saving power (the firmware's platform profile, else the power daemon's).
fn power_saver_active() -> bool {
    if let Ok(text) = std::fs::read_to_string("/sys/firmware/acpi/platform_profile") {
        let profile = text.trim();
        if profile == "low-power" || profile == "cool" {
            return true;
        }
    }
    if let Ok(output) = std::process::Command::new("powerprofilesctl")
        .arg("get")
        .output()
    {
        return String::from_utf8_lossy(&output.stdout).trim() == "power-saver";
    }
    false
}

fn parse_chat(
    args: &Map<String, Json>,
    client: &str,
    routing: &RoutingTable,
    images: Vec<ImageInput>,
) -> Result<ChatRequest, InferenceError> {
    const KNOWN: [&str; 10] = [
        "backend",
        "route",
        "remote_timeout",
        "messages",
        "model",
        "schema",
        "max_tokens",
        "temperature",
        "priority",
        "task",
    ];
    for key in args.keys() {
        if !KNOWN.contains(&key.as_str()) {
            return Err(invalid(format!("unknown argument: {key}")));
        }
    }
    let mut request = ChatRequest {
        client: client.to_string(),
        ..ChatRequest::default()
    };
    let messages = args
        .get("messages")
        .ok_or_else(|| invalid("messages is required"))?;
    request.messages = messages.clone(); // the queue validates the shape
    request.images = images;
    // Every image part must name a picture that was sent, and every picture must be used.
    let mut referenced = BTreeSet::new();
    for message in messages.as_array().into_iter().flatten() {
        let Some(parts) = message.get("content").and_then(Json::as_array) else {
            continue;
        };
        for part in parts {
            let Some(part) = part.as_object() else {
                return Err(invalid("message parts must be objects"));
            };
            let kind = part.get("type").and_then(Json::as_str).unwrap_or("");
            if kind == "text" {
                continue;
            }
            if kind != "image" {
                return Err(invalid(format!("unknown message part: {kind}")));
            }
            let Some(index) = part.get("index").and_then(Json::as_u64) else {
                return Err(invalid("an image part needs an index"));
            };
            if index as usize >= request.images.len() {
                return Err(invalid(
                    "an image part refers to a picture that was not sent",
                ));
            }
            referenced.insert(index);
        }
    }
    if referenced.len() != request.images.len() {
        return Err(invalid("every picture sent must be used in a message"));
    }
    let mut task = "chat".to_string();
    if let Some(value) = args.get("task") {
        let Some(text) = value.as_str() else {
            return Err(invalid("task must be a string"));
        };
        task = text.to_string();
        if !["act", "chat", "summarize", "vision", "code"].contains(&task.as_str()) {
            return Err(invalid("task must be act, chat, summarize, vision or code"));
        }
    }
    if let Some(value) = args.get("model") {
        let Some(text) = value.as_str() else {
            return Err(invalid("model must be a string"));
        };
        request.model = text.to_string();
    }
    // Local model slots: when the client omits model, use the profile from inference-routing.toml.
    // A caller may name the backend itself (Mobius does, for a persona's own model): it must be one the person configured, and never for Act.
    let explicit = match args.get("backend") {
        None => None,
        Some(value) => {
            let Some(id) = value.as_str() else {
                return Err(invalid("backend must be a string"));
            };
            (!id.is_empty() && id != "local").then(|| id.to_string())
        }
    };
    if let Some(id) = &explicit {
        if task == "act" {
            return Err(invalid("the act task only runs on a local model"));
        }
        let backend = routing
            .backend(id)
            .ok_or_else(|| invalid(format!("no such backend: {id}")))?;
        if backend.kind != "openai" {
            return Err(invalid(format!(
                "backend {} is of a kind this service does not speak",
                backend.id
            )));
        }
        request.backend = backend.id.clone();
        if request.model.is_empty() {
            request.model = "default".into();
        }
    }
    if explicit.is_none() && request.model.is_empty() {
        let route = routing.route(&task);
        if routing.is_local(&task) {
            request.model = route.model;
        } else {
            // The person routed this slot to another machine or a hosted service: that is the explicit choice. The Act slot is the exception: only a
            // model on this computer may emit tools for the desktop.
            if task == "act" {
                return Err(invalid("the act task only runs on a local model"));
            }
            let Some(backend) = routing.backend(&route.backend) else {
                return Err(invalid(format!(
                    "task \"{task}\" is routed to a backend that is not configured: {}",
                    route.backend
                )));
            };
            if backend.kind != "openai" {
                return Err(invalid(format!(
                    "backend {} is of a kind this service does not speak",
                    backend.id
                )));
            }
            request.backend = backend.id.clone();
            request.model = route.model;
        }
    }
    if let Some(value) = args.get("schema") {
        let Some(text) = value.as_str() else {
            return Err(invalid("schema must be a string of JSON"));
        };
        // Only the on-device Act slot may emit tool JSON (grammar). Refuse schemas on talk tasks.
        if task != "act" {
            return Err(invalid("schema is only allowed for task \"act\""));
        }
        if !request.backend.is_empty() {
            return Err(invalid("schema requires a local act backend"));
        }
        request.json_schema = text.to_string();
    }
    if let Some(value) = args.get("max_tokens") {
        let Some(number) = value
            .as_i64()
            .or_else(|| value.as_u64().map(|u| u.min(i64::MAX as u64) as i64))
        else {
            return Err(invalid("max_tokens must be an integer"));
        };
        request.max_tokens = if number < 1 {
            1
        } else {
            number.min(1_000_000) as u32
        };
    }
    if let Some(value) = args.get("temperature") {
        let Some(number) = value.as_f64() else {
            return Err(invalid("temperature must be a number"));
        };
        request.temperature = number;
    }
    if !request.backend.is_empty() {
        // Off the computer: the person can keep that from happening while the battery is being saved.
        if !routing.allow_remote_on_battery_saver && power_saver_active() {
            return Err(invalid(
                "Battery saver is on, so models on other computers and services are not used. Allow them in Settings, or use a model on this computer.",
            ));
        }
        if let Some(value) = args.get("remote_timeout") {
            let Some(seconds) = value.as_u64().or_else(|| value.as_f64().map(|f| f as u64)) else {
                return Err(invalid("remote_timeout must be a number of seconds"));
            };
            request.timeout_secs = seconds.clamp(15, 600);
        }
        if let Some(value) = args.get("route") {
            let Some(map) = value.as_object() else {
                return Err(invalid("route must be a dictionary"));
            };
            let mut route = RemoteRoute::default();
            for (key, v) in map {
                match key.as_str() {
                    "fallbacks" => {
                        let list = v
                            .as_array()
                            .ok_or_else(|| invalid("fallbacks is a list of model ids"))?;
                        if list.len() > 8 {
                            return Err(invalid("at most 8 fallback models"));
                        }
                        for item in list {
                            let id = item
                                .as_str()
                                .ok_or_else(|| invalid("fallbacks is a list of model ids"))?;
                            if id.is_empty() || id.len() > 128 {
                                return Err(invalid("a fallback model id is 1 to 128 characters"));
                            }
                            route.fallbacks.push(id.to_string());
                        }
                    }
                    "prefer" => {
                        let word = v
                            .as_str()
                            .ok_or_else(|| invalid("prefer is price or speed"))?;
                        if !["", "price", "speed"].contains(&word) {
                            return Err(invalid("prefer is price or speed"));
                        }
                        route.prefer = word.to_string();
                    }
                    "private_only" => {
                        route.private_only = v
                            .as_bool()
                            .ok_or_else(|| invalid("private_only is true or false"))?
                    }
                    other => return Err(invalid(format!("unknown route option: {other}"))),
                }
            }
            request.route = Some(route);
        }
    } else if args.contains_key("route") || args.contains_key("remote_timeout") {
        return Err(invalid(
            "route and remote_timeout are for models on other computers",
        ));
    }
    if let Some(value) = args.get("priority") {
        match value.as_str() {
            Some("background") => request.priority = Priority::Background,
            Some("interactive") => {}
            _ => return Err(invalid("priority must be interactive or background")),
        }
    }
    Ok(request)
}

impl Inner {
    pub fn set_remote(&self, remote: Arc<dyn RemoteChat>) {
        self.queue.set_remote(remote);
    }

    pub fn new(
        connection: Connection,
        config: Config,
        backend: Arc<dyn Backend>,
        transcriber: Arc<dyn Transcriber>,
        image_generator: Arc<dyn ImageGenerator>,
    ) -> Self {
        let (events, mut inbox) = mpsc::unbounded_channel::<Event>();
        let owners: Owners = Arc::new(Mutex::new(HashMap::new()));

        let on_delta = {
            let events = events.clone();
            Arc::new(move |operation: u64, text: &str| {
                let _ = events.send(Event::Delta(operation, text.to_string()));
            })
        };
        let on_done = {
            let events = events.clone();
            Arc::new(move |operation: u64, outcome: &Outcome| {
                let _ = events.send(Event::Done(operation, outcome.clone()));
            })
        };
        let queue = Arc::new(InferenceQueue::new(
            backend,
            config.limits,
            on_delta,
            on_done,
        ));
        {
            let events = events.clone();
            queue.on_loading(Arc::new(move |operation, model: &str, fraction| {
                let _ = events.send(Event::Loading(operation, model.to_string(), fraction));
            }));
        }
        {
            let events = events.clone();
            queue.on_progress(Arc::new(move |operation, fraction, stage: &str| {
                let _ = events.send(Event::Progress(operation, fraction, stage.to_string()));
            }));
        }
        queue.set_transcriber(transcriber);
        queue.set_image_generator(image_generator);

        // Dispatcher: replies go only to whoever asked.
        let dispatch_owners = Arc::clone(&owners);
        let dispatch_connection = connection.clone();
        tokio::spawn(async move {
            while let Some(event) = inbox.recv().await {
                let (operation, name, done) = match &event {
                    Event::Delta(op, _) => (*op, "Delta", false),
                    Event::Done(op, _) => (*op, "Completed", true),
                    Event::Loading(op, _, _) => (*op, "Loading", false),
                    Event::Progress(op, _, _) => (*op, "Progress", false),
                };
                let sender = {
                    let mut table = dispatch_owners.lock().unwrap();
                    if done {
                        table.remove(&operation).map(|o| o.sender)
                    } else {
                        table.get(&operation).map(|o| o.sender.clone())
                    }
                };
                let Some(sender) = sender else { continue };
                let Ok(destination) = BusName::try_from(sender) else {
                    continue;
                };
                let result = match event {
                    Event::Delta(op, text) => {
                        dispatch_connection
                            .emit_signal(
                                Some(destination),
                                OBJECT_PATH,
                                INTERFACE,
                                name,
                                &(op, text),
                            )
                            .await
                    }
                    Event::Done(op, outcome) => {
                        dispatch_connection
                            .emit_signal(
                                Some(destination),
                                OBJECT_PATH,
                                INTERFACE,
                                name,
                                &(op, outcome.ok, outcome_json(&outcome).to_string()),
                            )
                            .await
                    }
                    Event::Loading(op, model, fraction) => {
                        dispatch_connection
                            .emit_signal(
                                Some(destination),
                                OBJECT_PATH,
                                INTERFACE,
                                name,
                                &(op, model, fraction),
                            )
                            .await
                    }
                    Event::Progress(op, fraction, stage) => {
                        dispatch_connection
                            .emit_signal(
                                Some(destination),
                                OBJECT_PATH,
                                INTERFACE,
                                name,
                                &(op, fraction, stage),
                            )
                            .await
                    }
                };
                if let Err(error) = result {
                    eprintln!("helix-inference: could not emit {name}: {error}");
                }
            }
        });

        Self {
            config,
            queue,
            owners,
            clients: Arc::new(Mutex::new(HashMap::new())),
            connection,
        }
    }

    /// Cancels what that caller still has queued or running.
    pub fn owner_gone(&self, name: &str) {
        self.clients.lock().unwrap().remove(name);
        let mine: Vec<(u64, String)> = self
            .owners
            .lock()
            .unwrap()
            .iter()
            .filter(|(_, owner)| owner.sender == name)
            .map(|(op, owner)| (*op, owner.client.clone()))
            .collect();
        for (operation, client) in mine {
            self.queue.cancel(&client, operation);
        }
    }

    /// Frees the model after a long idle spell; true when it did.
    pub fn tick(&self) -> bool {
        !self.config.idle_unload.is_zero() && self.queue.unload_if_idle(self.config.idle_unload)
    }

    async fn client_of(&self, sender: &str) -> Result<String, InferenceError> {
        if let Some(found) = self.clients.lock().unwrap().get(sender) {
            return Ok(found.clone());
        }
        // Process id only: this service runs sandboxed, in its own user namespace, where other processes' executables are
        // unreadable.
        let proxy = zbus::fdo::DBusProxy::new(&self.connection)
            .await
            .map_err(|e| InferenceError::Failed(e.to_string()))?;
        let name = BusName::try_from(sender.to_string())
            .map_err(|e| InferenceError::Failed(e.to_string()))?;
        let pid = proxy
            .get_connection_unix_process_id(name.clone())
            .await
            .map_err(|_| {
                InferenceError::Failed("the bus did not report the caller's process".into())
            })?;
        let uid = proxy.get_connection_unix_user(name).await.map_err(|_| {
            InferenceError::Failed("the bus did not report the caller's process".into())
        })?;
        // SAFETY: geteuid has no preconditions.
        if uid != unsafe { libc::geteuid() } {
            return Err(invalid("callers must run as the same user"));
        }
        let client = format!("pid:{pid}");
        self.clients
            .lock()
            .unwrap()
            .insert(sender.to_string(), client.clone());
        Ok(client)
    }

    fn register(
        &self,
        request: ChatRequest,
        sender: &str,
        client: &str,
    ) -> Result<u64, InferenceError> {
        // Held across submit so the dispatcher cannot look the operation up before it is recorded.
        let mut owners = self.owners.lock().unwrap();
        let operation = self.queue.submit(request).map_err(invalid)?;
        owners.insert(
            operation,
            Owner {
                sender: sender.to_string(),
                client: client.to_string(),
            },
        );
        Ok(operation)
    }
}

/// The registered interface object: a shared handle, so `main` keeps its own reference to the service state.
pub struct Service(pub Arc<Inner>);

#[interface(name = "org.helix.Inference1")]
impl Service {
    async fn get_snapshot(&self) -> HashMap<String, OwnedValue> {
        let mut snapshot = self.0.queue.snapshot();
        snapshot["model_dir"] = json!(self.0.config.model_dir);
        snapshot["idle_unload_seconds"] = json!(self.0.config.idle_unload.as_secs());
        let routing = RoutingTable::load();
        snapshot["backends"] = Json::Array(
            routing
                .backends
                .iter()
                .map(|b| json!({"id": b.id, "kind": b.kind, "base_url": b.base_url, "label": b.label}))
                .collect(),
        );
        let mut profiles = Map::new();
        for (profile, route) in &routing.profiles {
            profiles.insert(
                profile.clone(),
                json!({"backend": route.backend, "model": route.model}),
            );
        }
        snapshot["profiles"] = Json::Object(profiles);
        snapshot["allow_remote_on_battery_saver"] = json!(routing.allow_remote_on_battery_saver);
        snapshot
            .as_object()
            .map(|map| {
                map.iter()
                    .filter_map(|(k, v)| to_variant(v).map(|v| (k.clone(), v)))
                    .collect()
            })
            .unwrap_or_default()
    }

    async fn list_models(&self) -> Vec<OwnedValue> {
        self.0
            .queue
            .backend()
            .models()
            .iter()
            .filter_map(|m| {
                to_variant(
                    &json!({"id": m.id, "bytes": m.bytes, "loaded": m.loaded, "vision": m.vision}),
                )
            })
            .collect()
    }

    async fn action(
        &self,
        action: String,
        args: HashMap<String, OwnedValue>,
        #[zbus(header)] header: Header<'_>,
    ) -> Result<u64, InferenceError> {
        let sender = header
            .sender()
            .map(|s| s.to_string())
            .ok_or_else(|| InferenceError::Failed("no sender".into()))?;
        let mut args = args;
        // Pictures (aay, "images") and a recording (ay, "audio") travel as byte arrays and are taken out before the rest
        // becomes JSON: millions of numbers in a JSON array would cost more than the data itself.0.
        let mut encoded_pictures: Vec<Vec<u8>> = Vec::new();
        let mut recording: Option<Vec<u8>> = None;
        if let Some(value) = args.remove("images") {
            let list = Vec::<Vec<u8>>::try_from(value)
                .map_err(|_| invalid("images must be a list of byte arrays"))?;
            if list.len() > pictures::MAX_IMAGES {
                return Err(invalid("too many pictures"));
            }
            let mut total = 0usize;
            for bytes in &list {
                total += bytes.len();
                if bytes.len() > pictures::MAX_IMAGE_BYTES
                    || total > pictures::MAX_IMAGE_BYTES_TOTAL
                {
                    return Err(invalid("the pictures are too large"));
                }
            }
            encoded_pictures = list;
        }
        if let Some(value) = args.remove("audio") {
            let bytes =
                Vec::<u8>::try_from(value).map_err(|_| invalid("audio must be a byte array"))?;
            if bytes.len() > MAX_AUDIO_BYTES {
                return Err(invalid("the recording is too long"));
            }
            recording = Some(bytes);
        }
        let options: Map<String, Json> = args
            .iter()
            .map(|(key, value)| (key.clone(), from_variant(value)))
            .collect();
        let client = self.0.client_of(&sender).await?;

        match action.as_str() {
            "image" => {
                if recording.is_some() || !encoded_pictures.is_empty() {
                    return Err(invalid(
                        "image generation currently accepts a text prompt only",
                    ));
                }
                for key in options.keys() {
                    if !["prompt", "model", "width", "height", "steps", "seed"]
                        .contains(&key.as_str())
                    {
                        return Err(invalid(format!("unknown image argument: {key}")));
                    }
                }
                let Some(prompt) = options.get("prompt").and_then(Json::as_str) else {
                    return Err(invalid("an image prompt is required"));
                };
                let mut request = ChatRequest {
                    client: client.clone(),
                    generate_image: true,
                    image_prompt: prompt.to_string(),
                    ..ChatRequest::default()
                };
                if let Some(value) = options.get("model") {
                    let Some(text) = value.as_str() else {
                        return Err(invalid("image model must be a file name"));
                    };
                    request.model = text.to_string();
                }
                let number = |key: &str, fallback: u64, max: u64| -> Result<u64, InferenceError> {
                    match options.get(key) {
                        None => Ok(fallback),
                        Some(value) => match value.as_u64() {
                            Some(n) if n <= max => Ok(n),
                            _ => Err(invalid(format!(
                                "{key} must be a bounded non-negative integer"
                            ))),
                        },
                    }
                };
                request.image_width = number("width", 512, 768)? as u32;
                request.image_height = number("height", 512, 768)? as u32;
                request.image_steps = number("steps", 4, 8)? as u32;
                request.image_seed = number("seed", 0, 2_147_483_647)?;
                self.0.register(request, &sender, &client)
            }
            "transcribe" => {
                let Some(bytes) = recording else {
                    return Err(invalid("audio is required"));
                };
                if bytes.len() % 2 != 0 || bytes.is_empty() {
                    return Err(invalid("audio must be 16-bit samples"));
                }
                for key in options.keys() {
                    if key != "language" && key != "priority" {
                        return Err(invalid(format!("unknown argument: {key}")));
                    }
                }
                let mut request = ChatRequest {
                    client: client.clone(),
                    transcribe: true,
                    audio: bytes
                        .as_chunks::<2>()
                        .0
                        .iter()
                        .map(|pair| i16::from_le_bytes(*pair))
                        .collect(),
                    ..ChatRequest::default()
                };
                if let Some(value) = options.get("language") {
                    let plain = value.as_str().is_some_and(|text| {
                        text == "auto"
                            || ((2..=3).contains(&text.len())
                                && text.bytes().all(|c| c.is_ascii_lowercase()))
                    });
                    if !plain {
                        return Err(invalid("language must be auto or a language code like en"));
                    }
                    request.language = value.as_str().unwrap_or("auto").to_string();
                }
                self.0.register(request, &sender, &client)
            }
            "chat" => {
                if recording.is_some() {
                    return Err(invalid("unknown argument: audio"));
                }
                let routing = RoutingTable::load();
                let mut decoded = Vec::new();
                for bytes in &encoded_pictures {
                    decoded.push(pictures::decode_image_input(bytes).map_err(invalid)?);
                }
                let request = parse_chat(&options, &client, &routing, decoded)?;
                self.0.register(request, &sender, &client)
            }
            other => Err(invalid(format!("unknown action: {other}"))),
        }
    }

    async fn cancel(
        &self,
        operation: u64,
        #[zbus(header)] header: Header<'_>,
    ) -> Result<(), InferenceError> {
        let sender = header
            .sender()
            .map(|s| s.to_string())
            .ok_or_else(|| InferenceError::Failed("no sender".into()))?;
        let client = self.0.client_of(&sender).await?;
        let mine = self
            .0
            .owners
            .lock()
            .unwrap()
            .get(&operation)
            .is_some_and(|owner| owner.sender == sender);
        if !mine || !self.0.queue.cancel(&client, operation) {
            return Err(invalid("There is no cancellable operation with that id"));
        }
        Ok(())
    }

    #[zbus(signal)]
    async fn delta(
        emitter: &zbus::object_server::SignalEmitter<'_>,
        operation: u64,
        text: &str,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn completed(
        emitter: &zbus::object_server::SignalEmitter<'_>,
        operation: u64,
        ok: bool,
        result: &str,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn loading(
        emitter: &zbus::object_server::SignalEmitter<'_>,
        operation: u64,
        model: &str,
        fraction: f64,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn progress(
        emitter: &zbus::object_server::SignalEmitter<'_>,
        operation: u64,
        fraction: f64,
        stage: &str,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn changed(emitter: &zbus::object_server::SignalEmitter<'_>) -> zbus::Result<()>;
}

#[cfg(test)]
mod routing_tests {
    use super::*;
    use serde_json::json;

    fn table() -> RoutingTable {
        RoutingTable::parse(
            "allow_remote_on_battery_saver = true\n[[backends]]\nid = \"lan\"\nkind = \"openai\"\nbase_url = \"http://studio:8080/v1\"\n\
             [[backends]]\nid = \"odd\"\nkind = \"carrier-pigeon\"\n\
             [profiles.chat]\nbackend = \"lan\"\nmodel = \"big\"\n\
             [profiles.code]\nbackend = \"odd\"\nmodel = \"x\"\n\
             [profiles.summarize]\nbackend = \"missing\"\n\
             [profiles.act]\nbackend = \"lan\"\nmodel = \"big\"\n\
             [profiles.vision]\nbackend = \"local\"\nmodel = \"small.gguf\"\n",
        )
    }

    fn args(task: &str) -> Map<String, Json> {
        let v = json!({"messages": [{"role": "user", "content": "hi"}], "task": task});
        v.as_object().unwrap().clone()
    }

    #[test]
    fn a_slot_the_person_routed_elsewhere_goes_there_and_nothing_else_does() {
        let routing = table();
        let request = parse_chat(&args("chat"), "c", &routing, vec![]).unwrap();
        assert_eq!(
            (request.backend.as_str(), request.model.as_str()),
            ("lan", "big")
        );
        // A local slot stays on the engine.
        let request = parse_chat(&args("vision"), "c", &routing, vec![]).unwrap();
        assert_eq!(
            (request.backend.as_str(), request.model.as_str()),
            ("", "small.gguf")
        );
        // Only a model on this computer may act for the desktop, wherever the slot is routed.
        assert!(parse_chat(&args("act"), "c", &routing, vec![]).is_err());
        // A backend that is missing or of an unknown kind is an error, never a fall back to local.
        assert!(parse_chat(&args("summarize"), "c", &routing, vec![]).is_err());
        assert!(parse_chat(&args("code"), "c", &routing, vec![]).is_err());
        // An output grammar never goes to another machine.
        let mut with_schema = args("chat");
        with_schema.insert("schema".into(), json!("{}"));
        assert!(parse_chat(&with_schema, "c", &routing, vec![]).is_err());
    }

    #[test]
    fn a_caller_may_name_a_configured_backend_and_how_it_should_choose() {
        let routing = table();
        let mut named = args("chat");
        named.insert("backend".into(), json!("lan"));
        named.insert("model".into(), json!("mine"));
        named.insert("remote_timeout".into(), json!(9999));
        named.insert(
            "route".into(),
            json!({"fallbacks": ["x/y"], "prefer": "price", "private_only": true}),
        );
        let request = parse_chat(&named, "c", &routing, vec![]).unwrap();
        assert_eq!(
            (request.backend.as_str(), request.model.as_str()),
            ("lan", "mine")
        );
        assert_eq!(request.timeout_secs, 600, "clamped");
        assert_eq!(request.route.unwrap().fallbacks, ["x/y"]);
        // Not a backend the person configured, not for Act, nothing odd in the options.
        let with = |key: &str, value: Json| {
            let mut a = args("chat");
            a.insert("backend".into(), json!("lan"));
            a.insert(key.into(), value);
            a
        };
        let mut unknown = args("chat");
        unknown.insert("backend".into(), json!("nowhere"));
        assert!(parse_chat(&unknown, "c", &routing, vec![]).is_err());
        let mut act = args("act");
        act.insert("backend".into(), json!("lan"));
        assert!(parse_chat(&act, "c", &routing, vec![]).is_err());
        assert!(parse_chat(
            &with("route", json!({"prefer": "vibes"})),
            "c",
            &routing,
            vec![]
        )
        .is_err());
        assert!(parse_chat(
            &with("route", json!({"colour": "red"})),
            "c",
            &routing,
            vec![]
        )
        .is_err());
        assert!(parse_chat(
            &with("route", json!({"fallbacks": vec!["a"; 9]})),
            "c",
            &routing,
            vec![]
        )
        .is_err());
        // Options for another machine are an error on a local request.
        let mut local = args("vision");
        local.insert("route".into(), json!({}));
        assert!(parse_chat(&local, "c", &routing, vec![]).is_err());
        // With battery saver on and not allowed, other machines are not used.
        let strict = RoutingTable::parse("[[backends]]\nid = \"lan\"\nkind = \"openai\"\nbase_url = \"http://studio/v1\"\n[profiles.chat]\nbackend = \"lan\"\n");
        let saving = std::fs::read_to_string("/sys/firmware/acpi/platform_profile")
            .is_ok_and(|p| matches!(p.trim(), "low-power" | "cool"));
        if saving {
            assert!(parse_chat(&args("chat"), "c", &strict, vec![]).is_err());
        }
    }
}
