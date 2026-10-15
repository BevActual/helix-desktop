//! Bounded-concurrency template hook runner (`theme/hook_runner.cpp`).

use std::collections::{BTreeMap, VecDeque};
use std::process::Command;
use std::sync::{Arc, Condvar, Mutex};
use std::thread;

pub const DEFAULT_MAX_CONCURRENT: usize = 4;

struct QueuedHook {
    command: String,
    template_id: String,
    generation: u64,
}

struct Inner {
    queue: VecDeque<QueuedHook>,
    running: usize,
    max_concurrent: usize,
    current_generation: u64,
    shutdown: bool,
    failures: BTreeMap<String, String>,
    on_failure: Option<std::sync::Arc<dyn Fn(String, String) + Send + Sync>>,
}

pub struct HookRunner {
    inner: Arc<(Mutex<Inner>, Condvar)>,
}

fn trim_line(text: &str) -> &str {
    text.trim_matches(|c| matches!(c, ' ' | '\t' | '\n' | '\r' | '\x0b' | '\x0c'))
}

fn compact_hook_error(stderr: &str, stdout: &str, code: Option<i32>) -> String {
    let text = if stderr.is_empty() { stdout } else { stderr };
    let text = trim_line(text.split('\n').next().unwrap_or(""));
    if text.is_empty() {
        return format!("exit {}", code.unwrap_or(-1));
    }
    text.chars().take(160).collect()
}

impl HookRunner {
    pub fn new(max_concurrent: usize) -> Self {
        let max = if max_concurrent > 0 { max_concurrent } else { DEFAULT_MAX_CONCURRENT };
        Self {
            inner: Arc::new((
                Mutex::new(Inner {
                    queue: VecDeque::new(),
                    running: 0,
                    max_concurrent: max,
                    current_generation: 0,
                    shutdown: false,
                    failures: BTreeMap::new(),
                    on_failure: None,
                }),
                Condvar::new(),
            )),
        }
    }

    pub fn set_on_failure<F>(&self, callback: F)
    where
        F: Fn(String, String) + Send + Sync + 'static,
    {
        self.inner.0.lock().unwrap().on_failure = Some(std::sync::Arc::new(callback));
    }

    pub fn enqueue(&self, command: String, generation: u64, template_id: String) {
        if command.is_empty() {
            return;
        }
        {
            let mut guard = self.inner.0.lock().unwrap();
            if guard.shutdown || generation < guard.current_generation {
                return;
            }
            guard.queue.push_back(QueuedHook { command, template_id, generation });
        }
        self.inner.1.notify_all();
        Self::pump(Arc::clone(&self.inner));
    }

    pub fn invalidate_before(&self, generation: u64) {
        let mut guard = self.inner.0.lock().unwrap();
        if generation <= guard.current_generation {
            return;
        }
        guard.current_generation = generation;
        guard.queue.retain(|hook| hook.generation >= generation);
        if guard.queue.is_empty() && guard.running == 0 {
            self.inner.1.notify_all();
        }
    }

    pub fn request_shutdown(&self) {
        let mut guard = self.inner.0.lock().unwrap();
        guard.shutdown = true;
        guard.queue.clear();
        self.inner.1.notify_all();
    }

    pub fn wait_idle(&self) {
        let mut guard = self.inner.0.lock().unwrap();
        while !guard.shutdown && (!guard.queue.is_empty() || guard.running > 0) {
            guard = self.inner.1.wait(guard).unwrap();
        }
    }

    pub fn pending_count(&self) -> usize {
        let guard = self.inner.0.lock().unwrap();
        guard.queue.len() + guard.running
    }

    pub(crate) fn take_failures(&self) -> BTreeMap<String, String> {
        std::mem::take(&mut self.inner.0.lock().unwrap().failures)
    }

    fn record_failure(inner: &Arc<(Mutex<Inner>, Condvar)>, generation: u64, template_id: String, error: String) {
        if template_id.is_empty() {
            return;
        }
        let mut guard = inner.0.lock().unwrap();
        if guard.shutdown || generation < guard.current_generation {
            return;
        }
        if let Some(handler) = guard.on_failure.clone() {
            handler(template_id, error);
        } else {
            guard.failures.insert(template_id, error);
        }
    }

    fn launch(inner: Arc<(Mutex<Inner>, Condvar)>, hook: QueuedHook) {
        let command = hook.command;
        let template_id = hook.template_id;
        let generation = hook.generation;
        thread::spawn(move || {
            let result = Command::new("/bin/sh").arg("-lc").arg(&command).output();
            match result {
                Ok(out) if out.status.success() => {}
                Ok(out) => {
                    let msg = compact_hook_error(
                        &String::from_utf8_lossy(&out.stderr),
                        &String::from_utf8_lossy(&out.stdout),
                        out.status.code(),
                    );
                    HookRunner::record_failure(&inner, generation, template_id, msg);
                }
                Err(_) => {
                    HookRunner::record_failure(&inner, generation, template_id, "failed to start".into());
                }
            }
            {
                let mut guard = inner.0.lock().unwrap();
                guard.running -= 1;
            }
            inner.1.notify_all();
            HookRunner::pump(inner);
        });
    }

    fn pump(inner: Arc<(Mutex<Inner>, Condvar)>) {
        loop {
            let hook = {
                let mut guard = inner.0.lock().unwrap();
                let mut picked = None;
                while !guard.shutdown && guard.running < guard.max_concurrent && !guard.queue.is_empty() {
                    let next = guard.queue.pop_front().unwrap();
                    if next.generation < guard.current_generation {
                        continue;
                    }
                    guard.running += 1;
                    picked = Some(next);
                    break;
                }
                if picked.is_none() && guard.queue.is_empty() && guard.running == 0 {
                    inner.1.notify_all();
                }
                picked
            };
            match hook {
                Some(hook) => Self::launch(inner.clone(), hook),
                None => return,
            }
        }
    }
}

impl Drop for HookRunner {
    fn drop(&mut self) {
        self.request_shutdown();
        let mut guard = self.inner.0.lock().unwrap();
        while guard.running > 0 {
            guard = self.inner.1.wait(guard).unwrap();
        }
    }
}
