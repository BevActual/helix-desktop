//! The Caps Lock palette (`quick-insert`): what it searches comes from `helix_shell_model::quick_insert`; this file carries out what
//! a chosen row means. Text is typed or pasted into whichever window had the keyboard before the palette opened (the palette closes
//! first, so focus returns there), and the commands are fixed programs with fixed arguments: nothing from the clipboard or an emoji
//! name is ever interpreted by a shell. The clipboard history lives in memory only and skips anything a password manager marks.

use std::io::{BufRead, BufReader, Write};
use std::process::{Command, Stdio};
use std::time::Duration;

use helix_shell_model::quick_insert::{decode_clip_hex, needs_care, Action, Outcome, Row, Source};
use tauri::AppHandle;

use crate::{close_panel, open_panel, shell_state};

/// Run by `wl-paste --watch` for every change of the clipboard text, with the content on stdin. It prints nothing for content a password
/// manager flags, and otherwise the first 64 KiB (plus one byte, so oversized content can be told apart) as one line of hex.
const WATCH_SCRIPT: &str = r#"case "$(wl-paste --list-types 2>/dev/null)" in *x-kde-passwordManagerHint*) exit 0;; esac; head -c 65537 | od -An -v -tx1 | tr -d ' \n'; echo"#;

/// How long to wait after the palette closes for the keyboard to return to the window underneath.
const FOCUS_RETURN: Duration = Duration::from_millis(220);

/// Follows the clipboard for as long as the shell runs. If `wl-paste` is missing or the session has no clipboard, the history is
/// simply empty.
pub fn watch_clipboard(app: AppHandle) {
    std::thread::spawn(move || loop {
        let spawned = Command::new("wl-paste")
            .args(["--type", "text", "--watch", "sh", "-c", WATCH_SCRIPT])
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn();
        let Ok(mut child) = spawned else {
            eprintln!("INSERT clipboard history unavailable (wl-paste)");
            return;
        };
        if let Some(out) = child.stdout.take() {
            for line in BufReader::new(out).lines().map_while(Result::ok) {
                if let Some(text) = decode_clip_hex(&line) {
                    shell_state(&app)
                        .lock()
                        .expect("shell state")
                        .insert
                        .copied(text);
                }
            }
        }
        let _ = child.wait();
        std::thread::sleep(Duration::from_secs(5));
    });
}

fn parse_source(name: &str) -> Result<Source, String> {
    match name {
        "emoji" => Ok(Source::Emoji),
        "clipboard" => Ok(Source::Clipboard),
        "actions" => Ok(Source::Actions),
        other => Err(format!("unknown source {other:?}")),
    }
}

#[derive(serde::Serialize)]
pub struct Results {
    /// Sent back with the chosen position, so a choice is only honoured for the results it was drawn from.
    generation: u64,
    rows: Vec<Row>,
}

#[tauri::command]
pub fn quick_insert_query(app: AppHandle, source: String, text: String) -> Result<Results, String> {
    let source = parse_source(&source)?;
    let state = shell_state(&app);
    let mut shell = state.lock().expect("shell state");
    let rows = shell.insert.query(source, &text);
    Ok(Results {
        generation: shell.insert.generation(),
        rows,
    })
}

#[tauri::command]
pub fn quick_insert_forget_clipboard(app: AppHandle) {
    shell_state(&app)
        .lock()
        .expect("shell state")
        .insert
        .forget_clipboard();
}

/// The person chose the row at `index` of the last query. The palette closes, then the outcome is carried out.
#[tauri::command]
pub fn quick_insert_activate(app: AppHandle, generation: u64, index: usize) -> Result<(), String> {
    let outcome = shell_state(&app)
        .lock()
        .expect("shell state")
        .insert
        .activate(generation, index)
        .map_err(|error| format!("{error:?}"))?;
    eprintln!("INSERT {}", describe(&outcome));
    close_panel(&app);
    match outcome {
        Outcome::Type(text) => deliver(&app, text, false),
        Outcome::Paste(text) => deliver(&app, text, true),
        Outcome::Do(action) => do_action(&app, action),
        Outcome::Ask(question) => {
            open_panel(&app, "assistant");
            crate::submit_request(&app, &question);
        }
    }
    Ok(())
}

/// What the log says: the kind, never the text.
fn describe(outcome: &Outcome) -> String {
    match outcome {
        Outcome::Type(text) => format!("type {} chars", text.chars().count()),
        Outcome::Paste(text) => format!("paste {} chars", text.chars().count()),
        Outcome::Do(action) => format!("do {action:?}"),
        Outcome::Ask(_) => "ask".to_string(),
    }
}

