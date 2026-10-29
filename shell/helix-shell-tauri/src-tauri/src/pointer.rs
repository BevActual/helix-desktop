//! The Magic Pointer: point at anything and press the pointer key (`helix-shell "point"`), and a bubble opens beside the pointer with
//! what can be done with the thing under it (`helix_shell_model::pointer` has the prompts and the arithmetic).
//!
//! The compositor notes where the pointer was and the window under it as the key is pressed. The bubble offers "What's this?",
//! "Copy text", a question of the person's own, Writing Tools on the selected text, and the actions apps add that fit (one that takes the
//! selection, from anywhere; one that needs nothing, over that app's own windows). The window is only captured when the person picks
//! something to do with it: the window alone (the bubble is not in the picture), cut to the patch around the pointer, kept in memory
//! while the bubble is open and never written anywhere but a moment's temporary file.

use std::io::Cursor;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;

use helix_compositor::compositor_ipc::{take_point_context, PointContext};
use helix_shell_model::pointer::{patch, place, prompt, Ask};
use helix_shell_model::writing::{excerpt, Tool, MAX_SOURCE};
use serde::Serialize;
use serde_json::Value;
use tauri::{AppHandle, Emitter};

use crate::shell_state;

/// One app action the bubble offers.
#[derive(Clone, Serialize)]
pub struct Offer {
    id: String,
    /// "Add to Notes"
    title: String,
    /// "Notes"
    app: String,
    /// The person taps twice for these ("confirm").
    confirm: bool,
    #[serde(skip)]
    params: Value,
}

