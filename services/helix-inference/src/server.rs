//! The llama.cpp engine as a supervised child process: `llama-server`, spoken to over a private Unix socket in OpenAI's chat format. The service
//! starts it when a model is asked for, watches it, restarts it if it dies between requests, and stops it to unload. A crash in the engine can no
//! longer take the service down, the engine needs no C++ in this crate's build, and an engine update is one replaced binary.
//!
//! Only this process talks to the child: the socket is created in the service's runtime directory with the child's own `--host <path>.sock`, nothing
//! listens on a TCP port, and the child is killed if the service dies (`PR_SET_PDEATHSIG`).

use std::io::{ErrorKind, Read, Write};
use std::os::unix::net::UnixStream;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use crate::backend::*;
use crate::grammar;
use crate::models;
use crate::sandbox;

#[derive(Clone, Debug)]
pub struct ServerConfig {
    /// The `llama-server` program.
    pub binary: PathBuf,
    /// Where its shared libraries are (added to `LD_LIBRARY_PATH`).
    pub library_dirs: Vec<PathBuf>,
    pub model_dir: PathBuf,
    /// Layers on the GPU: -1 means every layer the device can hold.
    pub gpu_layers: i32,
    pub context_tokens: u32,
    /// How long a model may take to load.
    pub load_timeout: Duration,
    /// Whether the engine runs in a `bwrap` box of its own (see sandbox.rs).
    pub sandbox: sandbox::Mode,
}

impl ServerConfig {
    pub fn new(binary: PathBuf, model_dir: PathBuf) -> Self {
        let library_dirs = binary
            .parent()
            .map(|d| vec![d.to_path_buf()])
            .unwrap_or_default();
        Self {
            binary,
            library_dirs,
            model_dir,
            gpu_layers: -1,
            context_tokens: 4096,
            load_timeout: Duration::from_secs(240),
            sandbox: sandbox::Mode::Off,
        }
    }
}

struct Running {
    child: Child,
    socket: PathBuf,
    model: String,
}

pub struct ServerBackend {
    config: ServerConfig,
    running: Mutex<Option<Running>>,
    progress: Mutex<Option<ProgressFn>>,
    devices: Mutex<Option<(Instant, Value)>>,
}

/// A folder only this process can enter, for the engine's socket (under the service's private temporary space when it runs as a unit).
fn private_dir() -> std::io::Result<PathBuf> {
    use std::os::unix::fs::DirBuilderExt;
    let dir = std::env::temp_dir().join(format!("helix-inference-{}", std::process::id()));
    match std::fs::DirBuilder::new().mode(0o700).create(&dir) {
        Ok(()) => Ok(dir),
        Err(e) if e.kind() == ErrorKind::AlreadyExists => Ok(dir),
        Err(e) => Err(e),
    }
}

impl ServerBackend {
    pub fn new(config: ServerConfig) -> Self {
        Self {
            config,
            running: Mutex::new(None),
            progress: Mutex::new(None),
            devices: Mutex::new(None),
        }
    }

    fn report(&self, model: &str, fraction: f64) {
        if let Some(callback) = self.progress.lock().unwrap().as_ref() {
            callback(model, fraction);
        }
    }

    /// What the engine is given to run with (the box starts with nothing else).
    fn environment(&self, work: &Path) -> Vec<(String, String)> {
        let mut paths: Vec<String> = self
            .config
            .library_dirs
            .iter()
            .map(|p| p.to_string_lossy().into_owned())
            .collect();
        if let Some(existing) = std::env::var_os("LD_LIBRARY_PATH") {
            paths.push(existing.to_string_lossy().into_owned());
        }
        let mut env = vec![
            ("LD_LIBRARY_PATH".to_string(), paths.join(":")),
            // Compiled-kernel caches go to the engine's own space, not a home folder.
            (
                "CUDA_CACHE_PATH".to_string(),
                work.join("cuda-cache").to_string_lossy().into_owned(),
            ),
            (
                "XDG_CACHE_HOME".to_string(),
                work.join("cache").to_string_lossy().into_owned(),
            ),
        ];
        // Choices about which device to use pass through.
        for key in [
            "CUDA_VISIBLE_DEVICES",
            "VK_ICD_FILENAMES",
            "GGML_VK_VISIBLE_DEVICES",
            "HIP_VISIBLE_DEVICES",
        ] {
            if let Ok(value) = std::env::var(key) {
                env.push((key.to_string(), value));
            }
        }
        env
    }