/// Inserts into the window that had the keyboard when the palette opened, and only that one. The palette has closed, so the keyboard is on
/// its way back: wait (briefly, on shell state that niri's events keep current) until that window has it again. If it never does, or the
/// text is a line break away from running as commands in a terminal, nothing is inserted and the person is told.
fn deliver(app: &AppHandle, text: String, paste: bool) {
    let app = app.clone();
    let target = shell_state(&app).lock().expect("shell state").insert_target;
    eprintln!("INSERT target window {target:?}");
    std::thread::spawn(move || {
        let Some(target) = target else {
            crate::show_notice(&app, "Nothing to insert into");
            return;
        };
        let mut back = false;
        for _ in 0..16 {
            std::thread::sleep(Duration::from_millis(40));
            let state = shell_state(&app);
            let shell = state.lock().expect("shell state");
            if shell.niri.focused_window().map(|w| w.id) == Some(target) {
                back = true;
                break;
            }
        }
        eprintln!(
            "INSERT focus returned={back} now={:?}",
            shell_state(&app)
                .lock()
                .expect("shell state")
                .niri
                .focused_window()
                .map(|w| w.id)
        );
        if !back {
            crate::show_notice(&app, "Not inserted: the window changed");
            return;
        }
        let terminal = {
            let state = shell_state(&app);
            let shell = state.lock().expect("shell state");
            shell
                .niri
                .window(target)
                .is_some_and(|w| helix_compositor::niri::looks_like_terminal(&w.app_id))
        };
        if terminal && needs_care(&text) {
            crate::show_notice(&app, "Multi-line text was not pasted into a terminal");
            return;
        }
        std::thread::sleep(FOCUS_RETURN / 2);
        if paste {
            paste_text(&text);
        } else {
            type_text(&text);
        }
    });
}

fn type_text(text: &str) {
    let _ = Command::new("wtype").arg("--").arg(text).status();
}

/// Puts text on the clipboard. Used by Caps Lock paste and by Agent Surface `clipboard-copy`.
pub fn copy_text(text: &str) -> Result<(), String> {
    let mut copy = Command::new("wl-copy")
        .stdin(Stdio::piped())
        .spawn()
        .map_err(|error| format!("wl-copy: {error}"))?;
    if let Some(mut stdin) = copy.stdin.take() {
        stdin
            .write_all(text.as_bytes())
            .map_err(|error| format!("wl-copy: {error}"))?;
    }
    match copy.wait() {
        Ok(status) if status.success() => Ok(()),
        Ok(_) => Err("wl-copy failed".into()),
        Err(error) => Err(format!("wl-copy: {error}")),
    }
}

/// Current clipboard text, or empty when the selection has none / is marked as a password.
pub fn paste_clipboard_text() -> Result<String, String> {
    let output = Command::new("wl-paste")
        .args(["--type", "text", "--no-newline"])
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .output()
        .map_err(|error| format!("wl-paste: {error}"))?;
    if !output.status.success() {
        return Ok(String::new());
    }
    let types = Command::new("wl-paste")
        .args(["--list-types"])
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .output()
        .ok()
        .and_then(|out| String::from_utf8(out.stdout).ok())
        .unwrap_or_default();
    if types.contains("x-kde-passwordManagerHint") {
        return Ok(String::new());
    }
    String::from_utf8(output.stdout).map_err(|_| "clipboard is not text".into())
}

/// Clears the current selection (best effort).
pub fn clear_clipboard_selection() {
    let _ = Command::new("wl-copy").args(["--clear"]).status();
}

/// The text goes onto the clipboard and Shift+Insert pastes it: a terminal gets a bracketed paste, not keystrokes it could run.
fn paste_text(text: &str) {
    if copy_text(text).is_err() {
        return;
    }
    std::thread::sleep(Duration::from_millis(60));
    let _ = Command::new("wtype")
        .args(["-M", "shift", "-k", "Insert", "-m", "shift"])
        .status();
}

fn do_action(app: &AppHandle, action: Action) {
    match action {
        Action::OpenLauncher => open_panel(app, "launcher"),
        Action::OpenControlCenter => open_panel(app, "control-center"),
        Action::OpenNotifications => open_panel(app, "notifications"),
        Action::OpenAgents => open_panel(app, "agents"),
        Action::OpenAssistant => open_panel(app, "assistant"),
        Action::ToggleDoNotDisturb => {
            let app = app.clone();
            tauri::async_runtime::spawn(async move { crate::notify::toggle_dnd(&app).await });
        }
        Action::ToggleNightLight => {
            let app = app.clone();
            tauri::async_runtime::spawn(async move { crate::toggle_night_light(&app).await });
        }
        Action::ToggleDarkTheme => {
            let app = app.clone();
            tauri::async_runtime::spawn(async move { crate::toggle_dark_theme(&app).await });
        }
        Action::LockSession => lock_session(),
        Action::FocusAgentDesk => {
            if crate::dock::agent_desk_focus().is_err() {
                crate::show_notice(app, "Could not show the agent desk");
            }
        }
    }
}

/// `swaylock` takes the session lock (ext-session-lock-v1) and checks the password with PAM. It is always started: a second one while
/// the session is already locked is refused by the compositor and exits, and nothing is inferred from a process name (any program can
/// call itself "swaylock", which would otherwise stop the real lock from ever being taken).
pub fn lock_session() {
    // The Helix lock screen; the system's own only when it is not installed or cannot start (exit 2). Exit 1 means the session is already locked.
    let helix = Command::new("helix-lock").arg("--daemonize").status();
    if !matches!(helix, Ok(status) if status.code() != Some(2)) {
        let _ = Command::new("swaylock")
            .args(["--daemonize", "--color", "0e1028"])
            .status();
    }
}
