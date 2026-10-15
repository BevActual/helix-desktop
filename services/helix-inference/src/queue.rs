//! One worker thread runs one request at a time (a model cannot be shared by concurrent generations), choosing interactive
//! work before background work and otherwise first come, first served. Callbacks run on the worker thread: the D-Bus layer
//! hands them to its own loop. Port of C++ `inference_queue.*`.

use std::collections::VecDeque;
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use crate::backend::*;
use crate::pictures;

#[derive(Clone, Copy, Debug)]
pub struct Limits {
    /// Waiting plus running, across all clients.
    pub max_pending: usize,
    pub max_per_client: usize,
    /// Total size of the messages and the schema.
    pub max_prompt_bytes: usize,
    /// A request asking for more is clamped.
    pub max_tokens_cap: u32,
}

impl Default for Limits {
    fn default() -> Self {
        Self { max_pending: 8, max_per_client: 3, max_prompt_bytes: 64 * 1024, max_tokens_cap: 1024 }
    }
}

pub type DeltaFn = Arc<dyn Fn(u64, &str) + Send + Sync>;
pub type DoneFn = Arc<dyn Fn(u64, &Outcome) + Send + Sync>;
pub type LoadingFn = Arc<dyn Fn(u64, &str, f64) + Send + Sync>;
pub type ProgressFn = Arc<dyn Fn(u64, f64, &str) + Send + Sync>;

struct Entry {
    operation: u64,
    request: ChatRequest,
}

struct State {
    waiting: VecDeque<Entry>,
    next: u64,
    running_operation: u64,
    running_client: String,
    running_cancelled: bool,
    stopping: bool,
    last_activity: Instant,
}

struct Shared {
    backend: Arc<dyn Backend>,
    limits: Limits,
    on_delta: DeltaFn,
    on_done: DoneFn,
    on_loading: Mutex<Option<LoadingFn>>,
    on_progress: Mutex<Option<ProgressFn>>,
    transcriber: Mutex<Option<Arc<dyn Transcriber>>>,
    image_generator: Mutex<Option<Arc<dyn ImageGenerator>>>,
    state: Mutex<State>,
    wake: Condvar,
}

pub struct InferenceQueue {
    shared: Arc<Shared>,
    worker: Option<JoinHandle<()>>,
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

impl InferenceQueue {
    pub fn new(backend: Arc<dyn Backend>, limits: Limits, on_delta: DeltaFn, on_done: DoneFn) -> Self {
        let shared = Arc::new(Shared {
            backend,
            limits,
            on_delta,
            on_done,
            on_loading: Mutex::new(None),
            on_progress: Mutex::new(None),
            transcriber: Mutex::new(None),
            image_generator: Mutex::new(None),
            state: Mutex::new(State {
                waiting: VecDeque::new(),
                next: 0,
                running_operation: 0,
                running_client: String::new(),
                running_cancelled: false,
                stopping: false,
                last_activity: Instant::now(),
            }),
            wake: Condvar::new(),
        });
        let weak = Arc::downgrade(&shared);
        shared.backend.set_progress(Arc::new(move |model: &str, fraction: f64| {
            let Some(shared) = weak.upgrade() else { return };
            let operation = lock(&shared.state).running_operation;
            let callback = lock(&shared.on_loading).clone();
            if let (true, Some(callback)) = (operation != 0, callback) {
                callback(operation, model, fraction);
            }
        }));
        let worker_shared = Arc::clone(&shared);
        let worker = std::thread::Builder::new()
            .name("inference-worker".into())
            .spawn(move || run(&worker_shared))
            .expect("spawn inference worker");
        Self { shared, worker: Some(worker) }
    }

    pub fn on_loading(&self, callback: LoadingFn) {
        *lock(&self.shared.on_loading) = Some(callback);
    }

    pub fn on_progress(&self, callback: ProgressFn) {
        *lock(&self.shared.on_progress) = Some(callback);
    }

    pub fn set_transcriber(&self, transcriber: Arc<dyn Transcriber>) {
        *lock(&self.shared.transcriber) = Some(transcriber);
    }