    /// The engine program, boxed or not as the mode and the machine allow.
    fn command(&self) -> Result<Command, String> {
        let work = private_dir().map_err(|e| format!("no private folder for the engine: {e}"))?;
        let env = self.environment(&work);
        let boxed = match self.config.sandbox {
            sandbox::Mode::Off => false,
            sandbox::Mode::Auto => match sandbox::works() {
                Ok(()) => true,
                Err(why) => {
                    static SAID: std::sync::Once = std::sync::Once::new();
                    SAID.call_once(|| {
                        eprintln!(
                            "helix-inference: the engine runs without a box of its own: {why}"
                        )
                    });
                    false
                }
            },
            sandbox::Mode::Require => {
                sandbox::works()
                    .map_err(|why| format!("the engine would not start without its box: {why}"))?;
                true
            }
        };
        if !boxed {
            let mut command = Command::new(&self.config.binary);
            command.envs(env);
            return Ok(command);
        }
        let devices = sandbox::gpu_devices();
        let spec = sandbox::Spec {
            binary: &self.config.binary,
            library_dirs: &self.config.library_dirs,
            model_dir: &self.config.model_dir,
            work_dir: &work,
            devices: &devices,
            env: &env,
        };
        let mut command = Command::new("bwrap");
        command
            .args(sandbox::bwrap_args(&spec, &|p| p.exists()))
            .arg("--")
            .arg(&self.config.binary);
        Ok(command)
    }

    /// Starts the engine on `model` and waits until it answers. The previous one, if any, is stopped first.
    fn start(&self, model: &str) -> Result<Running, String> {
        let path = self.config.model_dir.join(model);
        let dir =
            private_dir().map_err(|e| format!("no private folder for the engine's socket: {e}"))?;
        let socket = dir.join("engine.sock");
        let _ = std::fs::remove_file(&socket);
        let mut command = self.command()?;
        command
            .arg("-m")
            .arg(&path)
            .arg("--host")
            .arg(&socket)
            .args(["-c", &self.config.context_tokens.to_string()])
            .args(["-np", "1", "--no-webui", "--no-perf", "-lv", "1"]);
        if self.config.gpu_layers >= 0 {
            command.args(["-ngl", &self.config.gpu_layers.to_string()]);
        } else {
            command.args(["-ngl", "999"]);
        }
        if let Some(projector) = models::projector_for(&self.config.model_dir, model) {
            command.arg("--mmproj").arg(projector);
        }
        command
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::inherit());
        // SAFETY: only async-signal-safe `prctl` runs between fork and exec; it asks the kernel to kill the engine when this process dies.
        unsafe {
            command.pre_exec(|| {
                libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGKILL);
                Ok(())
            });
        }
        let mut child = command.spawn().map_err(|e| {
            format!(
                "the engine could not be started ({}): {e}",
                self.config.binary.display()
            )
        })?;
        self.report(model, 0.0);
        let started = Instant::now();
        loop {
            if let Ok(Some(status)) = child.try_wait() {
                let _ = std::fs::remove_file(&socket);
                return Err(format!(
                    "the engine stopped while loading {model} ({status})"
                ));
            }
            if health(&socket) {
                break;
            }
            if started.elapsed() > self.config.load_timeout {
                stop(&mut child);
                let _ = std::fs::remove_file(&socket);
                return Err(format!("the model took too long to load: {model}"));
            }
            std::thread::sleep(Duration::from_millis(100));
            // The engine reports no load progress of its own, so show time against what a load usually takes, never reaching the end early.
            let guess = (started.elapsed().as_secs_f64() / 20.0).min(0.95);
            self.report(model, guess);
        }
        self.report(model, 1.0);
        Ok(Running {
            child,
            socket,
            model: model.to_string(),
        })
    }

    fn stop_running(&self) {
        if let Some(mut running) = self.running.lock().unwrap().take() {
            stop(&mut running.child);
            let _ = std::fs::remove_file(&running.socket);
        }
    }

    /// The socket of an engine serving `model`: the running one if it is alive, otherwise a fresh start.
    fn ensure(&self, requested: &str) -> Result<(PathBuf, String), String> {
        let listed = models::list(&self.config.model_dir);
        let name = if requested.is_empty() {
            listed.first().map(|m| m.id.clone()).ok_or_else(|| {
                format!(
                    "no models are installed in {}",
                    self.config.model_dir.display()
                )
            })?
        } else {
            requested.to_string()
        };
        {
            let mut guard = self.running.lock().unwrap();
            if let Some(running) = guard.as_mut() {
                let alive = matches!(running.child.try_wait(), Ok(None));
                if alive && running.model == name {
                    return Ok((running.socket.clone(), name));
                }
            }
        }
        // Only a model the listing offers can be loaded.
        if !listed.iter().any(|m| m.id == name) {
            return Err(format!("no such model: {name}"));
        }
        self.stop_running();
        let running = self.start(&name)?;
        let socket = running.socket.clone();
        *self.running.lock().unwrap() = Some(running);
        Ok((socket, name))
    }

    /// `llama-server --list-devices`, as the service's budget wants it (name, memory), cached for a short while.
    fn devices(&self) -> Value {
        let mut cached = self.devices.lock().unwrap();
        if let Some((at, value)) = cached.as_ref() {
            if at.elapsed() < Duration::from_secs(30) {
                return value.clone();
            }
        }
        let text = self
            .command()
            .map(|mut c| {
                c.arg("--list-devices");
                c
            })
            .and_then(|mut c| {
                c.stdin(Stdio::null())
                    .stderr(Stdio::null())
                    .output()
                    .map_err(|e| e.to_string())
            })
            .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
            .unwrap_or_default();
        let value = parse_devices(&text);
        *cached = Some((Instant::now(), value.clone()));
        value
    }
}

