//! Scripted engines for tests and for exercising the service without a model.

use std::sync::Mutex;
use std::time::Duration;

use serde_json::{json, Value};

use crate::backend::*;
use crate::pictures;

pub struct FakeBackend {
    pub reply: String,
    pub chunk_chars: usize,
    pub per_chunk: Duration,
    state: Mutex<FakeState>,
    progress: Mutex<Option<ProgressFn>>,
}

#[derive(Default)]
struct FakeState {
    loaded: String,
}

impl FakeBackend {
    pub fn new(reply: &str, per_chunk: Duration) -> Self {
        Self {
            reply: reply.to_string(),
            chunk_chars: 3,
            per_chunk,
            state: Mutex::new(FakeState::default()),
            progress: Mutex::new(None),
        }
    }

    fn loaded(&self) -> String {
        self.state.lock().unwrap().loaded.clone()
    }
}

impl Backend for FakeBackend {
    fn models(&self) -> Vec<ModelInfo> {
        vec![ModelInfo {
            id: "fake-model".into(),
            bytes: 1,
            loaded: self.loaded() == "fake-model",
            vision: true,
        }]
    }

    fn load(&self, model: &str) -> Result<(), String> {
        if model != "fake-model" {
            return Err(format!("no such model: {model}"));
        }
        if let Some(progress) = self.progress.lock().unwrap().clone() {
            for fraction in [0.0, 0.5, 1.0] {
                progress(model, fraction);
            }
        }
        self.state.lock().unwrap().loaded = model.to_string();
        Ok(())
    }

    fn set_progress(&self, progress: ProgressFn) {
        *self.progress.lock().unwrap() = Some(progress);
    }

    fn unload(&self) {
        self.state.lock().unwrap().loaded.clear();
    }

    fn is_loaded(&self) -> bool {
        !self.loaded().is_empty()
    }

    fn chat(
        &self,
        request: &ChatRequest,
        on_delta: &mut dyn FnMut(&str),
        cancelled: &dyn Fn() -> bool,
    ) -> Outcome {
        let mut outcome = Outcome::default();
        let mut answer = self.reply.clone();
        if !request.images.is_empty() {
            // Lets tests see that pictures arrived decoded and in order.
            answer = format!("I can see {} image(s):", request.images.len());
            for picture in &request.images {
                answer.push_str(&format!(" {}x{}", picture.width, picture.height));
            }
            answer.push('.');
        }
        let chars: Vec<char> = answer.chars().collect();
        for piece in chars.chunks(self.chunk_chars) {
            if cancelled() {
                outcome.cancelled = true;
                outcome.error = "cancelled".into();
                return outcome;
            }
            if !self.per_chunk.is_zero() {
                std::thread::sleep(self.per_chunk);
            }
            let piece: String = piece.iter().collect();
            outcome.text.push_str(&piece);
            on_delta(&piece);
        }
        outcome.ok = true;
        outcome.completion_tokens = chars.len().div_ceil(self.chunk_chars) as u32;
        outcome
    }

    fn snapshot(&self) -> Value {
        json!({
            "backend": "fake",
            "loaded": self.loaded(),
            "contextTokens": 8192
        })
    }
}

pub struct FakeTranscriber;

impl Transcriber for FakeTranscriber {
    fn transcribe(
        &self,
        samples: &[i16],
        language: &str,
        _cancelled: &dyn Fn() -> bool,
    ) -> Outcome {
        Outcome {
            ok: true,
            language: if language == "auto" {
                "en".into()
            } else {
                language.into()
            },
            text: format!("heard {} samples", samples.len()),
            ..Outcome::default()
        }
    }
}

pub struct FakeImageGenerator;

impl ImageGenerator for FakeImageGenerator {
    fn generate(
        &self,
        request: &ChatRequest,
        progress: ImageProgress<'_>,
        cancelled: &dyn Fn() -> bool,
    ) -> Outcome {
        let mut outcome = Outcome::default();
        let delay = std::env::var("HELIX_FAKE_IMAGE_DELAY_MS")
            .ok()
            .and_then(|v| v.parse::<i64>().ok())
            .map(|v| v.clamp(0, 1000) as u64)
            .unwrap_or(100);
        for i in 0..=4 {
            if cancelled() {
                outcome.cancelled = true;
                outcome.error = "cancelled".into();
                return outcome;
            }
            progress(f64::from(i) / 4.0, "Generating image");
            std::thread::sleep(Duration::from_millis(delay));
        }
        let pixels = [80u8, 140, 200].repeat((request.image_width * request.image_height) as usize);
        outcome.ok = true;
        outcome.image_png =
            pictures::encode_png(&pixels, request.image_width, request.image_height);
        outcome.image_model = if request.model.is_empty() {
            "fake-image-model.gguf".into()
        } else {
            request.model.clone()
        };
        outcome.image_prompt = request.image_prompt.clone();
        outcome.image_width = request.image_width;
        outcome.image_height = request.image_height;
        outcome.image_seed = request.image_seed;
        outcome
    }
}
