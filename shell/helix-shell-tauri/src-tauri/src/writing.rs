//! Writing Tools: the panel that rewrites, proofreads, summarises or explains the text selected in any app (`helix_shell_model::writing`
//! has the tools and the prompts). The selection is read when the panel opens (the primary selection, else the clipboard); the answer
//! streams from the model service's Summaries task (small and quick, on this computer unless the person routed it elsewhere); Replace
//! pastes a rewrite over the selection in the window that had the keyboard, through the same careful path the Caps Lock palette uses.

use std::process::Command;
use std::time::Duration;

use futures_util::StreamExt;
use helix_shell_model::writing::{excerpt, messages, tidy, Tool, MAX_SOURCE};
use serde::Serialize;
use tauri::{AppHandle, Emitter};

use crate::shell_state;

#[derive(Clone, Default)]
pub struct Writing {
    source: String,
    tool: Option<Tool>,
    phase: Phase,
    result: String,
    error: String,
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
pub struct WritingView {
    excerpt: String,
    chars: usize,
    tool: String,
    phase: Phase,
    result: String,
    error: String,
    replaceable: bool,
    /// Whether the Summaries task runs on this computer (else the answer comes from the service the person routed it to).
    local: bool,
}

fn view(w: &Writing) -> WritingView {
    WritingView {
        excerpt: excerpt(&w.source),
        chars: w.source.chars().count(),
        tool: w.tool.map(|t| t.id().to_string()).unwrap_or_default(),
        phase: w.phase,
        result: w.result.clone(),
        error: w.error.clone(),
        replaceable: w.tool.is_some_and(Tool::replaces)
            && w.phase == Phase::Done
            && !w.result.is_empty(),
        local: helix_config::inference_routing::load().is_local_route("summarize"),
    }
}

fn emit(app: &AppHandle) {
    let v = view(&shell_state(app).lock().expect("shell state").writing);
    let _ = app.emit("writing-view", v);
}

/// Set while the selection has been captured and the panel is about to open (so opening does not capture again).
static READY: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
/// What was captured for the panel that is opening.
static CAPTURED: std::sync::Mutex<String> = std::sync::Mutex::new(String::new());

pub fn take_ready() -> bool {
    READY.swap(false, std::sync::atomic::Ordering::SeqCst)
}

fn wl_paste(args: &[&str]) -> String {
    Command::new("wl-paste")
        .args(args)
        .output()
        .ok()
        .filter(|o| o.status.success())
        .map(|o| String::from_utf8_lossy(&o.stdout).to_string())
        .unwrap_or_default()
}

/// What is selected in the window that has the keyboard: the primary selection when the session offers it, else a copy made in that
/// window (the copy key, then the clipboard is read and what was on it before is put back). Never the copy key in a terminal, where it
/// interrupts. Nothing a password manager marks.
fn selection(app: &AppHandle) -> String {
    // The nested test compositor has no clipboard for wl-paste to read, so a test hands the selection over in a file.
    if helix_base::nested_test_session() {
        if let Some(path) = std::env::var_os("HELIX_WRITING_TEST_SELECTION") {
            return std::fs::read_to_string(path)
                .unwrap_or_default()
                .chars()
                .take(MAX_SOURCE)
                .collect();
        }
    }
    if !wl_paste(&["--primary", "--list-types"]).contains("x-kde-passwordManagerHint") {
        let primary = wl_paste(&["--primary", "--no-newline"]);
        if !primary.trim().is_empty() {
            eprintln!("WRITING from the primary selection");
            return primary.chars().take(MAX_SOURCE).collect();
        }
    }
    let terminal = {
        let state = shell_state(app);
        let shell = state.lock().expect("shell state");
        shell
            .niri
            .focused_window()
            .is_none_or(|w| helix_compositor::niri::looks_like_terminal(&w.app_id))
    };
    if terminal {
        return String::new();
    }
    let before = crate::insert::paste_clipboard_text().unwrap_or_default();
    // A marker, so an unchanged clipboard (nothing selected) is not taken for the selection.
    let marker = "\u{2063}helix-writing";
    let _ = crate::insert::copy_text(marker);
    let _ = Command::new("wtype")
        .args(["-M", "ctrl", "c", "-m", "ctrl"])
        .status();
    let mut copied = String::new();
    for _ in 0..12 {
        std::thread::sleep(Duration::from_millis(40));
        copied = crate::insert::paste_clipboard_text().unwrap_or_default();
        if copied != marker {
            break;
        }
    }
    let _ = crate::insert::copy_text(&before);
    if copied == marker {
        return String::new();
    }
    eprintln!("WRITING from a copy");
    copied.chars().take(MAX_SOURCE).collect()
}

/// Opening Writing Tools: the selection is read first (from a thread: copying waits on the app), then the panel opens on the main thread.
pub fn capture_then_open(app: AppHandle) {
    std::thread::spawn(move || {
        // Opened from another panel (the launcher): wait for it to close, so the keyboard is back in the window with the selection.
        for _ in 0..15 {
            let open = shell_state(&app)
                .lock()
                .expect("shell state")
                .surface
                .open_panel_id()
                .is_some();
            if !open {
                break;
            }
            std::thread::sleep(Duration::from_millis(40));
        }
        std::thread::sleep(Duration::from_millis(120));
        let source = selection(&app);
        *CAPTURED.lock().expect("captured") = source;
        READY.store(true, std::sync::atomic::Ordering::SeqCst);
        let main = app.clone();
        let _ = app.run_on_main_thread(move || crate::open_panel(&main, "writing"));
    });
}

/// Writing Tools on text already in hand (the Magic Pointer's selection), optionally running one tool straight away.
pub fn open_with(app: &AppHandle, source: String, tool: Option<Tool>) {
    *CAPTURED.lock().expect("captured") = source;
    READY.store(true, std::sync::atomic::Ordering::SeqCst);
    crate::open_panel(app, "writing");
    if let Some(tool) = tool {
        let running = app.clone();
        tauri::async_runtime::spawn(async move {
            let _ = writing_run(running, tool.id().to_string(), None).await;
        });
    }
}

/// Called as the panel opens: what it will work on.
pub fn opened(app: &AppHandle) {
    let source = std::mem::take(&mut *CAPTURED.lock().expect("captured"));
    eprintln!("WRITING selection {} chars", source.chars().count());
    {
        let state = shell_state(app);
        let mut shell = state.lock().expect("shell state");
        let generation = shell.writing.generation + 1;
        shell.writing = Writing {
            source,
            generation,
            ..Writing::default()
        };
    }
    emit(app);
}

#[tauri::command]
pub fn writing_view(app: AppHandle) -> WritingView {
    view(&shell_state(&app).lock().expect("shell state").writing)
}

/// Runs one tool on the selection and streams the answer into the panel.
#[tauri::command]
pub async fn writing_run(
    app: AppHandle,
    tool: String,
    question: Option<String>,
) -> Result<(), String> {
    let tool = Tool::parse(&tool).ok_or("That is not one of the Writing Tools.")?;
    let (generation, source) = {
        let state = shell_state(&app);
        let mut shell = state.lock().expect("shell state");
        if shell.writing.source.trim().is_empty() {
            return Err("Select some text first.".into());
        }
        shell.writing.generation += 1;
        shell.writing.tool = Some(tool);
        shell.writing.phase = Phase::Working;
        shell.writing.result.clear();
        shell.writing.error.clear();
        (shell.writing.generation, shell.writing.source.clone())
    };
    eprintln!("WRITING run {}", tool.id());
    emit(&app);
    let outcome = stream(
        &app,
        generation,
        &messages(tool, &source, question.as_deref().unwrap_or_default()),
    )
    .await;
    {
        let state = shell_state(&app);
        let mut shell = state.lock().expect("shell state");
        if shell.writing.generation != generation {
            return Ok(());
        }
        match outcome {
            Ok(()) => {
                shell.writing.result = tidy(&shell.writing.result);
                shell.writing.phase = Phase::Done;
            }
            Err(why) => {
                shell.writing.phase = Phase::Failed;
                shell.writing.error = why;
            }
        }
        eprintln!(
            "WRITING {}",
            if shell.writing.phase == Phase::Done {
                "done"
            } else {
                "failed"
            }
        );
    }
    emit(&app);
    Ok(())
}

/// A piece of a streamed answer: more text, or (at the end) the whole of it.
pub(crate) enum Piece {
    More(String),
    Whole(String),
}

/// Streams one model call (`Inference1.Action("chat", args)`) into `take`, which returns false when the answer is no longer wanted
/// (the call is then cancelled). The completed message's whole text comes last; the stream's last pieces can arrive after it.
pub(crate) async fn stream_chat(
    args: std::collections::HashMap<&'static str, zbus::zvariant::Value<'static>>,
    mut take: impl FnMut(Piece) -> bool,
) -> Result<(), String> {
    let proxy = crate::mobius_talk::inference_proxy()
        .await
        .map_err(|why| format!("The model service is not running ({why})."))?;
    let deltas = proxy
        .receive_signal("Delta")
        .await
        .map_err(|e| e.to_string())?;
    let done = proxy
        .receive_signal("Completed")
        .await
        .map_err(|e| e.to_string())?;
    let operation: u64 = proxy.call("Action", &("chat", args)).await.map_err(|_| {
        "The model service is not running. It starts when you use it; try again in a moment."
            .to_string()
    })?;
    enum Ev {
        Delta(zbus::Message),
        Done(zbus::Message),
    }
    let mut events = futures_util::stream::select(deltas.map(Ev::Delta), done.map(Ev::Done));
    let deadline = tokio::time::sleep(Duration::from_secs(180));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            _ = &mut deadline => return Err("The model took too long.".into()),
            event = events.next() => match event {
                None => return Err("The answer ended unexpectedly.".into()),
                Some(Ev::Delta(message)) => {
                    if let Ok((op, chunk)) = message.body().deserialize::<(u64, String)>() {
                        if op != operation { continue; }
                        if !take(Piece::More(chunk)) {
                            let _ = proxy.call_method("Cancel", &(operation,)).await;
                            return Ok(());
                        }
                    }
                }
                Some(Ev::Done(message)) => {
                    if let Ok((op, ok, result)) = message.body().deserialize::<(u64, bool, String)>() {
                        if op != operation { continue; }
                        let value: serde_json::Value = serde_json::from_str(&result).unwrap_or_default();
                        if !ok {
                            return Err(value.get("error").and_then(|e| e.as_str()).unwrap_or("The model could not answer.").to_string());
                        }
                        if let Some(text) = value.get("text").and_then(|t| t.as_str()).filter(|t| !t.is_empty()) {
                            take(Piece::Whole(text.to_string()));
                        }
                        return Ok(());
                    }
                }
            }
        }
    }
}