impl Drop for ServerBackend {
    fn drop(&mut self) {
        self.stop_running();
    }
}

fn stop(child: &mut Child) {
    // SAFETY: `kill` on our own child's pid; the child is reaped below.
    unsafe { libc::kill(child.id() as i32, libc::SIGTERM) };
    let until = Instant::now() + Duration::from_secs(3);
    while Instant::now() < until {
        if matches!(child.try_wait(), Ok(Some(_))) {
            return;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    let _ = child.kill();
    let _ = child.wait();
}

/// `  CUDA0: NVIDIA GeForce RTX 4080 Laptop GPU (11931 MiB, 8052 MiB free)` lines.
fn parse_devices(text: &str) -> Value {
    let mut devices = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        let Some((name, rest)) = line.split_once(": ") else {
            continue;
        };
        let Some(open) = rest.rfind(" (") else {
            continue;
        };
        let (description, memory) = (&rest[..open], rest[open + 2..].trim_end_matches(')'));
        let mut numbers = memory.split(" MiB").map(|part| {
            part.trim_start_matches([',', ' '])
                .trim()
                .parse::<u64>()
                .ok()
        });
        let (Some(Some(total)), Some(Some(free))) = (numbers.next(), numbers.next()) else {
            continue;
        };
        devices.push(json!({
            "name": name,
            "description": description,
            "gpu": !name.starts_with("CPU"),
            "memory_free": free * 1024 * 1024,
            "memory_total": total * 1024 * 1024,
        }));
    }
    Value::Array(devices)
}

// --- a small HTTP/1.1 client for the child's socket ---------------------------------------------------------------------------------------

fn connect(socket: &Path) -> std::io::Result<UnixStream> {
    UnixStream::connect(socket)
}

fn health(socket: &Path) -> bool {
    let Ok(mut stream) = connect(socket) else {
        return false;
    };
    let _ = stream.set_read_timeout(Some(Duration::from_secs(2)));
    if stream
        .write_all(b"GET /health HTTP/1.1\r\nHost: engine\r\nConnection: close\r\n\r\n")
        .is_err()
    {
        return false;
    }
    let mut reply = Vec::new();
    let _ = stream.read_to_end(&mut reply);
    let text = String::from_utf8_lossy(&reply);
    text.starts_with("HTTP/1.1 200") && text.contains("\"ok\"")
}

/// Decodes `Transfer-Encoding: chunked` as bytes arrive, in whatever pieces they come.
#[derive(Default)]
struct Chunked {
    buffer: Vec<u8>,
    /// Bytes still to take from the current chunk (plus its closing CRLF), when inside one.
    left: Option<usize>,
    done: bool,
}

impl Chunked {
    fn feed(&mut self, bytes: &[u8]) -> Vec<u8> {
        self.buffer.extend_from_slice(bytes);
        let mut out = Vec::new();
        loop {
            match self.left {
                None => {
                    let Some(end) = self.buffer.windows(2).position(|w| w == b"\r\n") else {
                        break;
                    };
                    let line = String::from_utf8_lossy(&self.buffer[..end]).into_owned();
                    self.buffer.drain(..end + 2);
                    let size =
                        usize::from_str_radix(line.split(';').next().unwrap_or("").trim(), 16)
                            .unwrap_or(0);
                    if size == 0 {
                        self.done = true;
                        break;
                    }
                    self.left = Some(size);
                }
                Some(left) => {
                    let take = left.min(self.buffer.len());
                    out.extend(self.buffer.drain(..take));
                    if take < left {
                        self.left = Some(left - take);
                        break;
                    }
                    // The CRLF that closes the chunk.
                    if self.buffer.len() < 2 {
                        self.left = Some(0);
                        break;
                    }
                    self.buffer.drain(..2);
                    self.left = None;
                }
            }
            if self.left == Some(0) && self.buffer.len() >= 2 {
                self.buffer.drain(..2);
                self.left = None;
            }
        }
        out
    }
}

struct Head {
    status: u16,
    chunked: bool,
    length: Option<usize>,
}

fn parse_head(head: &str) -> Head {
    let mut lines = head.lines();
    let status = lines
        .next()
        .and_then(|l| l.split_whitespace().nth(1))
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);
    let (mut chunked, mut length) = (false, None);
    for line in lines {
        let lower = line.to_ascii_lowercase();
        if let Some(v) = lower.strip_prefix("transfer-encoding:") {
            chunked = v.contains("chunked");
        }
        if let Some(v) = lower.strip_prefix("content-length:") {
            length = v.trim().parse().ok();
        }
    }
    Head {
        status,
        chunked,
        length,
    }
}

