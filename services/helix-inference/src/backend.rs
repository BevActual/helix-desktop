//! The seam between the inference service and an engine. The service owns scheduling, limits and D-Bus; a backend only turns
//! one request into text. Backends run on the service's single worker thread and are never called concurrently.

use serde_json::Value;
use std::sync::Arc;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Priority {
    Interactive,
    Background,
}

/// One decoded picture handed to a vision model: 8-bit RGB, row after row.
#[derive(Clone, Debug, Default)]
pub struct ImageInput {
    pub width: u32,
    pub height: u32,
    pub rgb: Vec<u8>,
}

#[derive(Clone, Debug)]
pub struct ChatRequest {
    /// Verified identity of the caller, for limits and logging.
    pub client: String,
    /// Empty = the backend's default.
    pub model: String,
    /// `[{role, content}, ...]`; content is a string or a list of `{"type":"text","text":..}` / `{"type":"image","index":N}`.
    pub messages: Value,
    pub images: Vec<ImageInput>,
    pub generate_image: bool,
    pub image_prompt: String,
    pub image_width: u32,
    pub image_height: u32,
    pub image_steps: u32,
    pub image_seed: u64,
    pub transcribe: bool,
    /// 16 kHz mono samples.
    pub audio: Vec<i16>,
    pub language: String,
    /// When set the output must conform to it (structured output); empty = free text.
    pub json_schema: String,
    pub reason_first: bool,
    pub max_tokens: u32,
    pub temperature: f64,
    pub priority: Priority,
}

impl Default for ChatRequest {
    fn default() -> Self {
        Self {
            client: String::new(),
            model: String::new(),
            messages: Value::Null,
            images: Vec::new(),
            generate_image: false,
            image_prompt: String::new(),
            image_width: 512,
            image_height: 512,
            image_steps: 4,
            image_seed: 0,
            transcribe: false,
            audio: Vec::new(),
            language: "auto".into(),
            json_schema: String::new(),
            reason_first: false,
            max_tokens: 256,
            temperature: 0.0,
            priority: Priority::Interactive,
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct Outcome {
    pub ok: bool,
    /// Everything produced, including after a cancellation.
    pub text: String,
    pub error: String,
    pub cancelled: bool,
    /// Stopped by the token limit before the model finished.
    pub truncated: bool,
    pub prompt_tokens: u32,
    /// Of those, how many were already in the model's memory from the previous request.
    pub cached_prompt_tokens: u32,
    pub completion_tokens: u32,
    /// Transcription: the language that was spoken.
    pub language: String,
    /// Bounded base64 PNG; no worker-chosen file path crosses the bus.
    pub image_png: String,
    pub image_model: String,
    pub image_prompt: String,
    pub image_width: u32,
    pub image_height: u32,
    pub image_seed: u64,
}

#[derive(Clone, Debug)]
pub struct ModelInfo {
    /// File name without directory.
    pub id: String,
    pub bytes: u64,
    pub loaded: bool,
    /// A projector for this model is installed: it can read images.
    pub vision: bool,
}

pub type ProgressFn = Arc<dyn Fn(&str, f64) + Send + Sync>;

pub trait Backend: Send + Sync {
    fn models(&self) -> Vec<ModelInfo>;
    /// Called before a request that names a model that is not the current one.
    fn load(&self, model: &str) -> Result<(), String>;
    fn unload(&self);
    fn is_loaded(&self) -> bool;
    /// Produces the reply, calling `on_delta` with each new piece. Must return promptly once `cancelled()` is true.
    fn chat(
        &self,
        request: &ChatRequest,
        on_delta: &mut dyn FnMut(&str),
        cancelled: &dyn Fn() -> bool,
    ) -> Outcome;
    fn snapshot(&self) -> Value;
    /// Told how far loading a model has got (0..1) while `load` runs, on the worker thread.
    fn set_progress(&self, _progress: ProgressFn) {}
}

pub trait Transcriber: Send + Sync {
    /// `language` is a two-letter code or "auto". Must return promptly once `cancelled()` is true.
    fn transcribe(&self, samples: &[i16], language: &str, cancelled: &dyn Fn() -> bool) -> Outcome;
}

pub type ImageProgress<'a> = &'a (dyn Fn(f64, &str) + Send + Sync);

pub trait ImageGenerator: Send + Sync {
    fn generate(
        &self,
        request: &ChatRequest,
        progress: ImageProgress<'_>,
        cancelled: &dyn Fn() -> bool,
    ) -> Outcome;
}

pub const SAMPLE_RATE: usize = 16000;
pub const MAX_AUDIO_SECONDS: usize = 600;
pub const MAX_AUDIO_SAMPLES: usize = SAMPLE_RATE * MAX_AUDIO_SECONDS;
pub const MAX_AUDIO_BYTES: usize = MAX_AUDIO_SAMPLES * 2;
