//! Speech-to-text and image generation run as separate, hardened worker programs (`helix-transcribe`, `helix-image`): their
//! engines bundle their own ggml, which cannot share a process with llama.cpp's, and a crash while decoding untrusted input
//! must not take the chat service down. Port of the process wrappers in C++ `transcriber.cpp` / `image_generator.cpp`.

use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::mpsc;
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use crate::backend::*;
use crate::pictures;

/// A worker child, killed and reaped when dropped.
struct Worker {
    child: Child,
}

impl Drop for Worker {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn spawn(program: &Path, args: &[&str]) -> std::io::Result<Worker> {
    let child = Command::new(program)
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()?;
    Ok(Worker { child })
}

/// Feeds `input` to the worker's stdin on a thread, so a worker that never reads cannot block the poll loop.
fn write_input(worker: &mut Worker, input: Vec<u8>) {
    if let Some(mut stdin) = worker.child.stdin.take() {
        std::thread::spawn(move || {
            let _ = stdin.write_all(&input);
        });
    }
}

/// Chunks of the worker's stdout, then a final empty chunk at end of file.
fn read_output(worker: &mut Worker) -> mpsc::Receiver<Vec<u8>> {
    let (sender, receiver) = mpsc::channel();
    if let Some(mut stdout) = worker.child.stdout.take() {
        std::thread::spawn(move || {
            let mut buffer = [0u8; 65536];
            loop {
                match stdout.read(&mut buffer) {
                    Ok(0) | Err(_) => break,
                    Ok(n) => {
                        if sender.send(buffer[..n].to_vec()).is_err() {
                            return;
                        }
                    }
                }
            }
            let _ = sender.send(Vec::new());
        });
    }
    receiver
}

pub struct ProcessTranscriber {
    worker: PathBuf,
    speech_dir: PathBuf,
}

impl ProcessTranscriber {
    pub fn new(worker: PathBuf, speech_dir: PathBuf) -> Self {
        Self { worker, speech_dir }
    }

