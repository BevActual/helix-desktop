//! The llama.cpp engine, through the C shim in `native/llama_shim.cpp`. Only built when the engine is installed.

use std::ffi::{c_char, c_double, c_int, c_void, CStr, CString};
use std::path::Path;
use std::sync::Mutex;

use serde_json::{json, Value};

use crate::backend::*;

type DeltaFn = extern "C" fn(*mut c_void, *const c_char);
type CancelFn = extern "C" fn(*mut c_void) -> c_int;
type ProgressCFn = extern "C" fn(*mut c_void, *const c_char, c_double);

extern "C" {
    fn hx_free(text: *mut c_char);
    fn hx_llama_create(model_dir: *const c_char) -> *mut c_void;
    fn hx_llama_destroy(handle: *mut c_void);
    fn hx_llama_models(handle: *mut c_void) -> *mut c_char;
    fn hx_llama_load(handle: *mut c_void, model: *const c_char, error: *mut *mut c_char) -> c_int;
    fn hx_llama_unload(handle: *mut c_void);
    fn hx_llama_is_loaded(handle: *mut c_void) -> c_int;
    fn hx_llama_snapshot(handle: *mut c_void) -> *mut c_char;
    fn hx_llama_set_progress(handle: *mut c_void, callback: Option<ProgressCFn>, user: *mut c_void);
    fn hx_llama_chat(
        handle: *mut c_void,
        request: *const c_char,
        pictures: *const *const u8,
        widths: *const u32,
        heights: *const u32,
        picture_count: usize,
        on_delta: DeltaFn,
        cancelled: CancelFn,
        user: *mut c_void,
    ) -> *mut c_char;
}

/// Takes ownership of a string the shim allocated.
///
/// # Safety
/// `text` is null, or a NUL-terminated string the shim allocated that nothing else frees.
unsafe fn take(text: *mut c_char) -> String {
    if text.is_null() {
        return String::new();
    }
    // SAFETY: not null, and by the contract above a NUL-terminated string owned by the caller until `hx_free`.
    let owned = unsafe { CStr::from_ptr(text) }
        .to_string_lossy()
        .into_owned();
    // SAFETY: `text` came from the shim's allocator and is not used again.
    unsafe { hx_free(text) };
    owned
}

pub struct LlamaBackend {
    handle: *mut c_void,
    // Boxed so the address handed to the shim stays put; the shim only calls it on the worker thread.
    progress: Mutex<Option<Box<ProgressFn>>>,
}

// SAFETY: the engine is driven from the queue's single worker thread, plus `unload`/`models`/`snapshot` from others while no
// request runs; the C++ class guards what those read with its own mutex (see llama_backend.h).
unsafe impl Send for LlamaBackend {}
unsafe impl Sync for LlamaBackend {}

impl LlamaBackend {
    pub fn new(model_dir: &Path) -> Self {
        let dir = CString::new(model_dir.to_string_lossy().as_bytes()).unwrap_or_default();
        // SAFETY: `dir` is a valid C string for the call.
        let handle = unsafe { hx_llama_create(dir.as_ptr()) };
        Self {
            handle,
            progress: Mutex::new(None),
        }
    }
}

impl Drop for LlamaBackend {
    fn drop(&mut self) {
        // SAFETY: the handle came from hx_llama_create and is destroyed once.
        unsafe { hx_llama_destroy(self.handle) };
    }
}

extern "C" fn progress_trampoline(user: *mut c_void, model: *const c_char, fraction: c_double) {
    // SAFETY: `user` is the Box<ProgressFn> kept alive in `progress`; `model` is a valid C string for the call.
    let callback = unsafe { &*(user as *const ProgressFn) };
    let model = unsafe { CStr::from_ptr(model) }.to_string_lossy();
    callback(&model, fraction);
}

struct ChatCallbacks<'a> {
    on_delta: &'a mut dyn FnMut(&str),
    cancelled: &'a dyn Fn() -> bool,
}