async fn stream(
    app: &AppHandle,
    generation: u64,
    messages: &[(String, String)],
) -> Result<(), String> {
    let args = crate::mobius_talk::chat_args_for(messages, "summarize")?;
    stream_chat(args, |piece| {
        let current = {
            let state = shell_state(app);
            let mut shell = state.lock().expect("shell state");
            let current = shell.writing.generation == generation;
            if current {
                match piece {
                    Piece::More(chunk) => shell.writing.result.push_str(&chunk),
                    Piece::Whole(text) => shell.writing.result = text,
                }
            }
            current
        };
        if current {
            emit(app);
        }
        current
    })
    .await
}

/// Replace: the panel closes, the keyboard goes back to the window that had it, and the rewrite is pasted over the selection.
#[tauri::command]
pub fn writing_replace(app: AppHandle) -> Result<(), String> {
    let text = {
        let state = shell_state(&app);
        let shell = state.lock().expect("shell state");
        let w = &shell.writing;
        if !(w.tool.is_some_and(Tool::replaces) && w.phase == Phase::Done && !w.result.is_empty()) {
            return Err("Nothing to replace with yet.".into());
        }
        w.result.clone()
    };
    eprintln!("WRITING replace {} chars", text.chars().count());
    crate::close_panel(&app);
    // The panel was used with the pointer, so give the keyboard back to the window with the selection rather than wait for it to return.
    if let Some(target) = shell_state(&app).lock().expect("shell state").insert_target {
        let _ = crate::dock::focus_window(target);
    }
    // Pasted over the selection; typed in the nested test compositor, which has no clipboard to paste from.
    crate::insert::deliver(&app, text, !helix_base::nested_test_session());
    Ok(())
}

/// Copy: the answer goes on the clipboard.
#[tauri::command]
pub fn writing_copy(app: AppHandle) -> Result<(), String> {
    let text = shell_state(&app)
        .lock()
        .expect("shell state")
        .writing
        .result
        .clone();
    if text.is_empty() {
        return Err("Nothing to copy yet.".into());
    }
    crate::insert::copy_text(&text)?;
    eprintln!("WRITING copied");
    Ok(())
}
