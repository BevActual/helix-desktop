//! Helix Models: the app for the model gateway (`org.helix.Inference`, `docs/design/helix-models.md`). A window onto the gateway: what the engine is
//! doing, the backends (this computer, other computers, hosted services), the installed models, and which backend answers each slot. It never talks to a
//! model or a provider itself and never reads a key: the gateway does the work (`Probe` asks a backend what it offers), the routing file is the one
//! the gateway reads on every request (validated here with the same shared types), and a key typed here is written once, for the gateway to read.

use std::collections::HashMap;
use std::io::Write;
use std::os::unix::fs::OpenOptionsExt;
use std::path::PathBuf;

use helix_config::inference_routing::{self, Backend, BackendKind};
use serde::Serialize;
use serde_json::{json, Value as Json};
use tauri::{WebviewUrl, WebviewWindowBuilder};
use zbus::zvariant::{OwnedValue, Value};

const WINDOW: &str = "models";
const SLOTS: [&str; 5] = ["act", "chat", "summarize", "vision", "code"];

// --- the gateway ----------------------------------------------------------------------------------------------------------------------

static CONNECTION: tokio::sync::OnceCell<zbus::Connection> = tokio::sync::OnceCell::const_new();

async fn proxy() -> Result<zbus::Proxy<'static>, String> {
    let connection = CONNECTION
        .get_or_try_init(zbus::Connection::session)
        .await
        .map_err(|e| e.to_string())?;
    zbus::Proxy::new(
        connection,
        "org.helix.Inference",
        "/org/helix/Inference",
        "org.helix.Inference1",
    )
    .await
    .map_err(|e| e.to_string())
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
        Value::Array(items) => Json::Array(items.iter().map(from_variant).collect()),
        Value::Dict(entries) => {
            let mut object = serde_json::Map::new();
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

fn to_json(map: HashMap<String, OwnedValue>) -> Json {
    Json::Object(
        map.iter()
            .map(|(k, v)| (k.clone(), from_variant(v)))
            .collect(),
    )
}

// --- what the page is shown -----------------------------------------------------------------------------------------------------------

#[derive(Serialize)]
struct BackendView {
    id: String,
    label: String,
    kind: &'static str,
    base_url: String,
    /// `this` (this computer), `lan` (a computer on the same network) or `cloud` (anything on the internet).
    trust: &'static str,
    has_key: bool,
}

#[derive(Serialize)]
struct SlotView {
    slot: &'static str,
    backend: String,
    model: String,
    /// Only a model on this computer may act for the desktop.
    local_only: bool,
    /// Where "ask a stronger model" goes for this slot (empty for nowhere).
    escalate_backend: String,
    escalate_model: String,
}

#[derive(Serialize)]
struct View {
    /// Whether the gateway answered.
    gateway: bool,
    error: String,
    /// The gateway's own snapshot: engine, loaded model, devices, queue, model folder.
    snapshot: Json,
    models: Vec<Json>,
    backends: Vec<BackendView>,
    slots: Vec<SlotView>,
    allow_remote_on_battery_saver: bool,
}

fn secret_path(reference: &str) -> PathBuf {
    inference_routing::state_dir()
        .join("secrets")
        .join(format!("backend-{reference}"))
}

/// Whether a key is kept for this reference: the file exists and has something in it. The key itself is never read here.
fn has_key(reference: &str) -> bool {
    std::fs::metadata(secret_path(reference)).is_ok_and(|m| m.len() > 0)
}

fn reference_of(backend: &Backend) -> &str {
    if backend.api_key_ref.is_empty() {
        &backend.id
    } else {
        &backend.api_key_ref
    }
}

fn view_of(routing: &inference_routing::Routing) -> (Vec<BackendView>, Vec<SlotView>) {
    let backends = routing
        .backends
        .iter()
        .map(|b| BackendView {
            id: b.id.clone(),
            label: if b.label.is_empty() {
                b.id.clone()
            } else {
                b.label.clone()
            },
            kind: if b.kind == BackendKind::Local {
                "local"
            } else {
                "openai"
            },
            base_url: b.base_url.clone(),
            trust: if b.kind == BackendKind::Local {
                "this"
            } else if inference_routing::is_local_network_url(&b.base_url) {
                "lan"
            } else {
                "cloud"
            },
            has_key: b.kind != BackendKind::Local && has_key(reference_of(b)),
        })
        .collect();
    let slots = SLOTS
        .iter()
        .map(|slot| {
            let route = routing.route(slot);
            SlotView {
                slot,
                backend: if route.backend.is_empty() {
                    "local".into()
                } else {
                    route.backend
                },
                model: route.model,
                local_only: *slot == "act",
                escalate_backend: route.escalate_backend,
                escalate_model: route.escalate_model,
            }
        })
        .collect();
    (backends, slots)
}

#[tauri::command]
async fn models_view() -> View {
    let routing = inference_routing::load();
    let (backends, slots) = view_of(&routing);
    let mut view = View {
        gateway: false,
        error: String::new(),
        snapshot: Json::Null,
        models: Vec::new(),
        backends,
        slots,
        allow_remote_on_battery_saver: routing.allow_remote_on_battery_saver,
    };
    match proxy().await {
        Ok(proxy) => {
            match proxy
                .call::<_, _, HashMap<String, OwnedValue>>("GetSnapshot", &())
                .await
            {
                Ok(snapshot) => {
                    view.gateway = true;
                    view.snapshot = to_json(snapshot);
                }
                Err(error) => view.error = format!("The model service is not answering: {error}"),
            }
            if view.gateway {
                if let Ok(models) = proxy.call::<_, _, Vec<OwnedValue>>("ListModels", &()).await {
                    view.models = models.iter().map(|m| from_variant(m)).collect();
                }
            }
        }
        Err(error) => view.error = error,
    }
    view
}

// --- changes (each one validated, then written where the gateway reads it) --------------------------------------------------------------

fn plain_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 32
        && id
            .chars()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-' || c == '_')
}