#[derive(Clone, Default)]
pub struct Pointer {
    context: Option<PointContext>,
    /// Whether the bubble's surface is on the pointer's screen (else it opens in the middle of the focused one).
    here: bool,
    app: String,
    selection: String,
    offers: Vec<Offer>,
    picture: Option<Vec<u8>>,
    ask: Option<Ask>,
    phase: Phase,
    result: String,
    error: String,
    /// What an app action did, with its history entry for Undo.
    done: Option<(String, u64)>,
    generation: u64,
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
enum Phase {
    #[default]
    Ready,
    Working,
    Done,
    Failed,
}

#[derive(Clone, Serialize)]
pub struct PointerView {
    /// The pointer within the output (logical), when the bubble should sit beside it.
    at: Option<(f64, f64)>,
    /// "Firefox", "Pancake recipe"
    app: String,
    title: String,
    /// Whether there is a window to look at.
    window: bool,
    selection: String,
    offers: Vec<Offer>,
    ask: String,
    phase: Phase,
    result: String,
    error: String,
    done: String,
    undoable: bool,
    /// Whether the vision task runs on this computer.
    local: bool,
}

fn view(p: &Pointer) -> PointerView {
    let window = p.context.as_ref().and_then(|c| c.window.as_ref());
    PointerView {
        at: p.context.as_ref().filter(|_| p.here).map(|c| (c.x, c.y)),
        app: p.app.clone(),
        title: window
            .and_then(|w| w.title.clone())
            .unwrap_or_default()
            .chars()
            .take(80)
            .collect(),
        window: window.is_some(),
        selection: excerpt(&p.selection),
        offers: p.offers.clone(),
        ask: p.ask.map(|a| a.id().to_string()).unwrap_or_default(),
        phase: p.phase,
        result: p.result.clone(),
        error: p.error.clone(),
        done: p.done.as_ref().map(|(s, _)| s.clone()).unwrap_or_default(),
        undoable: p.done.as_ref().is_some_and(|(_, entry)| *entry > 0),
        local: helix_config::inference_routing::load().is_local_route("vision"),
    }
}

fn emit(app: &AppHandle) {
    let v = view(&shell_state(app).lock().expect("shell state").pointer);
    let _ = app.emit("pointer-view", v);
}

static READY: AtomicBool = AtomicBool::new(false);
static CAPTURED: Mutex<Option<Pointer>> = Mutex::new(None);

pub fn take_ready() -> bool {
    READY.swap(false, Ordering::SeqCst)
}

/// The primary selection, if the session offers one (no copy key here: pointing should change nothing). A nested test hands it over in
/// a file, since the nested compositor has no clipboard.
fn selection() -> String {
    if helix_base::nested_test_session() {
        if let Some(path) = std::env::var_os("HELIX_POINTER_TEST_SELECTION") {
            return std::fs::read_to_string(path)
                .unwrap_or_default()
                .chars()
                .take(MAX_SOURCE)
                .collect();
        }
    }
    let paste = |args: &[&str]| {
        std::process::Command::new("wl-paste")
            .args(args)
            .output()
            .ok()
            .filter(|o| o.status.success())
            .map(|o| String::from_utf8_lossy(&o.stdout).to_string())
            .unwrap_or_default()
    };
    if paste(&["--primary", "--list-types"]).contains("x-kde-passwordManagerHint") {
        return String::new();
    }
    paste(&["--primary", "--no-newline"])
        .chars()
        .take(MAX_SOURCE)
        .collect()
}

/// The app's name as the person knows it (its desktop entry), else its id.
fn app_name(app: &AppHandle, app_id: &str) -> String {
    if app_id.is_empty() {
        return String::new();
    }
    let known = shell_state(app).lock().expect("shell state").apps.clone();
    let apps = if known.is_empty() {
        crate::apps::scan()
    } else {
        known
    };
    apps.iter()
        .find(|a| {
            a.id.trim_end_matches(".desktop")
                .eq_ignore_ascii_case(app_id)
        })
        .map(|a| a.name.clone())
        .unwrap_or_else(|| app_id.to_string())
}

/// Opening the bubble: the compositor's note, the selection and the fitting app actions are gathered first (off the main thread), then
/// the panel opens on the main thread.
pub fn capture_then_open(app: AppHandle) {
    std::thread::spawn(move || {
        let context = take_point_context();
        let focused_output = crate::niri_query::request("FocusedOutput").and_then(|v| {
            v.get("FocusedOutput")?
                .get("name")?
                .as_str()
                .map(String::from)
        });
        let here = context
            .as_ref()
            .is_some_and(|c| focused_output.as_deref().is_none_or(|f| f == c.output));
        let app_id = context
            .as_ref()
            .and_then(|c| c.window.as_ref())
            .and_then(|w| w.app_id.clone())
            .unwrap_or_default();
        let selection = selection();
        let manifests = {
            let (user, system) = helix_actions::manifest::data_dirs();
            helix_actions::manifest::load(&user, &system)
        };
        let offers = helix_actions::manifest::for_pointer(
            &manifests,
            Some(app_id.as_str()).filter(|a| !a.is_empty()),
            Some(selection.as_str()),
        )
        .into_iter()
        .take(6)
        .map(|(provider, action, params)| Offer {
            id: action.id.clone(),
            title: action.title.clone(),
            app: provider.title.clone(),
            confirm: action.risk == helix_actions::Risk::Confirm,
            params,
        })
        .collect::<Vec<_>>();
        let name = app_name(&app, &app_id);
        eprintln!(
            "POINTER at {} window {} selection {} chars, {} offers",
            context
                .as_ref()
                .map(|c| format!("{:.0},{:.0}", c.x, c.y))
                .unwrap_or_else(|| "nowhere".into()),
            if app_id.is_empty() {
                "none"
            } else {
                app_id.as_str()
            },
            selection.chars().count(),
            offers.len(),
        );
        *CAPTURED.lock().expect("captured") = Some(Pointer {
            context,
            here,
            app: name,
            selection,
            offers,
            ..Pointer::default()
        });
        READY.store(true, Ordering::SeqCst);
        let main = app.clone();
        let _ = app.run_on_main_thread(move || crate::open_panel(&main, "point"));
    });
}

/// Called as the panel opens.
pub fn opened(app: &AppHandle) {
    let captured = CAPTURED
        .lock()
        .expect("captured")
        .take()
        .unwrap_or_default();
    {
        let state = shell_state(app);
        let mut shell = state.lock().expect("shell state");
        let generation = shell.pointer.generation + 1;
        shell.pointer = Pointer {
            generation,
            ..captured
        };
    }
    emit(app);
}

#[tauri::command]
pub fn pointer_view(app: AppHandle) -> PointerView {
    view(&shell_state(&app).lock().expect("shell state").pointer)
}

/// The patch of the window around the pointer, as PNG: captured once per bubble.
fn picture(app: &AppHandle) -> Result<Vec<u8>, String> {
    let context = {
        let state = shell_state(app);
        let shell = state.lock().expect("shell state");
        if let Some(bytes) = &shell.pointer.picture {
            return Ok(bytes.clone());
        }
        shell.pointer.context.clone()
    };
    let window = context
        .and_then(|c| c.window)
        .ok_or("There is no window under the pointer.")?;
    let path = crate::capture::capture_file()?;
    let shot = crate::capture::shoot_window(window.id, &path);
    let bytes = std::fs::read(&path);
    let _ = std::fs::remove_file(&path);
    shot?;
    let bytes = bytes.map_err(|e| format!("The window could not be captured ({e})."))?;
    let full = image::load_from_memory(&bytes).map_err(|e| e.to_string())?;
    let (x, y, w, h) = patch(
        (window.x, window.y),
        (window.width as f64, window.height as f64),
        (full.width(), full.height()),
    );
    let mut png = Vec::new();
    full.crop_imm(x, y, w, h)
        .write_to(&mut Cursor::new(&mut png), image::ImageFormat::Png)
        .map_err(|e| e.to_string())?;
    eprintln!("POINTER picture {w}x{h}");
    shell_state(app)
        .lock()
        .expect("shell state")
        .pointer
        .picture = Some(png.clone());
    Ok(png)
}

/// The model call: a system message, then the question with the picture attached (`{"type":"image","index":0}`), on the Vision task.
fn vision_args(
    system: &str,
    user: &str,
    picture: Vec<u8>,
) -> Result<std::collections::HashMap<&'static str, zbus::zvariant::Value<'static>>, String> {
    use zbus::zvariant::{Array, Dict, Signature, Value as Z};
    let err = |e: zbus::zvariant::Error| e.to_string();
    let v = Signature::try_from("v").map_err(|e| e.to_string())?;
    let s = Signature::try_from("s").map_err(|e| e.to_string())?;
    let entry = |pairs: Vec<(&str, Z<'static>)>| -> Result<Z<'static>, String> {
        let mut d = Dict::new(&s, &v);
        for (k, value) in pairs {
            d.append(Z::from(k.to_string()), Z::Value(Box::new(value)))
                .map_err(err)?;
        }
        Ok(Z::Value(Box::new(Z::Dict(d))))
    };
    let mut parts = Array::new(&v);
    parts
        .append(entry(vec![
            ("type", Z::from("text".to_string())),
            ("text", Z::from(user.to_string())),
        ])?)
        .map_err(err)?;
    parts
        .append(entry(vec![
            ("type", Z::from("image".to_string())),
            ("index", Z::U32(0)),
        ])?)
        .map_err(err)?;
    let mut list = Array::new(&v);
    list.append(entry(vec![
        ("role", Z::from("system".to_string())),
        ("content", Z::from(system.to_string())),
    ])?)
    .map_err(err)?;
    list.append(entry(vec![
        ("role", Z::from("user".to_string())),
        ("content", Z::Array(parts)),
    ])?)
    .map_err(err)?;
    let mut images = Array::new(&Signature::try_from("ay").map_err(|e| e.to_string())?);
    images.append(Z::from(picture)).map_err(err)?;
    let mut args = std::collections::HashMap::new();
    args.insert("messages", Z::Array(list));
    args.insert("images", Z::Array(images));
    args.insert("task", Z::from("vision".to_string()));
    args.insert("temperature", Z::F64(0.2));
    args.insert("max_tokens", Z::U32(400));
    args.insert("priority", Z::from("interactive".to_string()));
    Ok(args)
}

/// "What's this?", "Copy text" or a question: the patch around the pointer goes to the Vision model and the answer streams into the
/// bubble. Copy text puts what was read on the clipboard when it is done.
#[tauri::command]
pub async fn pointer_ask(
    app: AppHandle,
    ask: String,
    question: Option<String>,
) -> Result<(), String> {
    let ask = Ask::parse(&ask).ok_or("That is not something the pointer can do.")?;
    let question = question.unwrap_or_default();
    if ask == Ask::Question && question.trim().is_empty() {
        return Err("Ask a question first.".into());
    }
    let (generation, where_) = {
        let state = shell_state(&app);
        let mut shell = state.lock().expect("shell state");
        let p = &mut shell.pointer;
        p.generation += 1;
        p.ask = Some(ask);
        p.phase = Phase::Working;
        p.result.clear();
        p.error.clear();
        p.done = None;
        let title = p
            .context
            .as_ref()
            .and_then(|c| c.window.as_ref())
            .and_then(|w| w.title.clone())
            .unwrap_or_default();
        (p.generation, place(&p.app, &title))
    };
    eprintln!("POINTER ask {}", ask.id());
    emit(&app);
    let capturing = app.clone();
    let outcome = match tauri::async_runtime::spawn_blocking(move || picture(&capturing))
        .await
        .map_err(|e| e.to_string())
        .and_then(|r| r)
    {
        Ok(png) => {
            let (system, user) = prompt(ask, &where_, &question);
            match vision_args(&system, &user, png) {
                Ok(args) => {
                    crate::writing::stream_chat(args, |piece| {
                        let state = shell_state(&app);
                        let mut shell = state.lock().expect("shell state");
                        if shell.pointer.generation != generation {
                            return false;
                        }
                        match piece {
                            crate::writing::Piece::More(chunk) => {
                                shell.pointer.result.push_str(&chunk)
                            }
                            crate::writing::Piece::Whole(text) => shell.pointer.result = text,
                        }
                        drop(shell);
                        emit(&app);
                        true
                    })
                    .await
                }
                Err(why) => Err(why),
            }
        }
        Err(why) => Err(why),
    };
    let copy = {
        let state = shell_state(&app);
        let mut shell = state.lock().expect("shell state");
        let p = &mut shell.pointer;
        if p.generation != generation {
            return Ok(());
        }
        match outcome {
            Ok(()) => {
                p.result = p.result.trim().to_string();
                p.phase = Phase::Done;
            }
            Err(why) => {
                p.phase = Phase::Failed;
                p.error = why;
            }
        }
        eprintln!(
            "POINTER {} ({} chars)",
            if p.phase == Phase::Done {
                "done"
            } else {
                "failed"
            },
            p.result.chars().count()
        );
        (ask == Ask::ReadText && p.phase == Phase::Done && !p.result.is_empty())
            .then(|| p.result.clone())
    };
    if let Some(text) = copy {
        if crate::insert::copy_text(&text).is_ok() {
            eprintln!("POINTER copied");
        }
    }
    emit(&app);
    Ok(())
}

/// Writing Tools on the selection: the bubble gives way to the Writing Tools panel, already running `tool` (none: just open it).
#[tauri::command]
pub fn pointer_writing(app: AppHandle, tool: Option<String>) -> Result<(), String> {
    let source = shell_state(&app)
        .lock()
        .expect("shell state")
        .pointer
        .selection
        .clone();
    if source.trim().is_empty() {
        return Err("Nothing is selected.".into());
    }
    let tool = tool.as_deref().and_then(Tool::parse);
    eprintln!("POINTER writing {}", tool.map(|t| t.id()).unwrap_or("open"));
    crate::close_panel(&app);
    crate::writing::open_with(&app, source, tool);
    Ok(())
}

/// Runs one of the offered app actions for the person (the desktop records it as theirs, with the app's own Undo).
#[tauri::command]
pub async fn pointer_offer(app: AppHandle, id: String) -> Result<(), String> {
    let params = {
        let state = shell_state(&app);
        let shell = state.lock().expect("shell state");
        shell
            .pointer
            .offers
            .iter()
            .find(|o| o.id == id)
            .map(|o| o.params.clone())
            .ok_or("That action is not offered here.")?
    };
    let desktop = crate::REGISTERED
        .get()
        .ok_or("this shell is not the registered desktop")?;
    let reply = desktop
        .run_app_action(&id, &params)
        .await
        .map_err(|e| match e {
            zbus::Error::MethodError(_, Some(why), _) => why,
            other => other.to_string(),
        });
    {
        let state = shell_state(&app);
        let mut shell = state.lock().expect("shell state");
        let p = &mut shell.pointer;
        p.ask = None;
        p.result.clear();
        match reply {
            Ok(reply) => {
                let sentence = reply
                    .get("sentence")
                    .and_then(Value::as_str)
                    .unwrap_or("Done")
                    .to_string();
                let entry = reply.get("history").and_then(Value::as_u64).unwrap_or(0);
                eprintln!("POINTER ran {id}: {sentence}");
                p.done = Some((sentence, entry));
                p.phase = Phase::Done;
                p.error.clear();
            }
            Err(why) => {
                eprintln!("POINTER {id} failed: {why}");
                p.done = None;
                p.phase = Phase::Failed;
                p.error = why;
            }
        }
    }
    emit(&app);
    Ok(())
}

/// Undo of what the last offered action did.
#[tauri::command]
pub async fn pointer_undo(app: AppHandle) -> Result<String, String> {
    eprintln!("POINTER undo pressed");
    let entry = shell_state(&app)
        .lock()
        .expect("shell state")
        .pointer
        .done
        .clone()
        .map(|(_, entry)| entry)
        .ok_or("Nothing to undo.")?;
    let desktop = crate::REGISTERED
        .get()
        .ok_or("this shell is not the registered desktop")?;
    let outcome = desktop.undo_action(entry).await.map_err(|e| match e {
        zbus::Error::MethodError(_, Some(why), _) => why,
        other => other.to_string(),
    });
    {
        let state = shell_state(&app);
        let mut shell = state.lock().expect("shell state");
        let p = &mut shell.pointer;
        match &outcome {
            Ok(sentence) => {
                eprintln!("POINTER undid {entry}");
                p.done = Some((sentence.clone(), 0));
                p.phase = Phase::Done;
            }
            Err(why) => {
                eprintln!("POINTER undo failed: {why}");
                p.done = None;
                p.phase = Phase::Failed;
                p.error = why.clone();
            }
        }
    }
    emit(&app);
    outcome
}

/// Back to the menu from an answer.
#[tauri::command]
pub fn pointer_back(app: AppHandle) {
    {
        let state = shell_state(&app);
        let mut shell = state.lock().expect("shell state");
        let p = &mut shell.pointer;
        p.generation += 1;
        p.ask = None;
        p.done = None;
        p.phase = Phase::Ready;
        p.result.clear();
        p.error.clear();
    }
    emit(&app);
}

/// Copy: the answer goes on the clipboard.
#[tauri::command]
pub fn pointer_copy(app: AppHandle) -> Result<(), String> {
    let text = shell_state(&app)
        .lock()
        .expect("shell state")
        .pointer
        .result
        .clone();
    if text.is_empty() {
        return Err("Nothing to copy yet.".into());
    }
    crate::insert::copy_text(&text)?;
    eprintln!("POINTER copied");
    Ok(())
}