/// What a request to the child produced.
struct Streamed {
    status: u16,
    /// The body when it was not a stream (an error), else empty.
    body: String,
    cancelled: bool,
}

/// POSTs `body` and calls `on_line` with each `data:` line of the server-sent events that come back. Returns early, closing the connection (which
/// stops the engine's generation), as soon as `cancelled()` is true. `on_line` returns false to stop reading.
fn post_stream(
    socket: &Path,
    path: &str,
    body: &str,
    cancelled: &dyn Fn() -> bool,
    on_line: &mut dyn FnMut(&str) -> bool,
) -> Result<Streamed, String> {
    let mut stream = connect(socket).map_err(|e| format!("the engine is not answering: {e}"))?;
    stream
        .set_read_timeout(Some(Duration::from_millis(100)))
        .map_err(|e| e.to_string())?;
    let request = format!(
        "POST {path} HTTP/1.1\r\nHost: engine\r\nContent-Type: application/json\r\nAccept: text/event-stream\r\nConnection: close\r\nContent-Length: {}\r\n\r\n",
        body.len()
    );
    stream
        .write_all(request.as_bytes())
        .and_then(|_| stream.write_all(body.as_bytes()))
        .map_err(|e| format!("the engine is not answering: {e}"))?;
    let mut raw = Vec::new();
    let mut head: Option<Head> = None;
    let mut chunked = Chunked::default();
    let mut text = Vec::new();
    let mut error_body = Vec::new();
    let mut buf = [0u8; 16 * 1024];
    loop {
        if cancelled() {
            return Ok(Streamed {
                status: head.map_or(0, |h| h.status),
                body: String::new(),
                cancelled: true,
            });
        }
        match stream.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => {
                let piece: Vec<u8> = if head.is_none() {
                    raw.extend_from_slice(&buf[..n]);
                    let Some(end) = raw.windows(4).position(|w| w == b"\r\n\r\n") else {
                        continue;
                    };
                    head = Some(parse_head(&String::from_utf8_lossy(&raw[..end])));
                    raw.drain(..end + 4).for_each(drop);
                    std::mem::take(&mut raw)
                } else {
                    buf[..n].to_vec()
                };
                let h = head.as_ref().unwrap();
                if h.status != 200 {
                    error_body.extend_from_slice(&if h.chunked {
                        chunked.feed(&piece)
                    } else {
                        piece
                    });
                    if h.length.is_some_and(|l| error_body.len() >= l) || chunked.done {
                        break;
                    }
                    continue;
                }
                let decoded = if h.chunked {
                    chunked.feed(&piece)
                } else {
                    piece
                };
                text.extend_from_slice(&decoded);
                while let Some(end) = text.iter().position(|b| *b == b'\n') {
                    let line = String::from_utf8_lossy(&text[..end])
                        .trim_end_matches('\r')
                        .to_string();
                    text.drain(..=end);
                    if let Some(data) = line.strip_prefix("data:") {
                        if !on_line(data.trim_start()) {
                            return Ok(Streamed {
                                status: 200,
                                body: String::new(),
                                cancelled: false,
                            });
                        }
                    }
                }
                if chunked.done {
                    break;
                }
            }
            Err(e)
                if matches!(
                    e.kind(),
                    ErrorKind::WouldBlock | ErrorKind::TimedOut | ErrorKind::Interrupted
                ) => {}
            Err(e) => return Err(format!("the engine stopped answering: {e}")),
        }
    }
    let status = head.as_ref().map_or(0, |h| h.status);
    if status == 0 {
        return Err("the engine closed the connection without answering".into());
    }
    Ok(Streamed {
        status,
        body: String::from_utf8_lossy(&error_body).into_owned(),
        cancelled: false,
    })
}