    pub fn set_image_generator(&self, generator: Arc<dyn ImageGenerator>) {
        *lock(&self.shared.image_generator) = Some(generator);
    }

    /// Refused immediately (nothing queued, no callback) when a limit is exceeded or the request is malformed.
    pub fn submit(&self, mut request: ChatRequest) -> Result<u64, String> {
        let limits = self.shared.limits;
        if request.generate_image {
            if lock(&self.shared.image_generator).is_none() {
                return Err("image generation is not available".into());
            }
            pictures::validate_image_request(&request)?;
            request.messages = json!([]);
        } else if request.transcribe {
            if lock(&self.shared.transcriber).is_none() {
                return Err("speech to text is not available".into());
            }
            if request.audio.is_empty() || request.audio.len() > MAX_AUDIO_SAMPLES {
                return Err("the recording is empty or too long".into());
            }
            request.messages = json!([]);
        } else if !request.messages.as_array().is_some_and(|m| !m.is_empty()) {
            return Err("messages must be a non-empty array".into());
        }
        for message in request.messages.as_array().into_iter().flatten() {
            if !well_formed(message) {
                return Err("each message needs a string role and a string content (or a list of text and image parts)".into());
            }
        }
        if request.messages.to_string().len() + request.json_schema.len() > limits.max_prompt_bytes {
            return Err("the request is too large".into());
        }
        request.max_tokens = request.max_tokens.clamp(1, limits.max_tokens_cap);
        request.temperature = request.temperature.clamp(0.0, 2.0);

        let mut state = lock(&self.shared.state);
        if state.stopping {
            return Err("the inference service is shutting down".into());
        }
        let running = usize::from(state.running_operation != 0);
        if state.waiting.len() + running >= limits.max_pending {
            return Err("the inference queue is full".into());
        }
        let from_client = state.waiting.iter().filter(|e| e.request.client == request.client).count()
            + usize::from(state.running_operation != 0 && state.running_client == request.client);
        if from_client >= limits.max_per_client {
            return Err("too many requests from this client".into());
        }
        state.next += 1;
        let operation = state.next;
        state.last_activity = Instant::now();
        // Interactive requests go ahead of waiting background ones, behind earlier interactive ones.
        let interactive = request.priority == Priority::Interactive;
        let entry = Entry { operation, request };
        if interactive {
            let at = state
                .waiting
                .iter()
                .position(|e| e.request.priority == Priority::Background)
                .unwrap_or(state.waiting.len());
            state.waiting.insert(at, entry);
        } else {
            state.waiting.push_back(entry);
        }
        drop(state);
        self.shared.wake.notify_one();
        Ok(operation)
    }

    /// Only the requester may cancel. A waiting request completes at once as cancelled; a running one stops at the next
    /// token. Returns false for an unknown operation or someone else's.
    pub fn cancel(&self, client: &str, operation: u64) -> bool {
        {
            let mut state = lock(&self.shared.state);
            if state.running_operation == operation && operation != 0 {
                if state.running_client != client {
                    return false;
                }
                state.running_cancelled = true; // the worker reports it once the backend stops
                return true;
            }
            let Some(at) = state.waiting.iter().position(|e| e.operation == operation) else {
                return false;
            };
            if state.waiting[at].request.client != client {
                return false;
            }
            state.waiting.remove(at);
        }
        let outcome = Outcome { cancelled: true, error: "cancelled".into(), ..Outcome::default() };
        (self.shared.on_done)(operation, &outcome);
        true
    }

    /// Frees the model when nothing has run, or been asked to run, for `idle`. Never while a request is running or waiting.
    pub fn unload_if_idle(&self, idle: Duration) -> bool {
        // Held throughout: the worker cannot start a request while the model is being freed.
        let state = lock(&self.shared.state);
        if state.stopping
            || state.running_operation != 0
            || !state.waiting.is_empty()
            || !self.shared.backend.is_loaded()
            || state.last_activity.elapsed() < idle
        {
            return false;
        }
        self.shared.backend.unload();
        true
    }

    /// Queue depth and the backend's own state.
    pub fn snapshot(&self) -> Value {
        let mut out = self.shared.backend.snapshot();
        let state = lock(&self.shared.state);
        out["queue"] = json!({
            "waiting": state.waiting.len(),
            "running": state.running_operation != 0,
            "max": self.shared.limits.max_pending,
        });
        out
    }