    /// `HELIX_SPEECH_MODEL` (a file name, never a path), else the first `ggml-*.bin` by name.
    fn pick_model(&self) -> Option<PathBuf> {
        if let Ok(named) = std::env::var("HELIX_SPEECH_MODEL") {
            if let Some(name) = Path::new(&named).file_name() {
                let candidate = self.speech_dir.join(name);
                if candidate.is_file() {
                    return Some(candidate);
                }
            }
        }
        let mut found: Vec<PathBuf> = std::fs::read_dir(&self.speech_dir)
            .ok()?
            .flatten()
            .map(|e| e.path())
            .filter(|p| {
                p.is_file()
                    && p.extension().is_some_and(|e| e == "bin")
                    && p.file_name()
                        .is_some_and(|n| n.to_string_lossy().starts_with("ggml-"))
            })
            .collect();
        found.sort();
        found.into_iter().next()
    }
}

impl Transcriber for ProcessTranscriber {
    fn transcribe(&self, samples: &[i16], language: &str, cancelled: &dyn Fn() -> bool) -> Outcome {
        let mut outcome = Outcome::default();
        let Some(model) = self.pick_model() else {
            outcome.error = "no speech model is installed; get one from the Models library".into();
            return outcome;
        };
        if !self.worker.is_file() {
            outcome.error = "the speech worker is not installed".into();
            return outcome;
        }
        if samples.is_empty() || samples.len() > MAX_AUDIO_SAMPLES {
            outcome.error = "the recording is empty or too long".into();
            return outcome;
        }
        let model_arg = model.to_string_lossy().into_owned();
        let Ok(mut worker) = spawn(
            &self.worker,
            &["--model", &model_arg, "--language", language],
        ) else {
            outcome.error = "could not start the speech worker".into();
            return outcome;
        };
        // Input: a little-endian sample count, then the samples (the worker checks the count).
        let mut input = Vec::with_capacity(4 + samples.len() * 2);
        input.extend_from_slice(&(samples.len() as u32).to_le_bytes());
        for sample in samples {
            input.extend_from_slice(&sample.to_le_bytes());
        }
        write_input(&mut worker, input);
        let output = read_output(&mut worker);

        let seconds = samples.len() as f64 / SAMPLE_RATE as f64;
        let deadline = Instant::now() + Duration::from_secs((60.0 + seconds * 4.0) as u64);
        let mut reply = Vec::new();
        loop {
            if cancelled() {
                let _ = worker.child.kill();
                outcome.cancelled = true;
                outcome.error = "cancelled".into();
                break;
            }
            if Instant::now() > deadline {
                let _ = worker.child.kill();
                outcome.error = "transcription took too long".into();
                break;
            }
            match output.recv_timeout(Duration::from_millis(100)) {
                Ok(chunk) if chunk.is_empty() => break, // the worker closed its output: done (or gone)
                Ok(chunk) => {
                    reply.extend_from_slice(&chunk);
                    if reply.len() > (1 << 20) {
                        let _ = worker.child.kill();
                        outcome.error = "the speech worker sent too much".into();
                        break;
                    }
                }
                Err(mpsc::RecvTimeoutError::Timeout) => {}
                Err(mpsc::RecvTimeoutError::Disconnected) => break,
            }
        }
        let status = worker.child.wait().ok();
        if !outcome.error.is_empty() {
            return outcome;
        }
        match serde_json::from_slice::<Value>(&reply) {
            Ok(value) if value.get("ok").and_then(Value::as_bool) == Some(true) => {
                outcome.ok = true;
                outcome.text = value
                    .get("text")
                    .and_then(Value::as_str)
                    .unwrap_or("")
                    .into();
                outcome.language = value
                    .get("language")
                    .and_then(Value::as_str)
                    .unwrap_or("")
                    .into();
            }
            Ok(value) => {
                outcome.error = value
                    .get("error")
                    .and_then(Value::as_str)
                    .unwrap_or("transcription failed")
                    .into();
            }
            Err(_) => {
                use std::os::unix::process::ExitStatusExt;
                let crashed = status.and_then(|s| s.signal()).is_some();
                outcome.error = if crashed {
                    "the speech worker crashed".into()
                } else {
                    "the speech worker gave no answer".into()
                };
            }
        }
        outcome
    }
}

pub struct ProcessImageGenerator {
    worker: PathBuf,
    models: PathBuf,
}

impl ProcessImageGenerator {
    pub fn new(worker: PathBuf, models: PathBuf) -> Self {
        Self { worker, models }
    }
}

impl ImageGenerator for ProcessImageGenerator {
    fn generate(
        &self,
        request: &ChatRequest,
        progress: ImageProgress<'_>,
        cancelled: &dyn Fn() -> bool,
    ) -> Outcome {
        let mut outcome = Outcome::default();
        if let Err(error) = pictures::validate_image_request(request) {
            outcome.error = error;
            return outcome;
        }
        let model = if request.model.is_empty() {
            let mut files: Vec<PathBuf> = std::fs::read_dir(&self.models)
                .map(|entries| {
                    entries
                        .flatten()
                        .filter(|e| e.file_type().is_ok_and(|t| t.is_file()))
                        .map(|e| e.path())
                        .filter(|p| p.extension().is_some_and(|e| e == "gguf"))
                        .collect()
                })
                .unwrap_or_default();
            files.sort();
            files.into_iter().next()
        } else {
            Some(self.models.join(&request.model))
        };
        let usable = model.as_ref().is_some_and(|m| {
            std::fs::symlink_metadata(m).is_ok_and(|meta| meta.file_type().is_file())
        });
        let Some(model) = model.filter(|_| usable) else {
            outcome.error =
                "no image model is installed; download SD-Turbo from the Models library".into();
            return outcome;
        };
        if !self.worker.is_file() {
            outcome.error = "the image worker is not installed".into();
            return outcome;
        }
        let model_arg = model.to_string_lossy().into_owned();
        let Ok(mut worker) = spawn(&self.worker, &["--model", &model_arg]) else {
            outcome.error = "could not start the image worker".into();
            return outcome;
        };
        let input = format!(
            "{}\n",
            json!({
                "prompt": request.image_prompt,
                "width": request.image_width,
                "height": request.image_height,
                "steps": request.image_steps,
                "seed": request.image_seed,
            })
        );
        write_input(&mut worker, input.into_bytes());
        let output = read_output(&mut worker);

        let deadline = Instant::now() + Duration::from_secs(600);
        let (mut received, mut frames) = (0usize, 0usize);
        let mut buffered: Vec<u8> = Vec::new();
        while Instant::now() < deadline {
            if cancelled() {
                outcome.cancelled = true;
                outcome.error = "cancelled".into();
                return outcome;
            }
            match output.recv_timeout(Duration::from_millis(100)) {
                Ok(chunk) if chunk.is_empty() => break,
                Ok(chunk) => {
                    received += chunk.len();
                    if received > pictures::MAX_GENERATED_PNG * 2 {
                        outcome.error = "image worker output exceeded its limit".into();
                        return outcome;
                    }
                    buffered.extend_from_slice(&chunk);
                }
                Err(mpsc::RecvTimeoutError::Timeout) => continue,
                Err(mpsc::RecvTimeoutError::Disconnected) => break,
            }
            while let Some(newline) = buffered.iter().position(|&b| b == b'\n') {
                frames += 1;
                if frames > 100 {
                    outcome.error = "image worker sent too many events".into();
                    return outcome;
                }
                let line: Vec<u8> = buffered.drain(..=newline).collect();
                let Ok(frame) = serde_json::from_slice::<Value>(&line[..line.len() - 1]) else {
                    outcome.error = "image worker returned invalid output".into();
                    return outcome;
                };
                if !frame.is_object() {
                    outcome.error = "image worker returned invalid output".into();
                    return outcome;
                }
                if let Some(fraction) = frame.get("progress") {
                    let stage = frame.get("stage").and_then(Value::as_str);
                    let (Some(fraction), Some(stage)) = (fraction.as_f64(), stage) else {
                        outcome.error = "invalid image progress".into();
                        return outcome;
                    };
                    let stage: String = stage.chars().take(160).collect();
                    progress(fraction.clamp(0.0, 1.0), &stage);
                    continue;
                }
                if frame.get("ok").and_then(Value::as_bool) != Some(true) {
                    outcome.error = frame
                        .get("error")
                        .and_then(Value::as_str)
                        .unwrap_or("image generation failed")
                        .into();
                    return outcome;
                }
                let encoded = frame.get("png").and_then(Value::as_str).unwrap_or("");
                if encoded.len() > pictures::MAX_GENERATED_PNG * 4 / 3 + 4 {
                    outcome.error = "generated image is too large".into();
                    return outcome;
                }
                let decoded = pictures::decode_base64(encoded)
                    .and_then(|bytes| pictures::decode_image_input(&bytes).ok());
                let Some(decoded) = decoded
                    .filter(|d| d.width == request.image_width && d.height == request.image_height)
                else {
                    outcome.error = "image worker returned an invalid PNG or dimensions".into();
                    return outcome;
                };
                outcome.ok = true;
                outcome.image_png = encoded.to_string();
                outcome.image_model = model
                    .file_name()
                    .map(|n| n.to_string_lossy().into_owned())
                    .unwrap_or_default();
                outcome.image_prompt = request.image_prompt.clone();
                outcome.image_width = decoded.width;
                outcome.image_height = decoded.height;
                outcome.image_seed = request.image_seed;
                return outcome;
            }
        }
        outcome.error = "image worker stopped or exceeded its ten-minute deadline".into();
        outcome
    }
}