// --- requests ---------------------------------------------------------------------------------------------------------------------------

fn png_data_url(image: &ImageInput) -> Result<String, String> {
    use base64::Engine as _;
    let buffer = image::RgbImage::from_raw(image.width, image.height, image.rgb.clone())
        .ok_or("a picture's pixels do not match its size")?;
    let mut png = Vec::new();
    image::DynamicImage::ImageRgb8(buffer)
        .write_to(&mut std::io::Cursor::new(&mut png), image::ImageFormat::Png)
        .map_err(|e| e.to_string())?;
    Ok(format!(
        "data:image/png;base64,{}",
        base64::engine::general_purpose::STANDARD.encode(png)
    ))
}

/// The service's messages (`{"type":"image","index":N}` parts) as OpenAI chat messages (`image_url` parts carrying the picture).
fn chat_messages(messages: &Value, images: &[ImageInput]) -> Result<Value, String> {
    let mut out = Vec::new();
    for message in messages.as_array().ok_or("messages is a list")? {
        let role = message
            .get("role")
            .and_then(Value::as_str)
            .ok_or("a message has a role")?;
        let content = match message.get("content") {
            Some(Value::String(text)) => Value::String(text.clone()),
            Some(Value::Array(parts)) => {
                let mut converted = Vec::new();
                for part in parts {
                    match part.get("type").and_then(Value::as_str) {
                        Some("text") => converted.push(json!({"type": "text", "text": part.get("text").and_then(Value::as_str).unwrap_or("")})),
                        Some("image") => {
                            let index = part.get("index").and_then(Value::as_u64).ok_or("a picture has an index")? as usize;
                            let image = images.get(index).ok_or("a message names a picture that was not sent")?;
                            converted.push(json!({"type": "image_url", "image_url": {"url": png_data_url(image)?}}));
                        }
                        _ => return Err("a message part is neither text nor a picture".into()),
                    }
                }
                Value::Array(converted)
            }
            _ => return Err("a message has content".into()),
        };
        out.push(json!({"role": role, "content": content}));
    }
    Ok(Value::Array(out))
}