    pub fn backend(&self) -> &Arc<dyn Backend> {
        &self.shared.backend
    }
}

impl Drop for InferenceQueue {
    fn drop(&mut self) {
        let abandoned: Vec<Entry> = {
            let mut state = lock(&self.shared.state);
            state.stopping = true;
            state.running_cancelled = true;
            state.waiting.drain(..).collect()
        };
        self.shared.wake.notify_all();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
        for entry in abandoned {
            let outcome = Outcome {
                error: "the inference service is shutting down".into(),
                ..Outcome::default()
            };
            (self.shared.on_done)(entry.operation, &outcome);
        }
    }
}

fn well_formed(message: &Value) -> bool {
    let Some(object) = message.as_object() else { return false };
    if !object.get("role").is_some_and(Value::is_string) {
        return false;
    }
    match object.get("content") {
        Some(Value::String(_)) => true,
        Some(Value::Array(parts)) => parts.iter().all(|part| {
            let Some(part) = part.as_object() else { return false };
            let text = part.get("type").and_then(Value::as_str) == Some("text")
                && part.get("text").is_some_and(Value::is_string);
            let image = part.get("type").and_then(Value::as_str) == Some("image")
                && part.get("index").is_some_and(Value::is_u64);
            text || image
        }),
        _ => false,
    }
}

fn run(shared: &Arc<Shared>) {
    loop {
        let entry = {
            let mut state = lock(&shared.state);
            loop {
                if state.stopping {
                    return;
                }
                if let Some(entry) = state.waiting.pop_front() {
                    state.running_operation = entry.operation;
                    state.running_client = entry.request.client.clone();
                    state.running_cancelled = false;
                    break entry;
                }
                state = shared.wake.wait(state).unwrap_or_else(|p| p.into_inner());
            }
        };
        let operation = entry.operation;
        let cancelled = || lock(&shared.state).running_cancelled;
        let mut request = entry.request;

        let mut outcome;
        if request.generate_image {
            if let Some(callback) = lock(&shared.on_progress).clone() {
                callback(operation, 0.0, "Releasing chat model memory");
            }
            shared.backend.unload(); // shared queue: no chat call can race with this GPU handoff
            let generator = lock(&shared.image_generator).clone();
            let progress_callback = lock(&shared.on_progress).clone();
            let progress = move |fraction: f64, stage: &str| {
                if let Some(callback) = &progress_callback {
                    callback(operation, fraction, stage);
                }
            };
            outcome = match generator {
                Some(generator) => generator.generate(&request, &progress, &cancelled),
                None => Outcome { error: "image generation is not available".into(), ..Outcome::default() },
            };
        } else if request.transcribe {
            let transcriber = lock(&shared.transcriber).clone();
            let samples = std::mem::take(&mut request.audio);
            outcome = match transcriber {
                Some(transcriber) => transcriber.transcribe(&samples, &request.language, &cancelled),
                None => Outcome { error: "speech to text is not available".into(), ..Outcome::default() },
            };
        } else {
            let loaded = if request.model.is_empty() {
                Ok(())
            } else {
                shared.backend.load(&request.model)
            };
            outcome = match loaded {
                Err(error) => Outcome {
                    error: if error.is_empty() { "the model could not be loaded".into() } else { error },
                    ..Outcome::default()
                },
                Ok(()) => {
                    let delta = Arc::clone(&shared.on_delta);
                    shared.backend.chat(&request, &mut |piece: &str| delta(operation, piece), &cancelled)
                }
            };
        }

        let was_cancelled = {
            let mut state = lock(&shared.state);
            let was = state.running_cancelled;
            state.running_operation = 0;
            state.running_client.clear();
            state.running_cancelled = false;
            state.last_activity = Instant::now();
            was
        };
        if was_cancelled && !outcome.ok {
            outcome.cancelled = true;
            if outcome.error.is_empty() {
                outcome.error = "cancelled".into();
            }
        }
        (shared.on_done)(operation, &outcome);
    }
}
