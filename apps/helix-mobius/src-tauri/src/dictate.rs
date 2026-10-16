//! Dictation: record from the microphone while the person holds a recording open, then hand the samples to the inference service.
//!
//! The microphone is only open between `start` and `stop` (or `cancel`); nothing is recorded in the background, nothing is kept on
//! disk, and the audio goes nowhere but the local inference service. Recording uses PipeWire's `pw-record` in raw 16 kHz mono mode.

use std::collections::HashMap;
use std::io::Read;
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};

/// The service takes ten minutes at most.
pub const MAX_BYTES: usize = 16_000 * 2 * 600;

pub struct Recording {
    child: Child,
    buffer: Arc<Mutex<Vec<u8>>>,
    reader: std::thread::JoinHandle<()>,
}

#[derive(Default)]
pub struct Dictation {
    current: Mutex<Option<Recording>>,
}

impl Dictation {
    #[cfg(test)]
    pub fn is_recording(&self) -> bool {
        self.current.lock().expect("dictation").is_some()
    }

    /// Opens the microphone. `level` is called about ten times a second with the loudest sample of that stretch, 0..1.
    pub fn start(&self, level: impl Fn(f32) + Send + 'static) -> Result<(), String> {
        let mut current = self.current.lock().expect("dictation");
        if current.is_some() {
            return Err("Already listening.".into());
        }
        let mut command = if let Some(file) =
            std::env::var_os("HELIX_DICTATE_SOURCE_FILE").filter(|v| !v.is_empty())
        {
            // Tests: play a recorded file as if it were the microphone.
            let mut c = Command::new("sh");
            c.arg("-c")
                .arg("cat \"$1\"; exec sleep 3600")
                .arg("dictate")
                .arg(file);
            c
        } else {
            let mut c = Command::new("pw-record");
            c.args([
                "--raw",
                "--rate",
                "16000",
                "--channels",
                "1",
                "--format",
                "s16",
                "-",
            ]);
            c
        };
        let mut child = command
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .map_err(|_| {
                "No microphone recorder is available (pw-record from PipeWire).".to_string()
            })?;
        let mut stdout = child.stdout.take().ok_or("No microphone.")?;
        let buffer = Arc::new(Mutex::new(Vec::<u8>::new()));
        let sink = buffer.clone();
        let reader = std::thread::spawn(move || {
            let mut chunk = [0u8; 3200]; // 0.1 s
            loop {
                match stdout.read(&mut chunk) {
                    Ok(0) | Err(_) => break,
                    Ok(n) => {
                        let mut peak = 0i32;
                        for pair in chunk[..n].as_chunks::<2>().0 {
                            peak =
                                peak.max(i32::from(i16::from_le_bytes([pair[0], pair[1]])).abs());
                        }
                        level((peak as f32 / 32768.0).min(1.0));
                        let mut data = sink.lock().expect("buffer");
                        if data.len() + n > MAX_BYTES {
                            break; // ten minutes: stop taking more
                        }
                        data.extend_from_slice(&chunk[..n]);
                    }
                }
            }
        });
        *current = Some(Recording {
            child,
            buffer,
            reader,
        });
        Ok(())
    }

    /// Closes the microphone and returns what was heard (16-bit little-endian, 16 kHz, mono).
    pub fn stop(&self) -> Result<Vec<u8>, String> {
        let recording = self
            .current
            .lock()
            .expect("dictation")
            .take()
            .ok_or("Not listening.")?;
        let Recording {
            mut child,
            buffer,
            reader,
        } = recording;
        let _ = child.kill();
        let _ = child.wait();
        let _ = reader.join();
        let data = std::mem::take(&mut *buffer.lock().expect("buffer"));
        Ok(data)
    }

    pub fn cancel(&self) {
        let _ = self.stop();
    }
}

/// Results of transcriptions, matched to the operation that asked. A result can arrive before the caller starts waiting.
#[derive(Default)]
pub struct Hub {
    inner: Mutex<HubInner>,
}