extern "C" fn delta_trampoline(user: *mut c_void, text: *const c_char) {
    // SAFETY: `user` points at the ChatCallbacks on the caller's stack for the duration of hx_llama_chat.
    let callbacks = unsafe { &mut *(user as *mut ChatCallbacks<'_>) };
    let text = unsafe { CStr::from_ptr(text) }.to_string_lossy();
    (callbacks.on_delta)(&text);
}

extern "C" fn cancel_trampoline(user: *mut c_void) -> c_int {
    // SAFETY: as in `delta_trampoline`.
    let callbacks = unsafe { &*(user as *const ChatCallbacks<'_>) };
    c_int::from((callbacks.cancelled)())
}

impl Backend for LlamaBackend {
    fn models(&self) -> Vec<ModelInfo> {
        // SAFETY: handle is live; the returned string is owned by us.
        let text = unsafe { take(hx_llama_models(self.handle)) };
        serde_json::from_str::<Vec<Value>>(&text)
            .unwrap_or_default()
            .iter()
            .map(|m| ModelInfo {
                id: m["id"].as_str().unwrap_or("").into(),
                bytes: m["bytes"].as_u64().unwrap_or(0),
                loaded: m["loaded"].as_bool().unwrap_or(false),
                vision: m["vision"].as_bool().unwrap_or(false),
            })
            .collect()
    }

    fn load(&self, model: &str) -> Result<(), String> {
        let name = CString::new(model).map_err(|_| "the model name is not valid".to_string())?;
        let mut error: *mut c_char = std::ptr::null_mut();
        // SAFETY: handle is live; `error` is written only on failure and then owned by us.
        let ok = unsafe { hx_llama_load(self.handle, name.as_ptr(), &mut error) };
        if ok != 0 {
            return Ok(());
        }
        Err(unsafe { take(error) })
    }

    fn unload(&self) {
        // SAFETY: handle is live.
        unsafe { hx_llama_unload(self.handle) };
    }

    fn is_loaded(&self) -> bool {
        // SAFETY: handle is live.
        unsafe { hx_llama_is_loaded(self.handle) != 0 }
    }

    fn chat(
        &self,
        request: &ChatRequest,
        on_delta: &mut dyn FnMut(&str),
        cancelled: &dyn Fn() -> bool,
    ) -> Outcome {
        let body = json!({
            "model": request.model,
            "messages": request.messages,
            "schema": request.json_schema,
            "reason_first": request.reason_first,
            "max_tokens": request.max_tokens,
            "temperature": request.temperature,
        })
        .to_string();
        let Ok(body) = CString::new(body) else {
            return Outcome {
                error: "the request could not be encoded".into(),
                ..Outcome::default()
            };
        };
        let pointers: Vec<*const u8> = request.images.iter().map(|i| i.rgb.as_ptr()).collect();
        let widths: Vec<u32> = request.images.iter().map(|i| i.width).collect();
        let heights: Vec<u32> = request.images.iter().map(|i| i.height).collect();
        let mut callbacks = ChatCallbacks {
            on_delta,
            cancelled,
        };
        // SAFETY: every pointer is valid for the call; `callbacks` outlives it; the result is owned by us.
        let text = unsafe {
            take(hx_llama_chat(
                self.handle,
                body.as_ptr(),
                pointers.as_ptr(),
                widths.as_ptr(),
                heights.as_ptr(),
                pointers.len(),
                delta_trampoline,
                cancel_trampoline,
                &mut callbacks as *mut ChatCallbacks<'_> as *mut c_void,
            ))
        };
        let value: Value = serde_json::from_str(&text)
            .unwrap_or_else(|_| json!({"error": "the engine gave no answer"}));
        Outcome {
            ok: value["ok"].as_bool().unwrap_or(false),
            text: value["text"].as_str().unwrap_or("").into(),
            error: value["error"].as_str().unwrap_or("").into(),
            cancelled: value["cancelled"].as_bool().unwrap_or(false),
            truncated: value["truncated"].as_bool().unwrap_or(false),
            prompt_tokens: value["prompt_tokens"].as_u64().unwrap_or(0) as u32,
            cached_prompt_tokens: value["cached_prompt_tokens"].as_u64().unwrap_or(0) as u32,
            completion_tokens: value["completion_tokens"].as_u64().unwrap_or(0) as u32,
            ..Outcome::default()
        }
    }

    fn snapshot(&self) -> Value {
        // SAFETY: handle is live.
        let text = unsafe { take(hx_llama_snapshot(self.handle)) };
        serde_json::from_str(&text).unwrap_or_else(|_| json!({}))
    }

    fn set_progress(&self, progress: ProgressFn) {
        let mut slot = self.progress.lock().unwrap();
        let boxed = Box::new(progress);
        let pointer = &*boxed as *const ProgressFn as *mut c_void;
        *slot = Some(boxed);
        // SAFETY: the box in `slot` outlives the engine's use of the pointer (replaced only here, dropped with self).
        unsafe { hx_llama_set_progress(self.handle, Some(progress_trampoline), pointer) };
    }
}