/// Which backend and model answers a slot. The Act slot only ever runs on this computer; a backend must exist.
#[tauri::command]
fn slot_set(slot: String, backend: String, model: String) -> Result<(), String> {
    if !SLOTS.contains(&slot.as_str()) {
        return Err("That is not one of the slots.".into());
    }
    if model.len() > 200 || model.chars().any(char::is_control) {
        return Err("That model name is not valid.".into());
    }
    let mut routing = inference_routing::load();
    let found = routing
        .backends
        .iter()
        .find(|b| b.id == backend)
        .ok_or("That backend is not set up.")?;
    if slot == "act" && found.kind != BackendKind::Local {
        return Err("Acting for the desktop only runs on a model on this computer.".into());
    }
    // Only the backend and model change; the slot's stronger-model choice stays.
    let mut route = routing.route(&slot);
    route.backend = backend;
    route.model = model.trim().to_string();
    routing.profiles.insert(slot, route);
    inference_routing::save(&routing)
}

/// Where "ask a stronger model" goes for a slot, and where a request too long for the local model goes. Never for Act; the backend must be set up
/// and not this computer (an empty backend turns it off).
#[tauri::command]
fn slot_escalate(slot: String, backend: String, model: String) -> Result<(), String> {
    if !SLOTS.contains(&slot.as_str()) || slot == "act" {
        return Err("A stronger model can be set for Talk, Summarise, Pictures and Code.".into());
    }
    if model.len() > 200 || model.chars().any(char::is_control) {
        return Err("That model name is not valid.".into());
    }
    let mut routing = inference_routing::load();
    if !backend.is_empty() {
        let found = routing
            .backends
            .iter()
            .find(|b| b.id == backend)
            .ok_or("That backend is not set up.")?;
        if found.kind == BackendKind::Local {
            return Err("A stronger model is on another computer or a service; this computer already answers.".into());
        }
    }
    let mut route = routing.route(&slot);
    route.escalate_model = if backend.is_empty() {
        String::new()
    } else {
        model.trim().to_string()
    };
    route.escalate_backend = backend;
    routing.profiles.insert(slot, route);
    inference_routing::save(&routing)
}