fn request_body(request: &ChatRequest) -> Result<String, String> {
    let mut body = json!({
        "messages": chat_messages(&request.messages, &request.images)?,
        "stream": true,
        "stream_options": {"include_usage": true},
        "max_tokens": request.max_tokens,
        "temperature": request.temperature,
        "cache_prompt": true,
    });
    if request.temperature <= 0.0 {
        // Greedy, as the in-process engine was.
        body["top_k"] = json!(1);
    }
    if !request.json_schema.is_empty() {
        // The grammar first: a request that cannot be constrained is refused, never run unconstrained.
        let schema: Value = serde_json::from_str(&request.json_schema)
            .map_err(|e| format!("the output schema is not supported: {e}"))?;
        let grammar = grammar::from_tool_schema(
            &schema,
            grammar::Options {
                reason_first: request.reason_first,
            },
        )
        .map_err(|e| format!("the output schema is not supported: {e}"))?;
        body["grammar"] = json!(grammar);
    }
    Ok(body.to_string())
}

impl Backend for ServerBackend {
    fn models(&self) -> Vec<ModelInfo> {
        let loaded = self
            .running
            .lock()
            .unwrap()
            .as_ref()
            .map(|r| r.model.clone());
        models::list(&self.config.model_dir)
            .into_iter()
            .map(|m| ModelInfo {
                loaded: loaded.as_deref() == Some(m.id.as_str()),
                id: m.id,
                bytes: m.bytes,
                vision: m.vision,
            })
            .collect()
    }

    fn load(&self, model: &str) -> Result<(), String> {
        self.ensure(model).map(|_| ())
    }

    fn unload(&self) {
        self.stop_running();
    }

