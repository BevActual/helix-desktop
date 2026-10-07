//! The Caps Lock palette (`quick-insert`): what it searches comes from `helix_shell_core::quick_insert`; this file carries out what
//! a chosen row means. Text is typed or pasted into whichever window had the keyboard before the palette opened (the palette closes
//! first, so focus returns there), and the commands are fixed programs with fixed arguments: nothing from the clipboard or an emoji
//! name is ever interpreted by a shell. The clipboard history lives in memory only and skips anything a password manager marks.

use std::io::{BufRead, BufReader, Write};
use std::process::{Command, Stdio};
use std::time::Duration;

use helix_shell_core::quick_insert::{decode_clip_hex, Action, Outcome, Row, Source};
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
        Outcome::Type(text) => later(move || type_text(&text)),
        Outcome::Paste(text) => later(move || paste_text(&text)),
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

fn later(work: impl FnOnce() + Send + 'static) {
    std::thread::spawn(move || {
        std::thread::sleep(FOCUS_RETURN);
        work();
    });
}

fn type_text(text: &str) {
    let _ = Command::new("wtype").arg("--").arg(text).status();
}

/// The text goes onto the clipboard and Shift+Insert pastes it: a terminal gets a bracketed paste, not keystrokes it could run.
fn paste_text(text: &str) {
    let Ok(mut copy) = Command::new("wl-copy").stdin(Stdio::piped()).spawn() else {
        return;
    };
    if let Some(mut stdin) = copy.stdin.take() {
        let _ = stdin.write_all(text.as_bytes());
    }
    let _ = copy.wait();
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
    }
}

/// `swaylock` takes the session lock (ext-session-lock-v1) and checks the password with PAM; a second request while it runs is ignored.
pub fn lock_session() {
    let running = Command::new("pgrep")
        .args(["-x", "swaylock"])
        .stdout(Stdio::null())
        .status()
        .is_ok_and(|status| status.success());
    if !running {
        let _ = Command::new("swaylock")
            .args(["--daemonize", "--color", "0e1028"])
            .status();
    }
}

#[cfg(test)]
mod tests {
    use super::WATCH_SCRIPT;
    use std::os::unix::fs::PermissionsExt;
    use std::process::{Command, Stdio};

    /// Runs the watcher's script as `wl-paste --watch` would, with a stand-in `wl-paste` that reports `types`, feeding `content` on stdin.
    fn run(types: &str, content: &[u8]) -> String {
        let dir = std::env::temp_dir().join(format!(
            "helix-insert-test-{}-{}",
            std::process::id(),
            types.len()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let fake = dir.join("wl-paste");
        std::fs::write(&fake, format!("#!/bin/sh\nprintf '%s\\n' '{types}'\n")).unwrap();
        std::fs::set_permissions(&fake, std::fs::Permissions::from_mode(0o755)).unwrap();
        let path = format!(
            "{}:{}",
            dir.display(),
            std::env::var("PATH").unwrap_or_default()
        );
        let mut child = Command::new("sh")
            .args(["-c", WATCH_SCRIPT])
            .env("PATH", path)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .spawn()
            .unwrap();
        // The script stops reading after 64 KiB, so a big writer must not be waited for (it gets a broken pipe, as wl-paste would).
        let mut stdin = child.stdin.take().unwrap();
        let content = content.to_vec();
        let writer = std::thread::spawn(move || {
            let _ = std::io::Write::write_all(&mut stdin, &content);
        });
        let out = child.wait_with_output().unwrap();
        let _ = writer.join();
        let _ = std::fs::remove_dir_all(&dir);
        String::from_utf8(out.stdout).unwrap()
    }

    #[test]
    fn ordinary_text_is_reported_as_one_hex_line() {
        assert_eq!(
            run("text/plain\ntext/plain;charset=utf-8", b"hi\n"),
            "68690a\n"
        );
    }

    #[test]
    fn what_a_password_manager_flags_is_never_reported() {
        assert_eq!(run("text/plain\nx-kde-passwordManagerHint", b"hunter2"), "");
    }

    #[test]
    fn only_the_first_64_kib_plus_one_byte_are_read() {
        let big = vec![b'a'; 200_000];
        let line = run("text/plain", &big);
        assert_eq!(line.trim().len(), 2 * (64 * 1024 + 1));
    }
}