/// Adds or changes a backend that speaks the OpenAI chat protocol: another computer on the network (`llama-server`, Ollama, LM Studio) or a hosted
/// service. A key, if given, is written for the gateway; an empty key leaves the saved one alone.
#[tauri::command]
fn backend_save(id: String, label: String, base_url: String, key: String) -> Result<(), String> {
    if !plain_id(&id) || id == "local" {
        return Err(
            "The id is lowercase letters, digits, - and _ (up to 32), and not 'local'.".into(),
        );
    }
    let base_url = base_url.trim().trim_end_matches('/').to_string();
    let secure = base_url.starts_with("https://");
    if !(secure
        || (base_url.starts_with("http://") && inference_routing::is_local_network_url(&base_url)))
    {
        return Err("A backend on the internet must use https. A computer on your own network may use http.".into());
    }
    if base_url.len() > 300 || base_url.chars().any(|c| c.is_control() || c == ' ') {
        return Err("That address is not valid.".into());
    }
    let label = label.trim().chars().take(60).collect::<String>();
    let mut routing = inference_routing::load();
    let backend = Backend {
        id: id.clone(),
        kind: BackendKind::Openai,
        base_url,
        label,
        api_key_ref: id.clone(),
    };
    match routing.backends.iter_mut().find(|b| b.id == id) {
        Some(existing) if existing.kind == BackendKind::Local => {
            return Err("That one is this computer.".into())
        }
        Some(existing) => *existing = backend,
        None => routing.backends.push(backend),
    }
    if !key.trim().is_empty() {
        write_key(&id, key.trim())?;
    }
    inference_routing::save(&routing)
}

fn write_key(reference: &str, secret: &str) -> Result<(), String> {
    use std::os::unix::fs::PermissionsExt;
    let dir = inference_routing::state_dir().join("secrets");
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let _ = std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o700));
    let path = secret_path(reference);
    let temporary = path.with_extension("tmp");
    {
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(&temporary)
            .map_err(|e| e.to_string())?;
        file.write_all(secret.as_bytes())
            .map_err(|e| e.to_string())?;
        file.sync_all().map_err(|e| e.to_string())?;
    }
    std::fs::rename(&temporary, &path).map_err(|e| e.to_string())
}

#[tauri::command]
fn key_clear(id: String) -> Result<(), String> {
    if !plain_id(&id) {
        return Err("That is not a backend.".into());
    }
    match std::fs::remove_file(secret_path(&id)) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(format!("The saved key could not be removed: {e}")),
    }
}

/// Removes a backend that no slot uses, and its saved key.
#[tauri::command]
fn backend_remove(id: String) -> Result<(), String> {
    let mut routing = inference_routing::load();
    let found = routing
        .backends
        .iter()
        .find(|b| b.id == id)
        .ok_or("That backend is not set up.")?;
    if found.kind == BackendKind::Local {
        return Err("This computer cannot be removed.".into());
    }
    if let Some((slot, _)) = routing
        .profiles
        .iter()
        .find(|(_, route)| route.backend == id)
    {
        return Err(format!(
            "The {slot} slot still uses it. Choose another backend for it first."
        ));
    }
    routing.backends.retain(|b| b.id != id);
    inference_routing::save(&routing)?;
    let _ = std::fs::remove_file(secret_path(&id));
    Ok(())
}

#[tauri::command]
fn battery_remote(allow: bool) -> Result<(), String> {
    let mut routing = inference_routing::load();
    routing.allow_remote_on_battery_saver = allow;
    inference_routing::save(&routing)
}

/// Asks the gateway to test a backend: it reaches it with its key and lists the models it offers.
#[tauri::command]
async fn backend_probe(id: String) -> Result<Json, String> {
    let proxy = proxy().await?;
    let text: String = proxy
        .call("Probe", &(id,))
        .await
        .map_err(|e| e.to_string())?;
    serde_json::from_str(&text).map_err(|e| e.to_string())
}

/// What a Hugging Face repository offers, through the model service (it fetches; this app never does).
#[tauri::command]
async fn hub_files(repo: String) -> Result<Json, String> {
    let proxy = proxy().await?;
    let text: String = proxy
        .call("HubFiles", &(repo.trim(),))
        .await
        .map_err(|e| gateway_error(&e))?;
    serde_json::from_str(&text).map_err(|e| e.to_string())
}

/// Starts a download in the model service. Asked for by the person, never on its own.
#[tauri::command]
async fn model_download(repo: String, file: String, projector: bool) -> Result<u64, String> {
    let proxy = proxy().await?;
    proxy
        .call("Download", &(repo.trim(), file.as_str(), projector))
        .await
        .map_err(|e| gateway_error(&e))
}

#[tauri::command]
async fn downloads() -> Result<Json, String> {
    let proxy = proxy().await?;
    let text: String = proxy
        .call("Downloads", &())
        .await
        .map_err(|e| gateway_error(&e))?;
    serde_json::from_str(&text).map_err(|e| e.to_string())
}

#[tauri::command]
async fn download_cancel(id: u64) -> Result<bool, String> {
    let proxy = proxy().await?;
    proxy
        .call("CancelDownload", &(id,))
        .await
        .map_err(|e| gateway_error(&e))
}