    fn is_loaded(&self) -> bool {
        self.running
            .lock()
            .unwrap()
            .as_mut()
            .is_some_and(|r| matches!(r.child.try_wait(), Ok(None)))
    }

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
        let body = match request_body(request) {
            Ok(body) => body,
            Err(error) => return fail(error),
        };
        if !request.images.is_empty()
            && models::projector_for(&self.config.model_dir, &request.model).is_none()
            && !request.model.is_empty()
        {
            return fail(format!(
                "this model cannot read pictures: no vision projector is installed for {}",
                request.model
            ));
        }
        let mut outcome = Outcome::default();
        // A second try is made only when the engine died before saying anything: a restart is then invisible to the caller.
        for attempt in 0..2 {
            let (socket, _) = match self.ensure(&request.model) {
                Ok(found) => found,
                Err(error) => return fail(error),
            };
            let mut finish = String::new();
            let mut spoke = false;
            let result = post_stream(
                &socket,
                "/v1/chat/completions",
                &body,
                cancelled,
                &mut |data| {
                    if data == "[DONE]" {
                        return false;
                    }
                    let Ok(value) = serde_json::from_str::<Value>(data) else {
                        return true;
                    };
                    if let Some(delta) = value
                        .pointer("/choices/0/delta/content")
                        .and_then(Value::as_str)
                    {
                        if !delta.is_empty() {
                            spoke = true;
                            outcome.text.push_str(delta);
                            on_delta(delta);
                        }
                    }
                    if let Some(reason) = value
                        .pointer("/choices/0/finish_reason")
                        .and_then(Value::as_str)
                    {
                        finish = reason.to_string();
                    }
                    if let Some(usage) = value.get("usage").filter(|u| u.is_object()) {
                        outcome.prompt_tokens = usage["prompt_tokens"].as_u64().unwrap_or(0) as u32;
                        outcome.completion_tokens =
                            usage["completion_tokens"].as_u64().unwrap_or(0) as u32;
                    }
                    if let Some(cached) = value.pointer("/timings/cache_n").and_then(Value::as_u64)
                    {
                        outcome.cached_prompt_tokens = cached as u32;
                    }
                    true
                },
            );
            match result {
                Ok(streamed) if streamed.cancelled => {
                    outcome.cancelled = true;
                    return outcome;
                }
                Ok(streamed) if streamed.status == 200 => {
                    outcome.ok = true;
                    outcome.truncated = finish == "length";
                    return outcome;
                }
                Ok(streamed) => {
                    let message = serde_json::from_str::<Value>(&streamed.body)
                        .ok()
                        .and_then(|v| {
                            v.pointer("/error/message")
                                .and_then(Value::as_str)
                                .map(str::to_string)
                        })
                        .unwrap_or_else(|| {
                            format!("the engine refused the request ({})", streamed.status)
                        });
                    return fail(
                        if message.contains("exceed") && message.contains("context") {
                            "the request does not fit in the model's context window".into()
                        } else {
                            message
                        },
                    );
                }
                Err(error) => {
                    if spoke || attempt == 1 {
                        return Outcome { error, ..outcome };
                    }
                    // The engine is gone: drop it so the next `ensure` starts a new one.
                    self.stop_running();
                }
            }
        }
        outcome
    }

    fn snapshot(&self) -> Value {
        let loaded = self
            .running
            .lock()
            .unwrap()
            .as_ref()
            .map(|r| r.model.clone())
            .unwrap_or_default();
        json!({
            "backend": "llama-server",
            "loaded": loaded,
            "gpuLayers": self.config.gpu_layers,
            "contextTokens": self.config.context_tokens,
            "devices": self.devices(),
        })
    }

    fn set_progress(&self, progress: ProgressFn) {
        *self.progress.lock().unwrap() = Some(progress);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn chunked_bodies_are_decoded_whatever_the_pieces() {
        let whole = b"5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        for split in 1..whole.len() {
            let mut d = Chunked::default();
            let mut out = d.feed(&whole[..split]);
            out.extend(d.feed(&whole[split..]));
            assert_eq!(out, b"hello world", "split at {split}");
            assert!(d.done);
        }
    }

    #[test]
    fn devices_are_read_from_the_engines_list() {
        let v = parse_devices("Available devices:\n  CUDA0: NVIDIA GeForce RTX 4080 Laptop GPU (11931 MiB, 8052 MiB free)\n  Vulkan0: NVIDIA GeForce RTX 4080 Laptop GPU (12282 MiB, 8052 MiB free)\n");
        assert_eq!(v.as_array().unwrap().len(), 2);
        assert_eq!(v[0]["name"], "CUDA0");
        assert_eq!(v[0]["memory_free"], 8052u64 * 1024 * 1024);
        assert_eq!(v[0]["gpu"], true);
    }

    #[test]
    fn messages_carry_their_pictures() {
        let image = ImageInput {
            width: 2,
            height: 1,
            rgb: vec![255, 0, 0, 0, 255, 0],
        };
        let messages = json!([
            {"role": "system", "content": "be brief"},
            {"role": "user", "content": [{"type": "text", "text": "what is this?"}, {"type": "image", "index": 0}]}
        ]);
        let out = chat_messages(&messages, &[image.clone()]).unwrap();
        assert_eq!(out[0]["content"], "be brief");
        assert!(out[1]["content"][1]["image_url"]["url"]
            .as_str()
            .unwrap()
            .starts_with("data:image/png;base64,"));
        let missing = json!([{"role": "user", "content": [{"type": "image", "index": 3}]}]);
        assert!(chat_messages(&missing, &[image]).is_err());
        assert!(chat_messages(&json!([{"role": "user"}]), &[]).is_err());
    }

    #[test]
    fn a_request_that_cannot_be_constrained_is_refused() {
        let mut request = ChatRequest {
            messages: json!([{"role": "user", "content": "hi"}]),
            ..ChatRequest::default()
        };
        assert!(request_body(&request).is_ok());
        request.json_schema = "{\"oneOf\": []}".into();
        assert!(request_body(&request)
            .unwrap_err()
            .contains("not supported"));
        request.json_schema = "not json".into();
        assert!(request_body(&request).is_err());
    }

    // --- the real engine -------------------------------------------------------------------------------------------------------------
    // These need `llama-server` and a model: `HELIX_LLAMA_SERVER=… HELIX_MODEL_DIR=… cargo test -p helix-inference real_engine -- --ignored`.

    fn real() -> Option<ServerBackend> {
        let binary = PathBuf::from(std::env::var_os("HELIX_LLAMA_SERVER")?);
        let dir = PathBuf::from(std::env::var_os("HELIX_MODEL_DIR")?);
        let mut config = ServerConfig::new(binary, dir);
        config.library_dirs.extend(
            std::env::var_os("HELIX_LLAMA_LIBS")
                .map(|l| {
                    l.to_string_lossy()
                        .split(':')
                        .map(PathBuf::from)
                        .collect::<Vec<_>>()
                })
                .unwrap_or_default(),
        );
        config.sandbox = sandbox::Mode::from_env(
            &std::env::var("HELIX_ENGINE_SANDBOX").unwrap_or_else(|_| "require".into()),
        );
        Some(ServerBackend::new(config))
    }

    fn ask(text: &str, schema: &str) -> ChatRequest {
        ChatRequest {
            messages: json!([{"role": "user", "content": text}]),
            max_tokens: 64,
            json_schema: schema.into(),
            ..ChatRequest::default()
        }
    }

    #[test]
    #[ignore]
    fn real_engine_streams_constrains_cancels_and_restarts() {
        let Some(backend) = real() else { return };
        let none = || false;
        // It streams: the deltas add up to the text.
        let mut pieces = String::new();
        let out = backend.chat(
            &ask("Say hello in five words.", ""),
            &mut |d| pieces.push_str(d),
            &none,
        );
        assert!(out.ok, "{}", out.error);
        assert_eq!(pieces, out.text);
        assert!(out.completion_tokens > 0 && out.prompt_tokens > 0);
        assert!(backend.is_loaded());
        assert!(backend.models().iter().any(|m| m.loaded));
        // The same prompt again is largely remembered by the engine.
        let again = backend.chat(&ask("Say hello in five words.", ""), &mut |_| {}, &none);
        assert!(again.cached_prompt_tokens > 0, "no prompt cache: {again:?}");
        // The grammar: only a tool call from the schema can come out.
        let schema = r#"{"oneOf":[{"type":"object","required":["tool","args"],"properties":{"tool":{"const":"open-app"},"args":{"prefixItems":[{"enum":["files","terminal"]}],"minItems":1}}}]}"#;
        let out = backend.chat(&ask("Open the file manager.", schema), &mut |_| {}, &none);
        assert!(out.ok, "{}", out.error);
        let call: Value = serde_json::from_str(&out.text)
            .unwrap_or_else(|e| panic!("not JSON ({e}): {}", out.text));
        assert_eq!(call["tool"], "open-app");
        assert!(["files", "terminal"].contains(&call["args"][0].as_str().unwrap()));
        // Cancelling stops it promptly and keeps what was said.
        let started = Instant::now();
        let said = std::cell::Cell::new(0usize);
        let out = backend.chat(
            &ChatRequest {
                max_tokens: 400,
                ..ask("Count from one to two hundred, one number per line.", "")
            },
            &mut |d| said.set(said.get() + d.len()),
            &|| said.get() > 20,
        );
        assert!(out.cancelled && !out.text.is_empty());
        assert!(started.elapsed() < Duration::from_secs(10));
        // An engine that died between requests is started again without the caller noticing.
        {
            let mut guard = backend.running.lock().unwrap();
            let running = guard.as_mut().unwrap();
            let _ = running.child.kill();
            let _ = running.child.wait();
        }
        let out = backend.chat(&ask("Say OK.", ""), &mut |_| {}, &none);
        assert!(out.ok, "{}", out.error);
        // A model that is not listed is refused, and unloading stops the engine.
        assert!(backend.load("../../etc/passwd").is_err());
        backend.unload();
        assert!(!backend.is_loaded());
    }

    #[test]
    #[ignore]
    fn real_engine_reads_a_picture() {
        let Some(backend) = real() else { return };
        let Some(vision) = backend.models().into_iter().find(|m| m.vision) else {
            return;
        };
        // 64x64 solid red.
        let image = ImageInput {
            width: 64,
            height: 64,
            rgb: [255u8, 0, 0].repeat(64 * 64),
        };
        let request = ChatRequest {
            model: vision.id,
            messages: json!([{"role": "user", "content": [{"type": "image", "index": 0}, {"type": "text", "text": "What single colour is this picture? One word."}]}]),
            images: vec![image],
            max_tokens: 16,
            ..ChatRequest::default()
        };
        let out = backend.chat(&request, &mut |_| {}, &|| false);
        assert!(out.ok, "{}", out.error);
        assert!(out.text.to_lowercase().contains("red"), "{}", out.text);
    }
}