#[derive(Default)]
struct HubInner {
    waiting: HashMap<u64, tokio::sync::oneshot::Sender<(bool, String)>>,
    early: HashMap<u64, (bool, String)>,
    known: std::collections::HashSet<u64>,
    /// Completions of operations nobody had claimed yet (the reply to the request can be handled after its signal).
    unclaimed: HashMap<u64, (bool, String)>,
}

impl Hub {
    /// Remember that `operation` is a transcription, so its result is not mistaken for a chat reply.
    pub fn expect(&self, operation: u64) {
        let mut inner = self.inner.lock().expect("hub");
        inner.known.insert(operation);
        if let Some(done) = inner.unclaimed.remove(&operation) {
            inner.early.insert(operation, done);
        }
    }

    /// Called for every completed operation. Returns true if it was a transcription (and so consumed).
    pub fn deliver(&self, operation: u64, ok: bool, result: String) -> bool {
        let mut inner = self.inner.lock().expect("hub");
        if !inner.known.contains(&operation) {
            if inner.unclaimed.len() >= 16 {
                inner.unclaimed.clear();
            }
            inner.unclaimed.insert(operation, (ok, result));
            return false;
        }
        match inner.waiting.remove(&operation) {
            Some(sender) => {
                let _ = sender.send((ok, result));
            }
            None => {
                inner.early.insert(operation, (ok, result));
            }
        }
        true
    }

    pub async fn wait(&self, operation: u64) -> Result<(bool, String), String> {
        let receiver = {
            let mut inner = self.inner.lock().expect("hub");
            if let Some(done) = inner.early.remove(&operation) {
                inner.known.remove(&operation);
                return Ok(done);
            }
            let (sender, receiver) = tokio::sync::oneshot::channel();
            inner.waiting.insert(operation, sender);
            receiver
        };
        let outcome = tokio::time::timeout(std::time::Duration::from_secs(900), receiver).await;
        self.inner.lock().expect("hub").known.remove(&operation);
        match outcome {
            Ok(Ok(done)) => Ok(done),
            _ => Err("The transcription did not finish.".into()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn a_result_is_matched_whenever_it_arrives() {
        let hub = Hub::default();
        hub.expect(7);
        assert!(hub.deliver(7, true, "early".into()));
        assert_eq!(hub.wait(7).await.unwrap(), (true, "early".to_string()));
        hub.expect(8);
        let hub = Arc::new(hub);
        let waiter = {
            let hub = hub.clone();
            tokio::spawn(async move { hub.wait(8).await })
        };
        tokio::task::yield_now().await;
        assert!(hub.deliver(8, false, "late".into()));
        assert_eq!(waiter.await.unwrap().unwrap(), (false, "late".to_string()));
        assert!(
            !hub.deliver(9, true, "chat".into()),
            "a chat reply is not a transcription"
        );
        // The signal can be seen before the caller learns the operation id.
        assert!(!hub.deliver(10, true, "first".into()));
        hub.expect(10);
        assert_eq!(hub.wait(10).await.unwrap(), (true, "first".to_string()));
    }

    #[test]
    fn a_recording_comes_from_the_source_and_stops() {
        let path = std::env::temp_dir().join(format!("helix-dictate-{}.raw", std::process::id()));
        std::fs::write(&path, vec![1u8; 6400]).unwrap();
        std::env::set_var("HELIX_DICTATE_SOURCE_FILE", &path);
        let dictation = Dictation::default();
        dictation.start(|_| {}).unwrap();
        assert!(dictation.is_recording());
        assert!(dictation.start(|_| {}).is_err());
        std::thread::sleep(std::time::Duration::from_millis(300));
        let data = dictation.stop().unwrap();
        assert_eq!(data.len(), 6400);
        assert!(!dictation.is_recording());
        assert!(dictation.stop().is_err());
        std::env::remove_var("HELIX_DICTATE_SOURCE_FILE");
        let _ = std::fs::remove_file(&path);
    }
}