/// The machine's memory in bytes, for "does it fit" hints.
#[tauri::command]
fn machine_memory() -> u64 {
    std::fs::read_to_string("/proc/meminfo")
        .ok()
        .and_then(|t| {
            t.lines()
                .find(|l| l.starts_with("MemTotal:"))
                .and_then(|l| l.split_whitespace().nth(1))
                .and_then(|kb| kb.parse::<u64>().ok())
        })
        .map_or(0, |kb| kb * 1024)
}

/// The service's own words when it refuses (zbus wraps them).
fn gateway_error(error: &zbus::Error) -> String {
    match error {
        zbus::Error::MethodError(_, Some(text), _) => text.clone(),
        other => other.to_string(),
    }
}

/// Frees the engine's memory now, if nothing is being answered.
#[tauri::command]
async fn engine_unload() -> Result<bool, String> {
    let proxy = proxy().await?;
    proxy.call("Unload", &()).await.map_err(|e| e.to_string())
}

/// Deletes an installed model (every part of a split one). Only a model the gateway lists, and never while it is loaded.
#[tauri::command]
async fn model_delete(id: String) -> Result<(), String> {
    let proxy = proxy().await?;
    let snapshot: HashMap<String, OwnedValue> = proxy
        .call("GetSnapshot", &())
        .await
        .map_err(|e| e.to_string())?;
    let snapshot = to_json(snapshot);
    let dir = PathBuf::from(
        snapshot["model_dir"]
            .as_str()
            .ok_or("The model folder is not known.")?,
    );
    let models: Vec<OwnedValue> = proxy
        .call("ListModels", &())
        .await
        .map_err(|e| e.to_string())?;
    let listed = models
        .iter()
        .map(|m| from_variant(m))
        .find(|m| m["id"] == id)
        .ok_or("That model is not installed.")?;
    if listed["loaded"] == true {
        return Err("It is in use. Free the engine's memory first.".into());
    }
    let parts = shard_group(&id);
    let mut removed = 0;
    for name in &parts {
        match std::fs::remove_file(dir.join(name)) {
            Ok(()) => removed += 1,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(e) => return Err(format!("{name} could not be deleted: {e}")),
        }
    }
    if removed == 0 {
        return Err("Nothing was deleted.".into());
    }
    eprintln!("MODELS deleted {id} ({removed} files)");
    Ok(())
}

/// `name-00001-of-00003.gguf` → all three file names; any other name → itself. (Only ever names inside the model folder: no path separators.)
fn shard_group(id: &str) -> Vec<String> {
    let plain = !id.contains('/') && !id.contains('\\') && !id.starts_with('.');
    if !plain {
        return Vec::new();
    }
    if let Some(stem) = id.strip_suffix(".gguf") {
        if let Some(at) = stem.len().checked_sub(15) {
            if let Some((base, tail)) = stem.split_at_checked(at) {
                if let Some((index, total)) =
                    tail.strip_prefix('-').and_then(|t| t.split_once("-of-"))
                {
                    let digits = |s: &str| s.len() == 5 && s.bytes().all(|b| b.is_ascii_digit());
                    if digits(index) && digits(total) {
                        if let Ok(count) = total.parse::<u32>() {
                            return (1..=count.min(99))
                                .map(|i| format!("{base}-{i:05}-of-{total}.gguf"))
                                .collect();
                        }
                    }
                }
            }
        }
    }
    vec![id.to_string()]
}

// --- window, theme, layout reporting ----------------------------------------------------------------------------------------------------

#[tauri::command]
fn layout_reporting() -> bool {
    std::env::var_os("HELIX_UI_LAYOUT_LOG").is_some()
}

#[tauri::command]
fn report_layout(report: Json) {
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

fn startup_page() -> Option<String> {
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let page = if arg == "--page" {
            args.next()
        } else {
            arg.strip_prefix("--page=").map(str::to_string)
        };
        if let Some(page) =
            page.filter(|p| ["overview", "backends", "models", "routing"].contains(&p.as_str()))
        {
            return Some(page);
        }
    }
    None
}

/// 0 until the desktop service has said, then 1 for dark and 2 for light.
static APP_THEME: std::sync::atomic::AtomicU8 = std::sync::atomic::AtomicU8::new(0);
static APP_ACCENT: std::sync::Mutex<String> = std::sync::Mutex::new(String::new());

fn theme_script() -> Option<String> {
    let dark = match APP_THEME.load(std::sync::atomic::Ordering::Relaxed) {
        1 => true,
        2 => false,
        _ => return None,
    };
    let accent = APP_ACCENT.lock().map(|a| a.clone()).unwrap_or_default();
    let look = helix_desktop_api::theme_script(&helix_desktop_api::Look { dark, accent });
    Some(format!(
        "{look}; {}",
        helix_config::shape_prefs::read().script("app")
    ))
}

/// The interface scale, light or dark theme, corners and glass (Settings) follow the person's choices, now and as they change.
fn follow_appearance(app: tauri::AppHandle) {
    let shape_app = app.clone();
    helix_config::shape_prefs::watch(move |_| {
        if let Some(script) = theme_script() {
            let target = shape_app.clone();
            let _ = shape_app.run_on_main_thread(move || {
                for window in tauri::Manager::webview_windows(&target).values() {
                    let _ = window.eval(&script);
                }
            });
        }
    });
    let scale_app = app.clone();
    tauri::async_runtime::spawn(async move {
        let Ok(desktop) = helix_desktop_api::Desktop::session().await else {
            return;
        };
        let _ = desktop
            .watch_ui_scale(move |scale| {
                let app = scale_app.clone();
                std::thread::spawn(move || {
                    for _ in 0..40 {
                        let windows = tauri::Manager::webview_windows(&app);
                        if !windows.is_empty() {
                            for window in windows.values() {
                                let _ = window.set_zoom(scale);
                            }
                            return;
                        }
                        std::thread::sleep(std::time::Duration::from_millis(250));
                    }
                });
            })
            .await;
    });
    tauri::async_runtime::spawn(async move {
        let Ok(desktop) = helix_desktop_api::Desktop::session().await else {
            return;
        };
        let _ = desktop
            .watch_theme(move |look| {
                APP_THEME.store(
                    if look.dark { 1 } else { 2 },
                    std::sync::atomic::Ordering::Relaxed,
                );
                if let Ok(mut accent) = APP_ACCENT.lock() {
                    *accent = look.accent.clone();
                }
                if let Some(script) = theme_script() {
                    for window in tauri::Manager::webview_windows(&app).values() {
                        let _ = window.eval(&script);
                    }
                }
            })
            .await;
    });
}

fn main() {
    gtk::glib::set_prgname(Some("org.helix.Models"));
    if std::env::var_os("JSC_useJIT").is_none() {
        std::env::set_var("JSC_useJIT", "false");
    }
    let page = startup_page();
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            models_view,
            slot_set,
            slot_escalate,
            backend_save,
            backend_remove,
            backend_probe,
            key_clear,
            battery_remote,
            engine_unload,
            model_delete,
            hub_files,
            model_download,
            downloads,
            download_cancel,
            machine_memory,
            layout_reporting,
            report_layout
        ])
        .on_page_load(|webview, payload| {
            if matches!(payload.event(), tauri::webview::PageLoadEvent::Finished) {
                if let Some(script) = theme_script() {
                    let _ = webview.eval(&script);
                }
            }
        })
        .setup(move |app| {
            follow_appearance(app.handle().clone());
            let url = match &page {
                Some(p) => format!("index.html?page={p}"),
                None => "index.html".into(),
            };
            WebviewWindowBuilder::new(app, WINDOW, WebviewUrl::App(url.into()))
                .title("Helix Models")
                .decorations(false)
                .transparent(true)
                .inner_size(1000.0, 700.0)
                .min_inner_size(760.0, 520.0)
                .build()?;
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running Helix Models");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_split_model_is_every_one_of_its_parts_and_nothing_outside_the_folder() {
        assert_eq!(
            shard_group("q-00001-of-00003.gguf"),
            [
                "q-00001-of-00003.gguf",
                "q-00002-of-00003.gguf",
                "q-00003-of-00003.gguf"
            ]
        );
        assert_eq!(shard_group("plain.gguf"), ["plain.gguf"]);
        assert!(shard_group("../../etc/passwd").is_empty());
        assert!(shard_group("a/b.gguf").is_empty());
        assert!(shard_group(".hidden.gguf").is_empty());
    }

    #[test]
    fn ids_are_short_plain_names() {
        assert!(plain_id("studio-2"));
        assert!(!plain_id("Studio"));
        assert!(!plain_id(""));
        assert!(!plain_id("a b"));
        assert!(!plain_id(&"x".repeat(33)));
    }

    #[test]
    fn a_backend_and_its_trust_are_described_for_the_page() {
        let mut routing = inference_routing::Routing::default();
        routing.backends.push(Backend {
            id: "studio".into(),
            kind: BackendKind::Openai,
            base_url: "http://192.168.1.20:8080/v1".into(),
            label: "Studio".into(),
            api_key_ref: String::new(),
        });
        routing.backends.push(Backend {
            id: "openrouter".into(),
            kind: BackendKind::Openai,
            base_url: "https://openrouter.ai/api/v1".into(),
            label: String::new(),
            api_key_ref: "openrouter".into(),
        });
        let (backends, slots) = view_of(&routing);
        let trust: Vec<(&str, &str)> = backends.iter().map(|b| (b.id.as_str(), b.trust)).collect();
        assert_eq!(
            trust,
            [
                ("local", "this"),
                ("studio", "lan"),
                ("openrouter", "cloud")
            ]
        );
        assert_eq!(backends[2].label, "openrouter", "no label shows the id");
        assert_eq!(slots.len(), 5);
        assert!(slots.iter().find(|s| s.slot == "act").unwrap().local_only);
    }

    #[test]
    fn a_slot_and_a_backend_are_validated_before_anything_is_written() {
        let dir = std::env::temp_dir().join(format!("helix-models-app-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::env::set_var("HELIX_STATE_HOME", &dir);
        assert!(backend_save(
            "Bad Id".into(),
            "x".into(),
            "https://x.test/v1".into(),
            String::new()
        )
        .is_err());
        assert!(backend_save(
            "local".into(),
            "x".into(),
            "https://x.test/v1".into(),
            String::new()
        )
        .is_err());
        assert!(
            backend_save(
                "web".into(),
                "x".into(),
                "http://api.example.com/v1".into(),
                String::new()
            )
            .is_err(),
            "no key over http to the internet"
        );
        backend_save(
            "studio".into(),
            "Studio".into(),
            "http://192.168.1.20:8080/v1/".into(),
            "sk-secret".into(),
        )
        .unwrap();
        let saved = inference_routing::load();
        let studio = saved.backends.iter().find(|b| b.id == "studio").unwrap();
        assert_eq!(studio.base_url, "http://192.168.1.20:8080/v1");
        assert!(has_key("studio"));
        assert!(
            !std::fs::read_to_string(inference_routing::routing_path())
                .unwrap()
                .contains("sk-secret"),
            "the key is not in the routing file"
        );
        // A slot: Act only on this computer; a backend must exist.
        assert!(slot_set("act".into(), "studio".into(), "m".into()).is_err());
        assert!(slot_set("chat".into(), "nowhere".into(), "m".into()).is_err());
        assert!(slot_set("poetry".into(), "local".into(), "m".into()).is_err());
        // A stronger model: not for Act, not this computer, only a backend that is set up; changing the slot's own route keeps it.
        assert!(slot_escalate("act".into(), "studio".into(), "m".into()).is_err());
        assert!(slot_escalate("chat".into(), "local".into(), "m".into()).is_err());
        assert!(slot_escalate("chat".into(), "nowhere".into(), "m".into()).is_err());
        slot_escalate("chat".into(), "studio".into(), "huge".into()).unwrap();
        slot_set("chat".into(), "local".into(), "small.gguf".into()).unwrap();
        let kept = inference_routing::load().route("chat");
        assert_eq!(
            (
                kept.escalate_backend.as_str(),
                kept.escalate_model.as_str(),
                kept.model.as_str()
            ),
            ("studio", "huge", "small.gguf")
        );
        slot_escalate("chat".into(), String::new(), "ignored".into()).unwrap();
        assert_eq!(inference_routing::load().route("chat").escalate_model, "");
        slot_set("chat".into(), "studio".into(), "big-model".into()).unwrap();
        slot_set("chat".into(), "studio".into(), "big-model".into()).unwrap();
        assert_eq!(inference_routing::load().route("chat").model, "big-model");
        // A backend in use cannot be removed; once free it goes, with its key.
        assert!(backend_remove("studio".into())
            .unwrap_err()
            .contains("chat"));
        slot_set("chat".into(), "local".into(), String::new()).unwrap();
        backend_remove("studio".into()).unwrap();
        assert!(inference_routing::load()
            .backends
            .iter()
            .all(|b| b.id != "studio"));
        assert!(!has_key("studio"));
        assert!(backend_remove("local".into()).is_err());
        std::env::remove_var("HELIX_STATE_HOME");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
