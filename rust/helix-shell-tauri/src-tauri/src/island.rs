//! The live-status pill in the middle of the bar. What it shows is decided by `helix_shell_core::island`; this file gathers the inputs
//! (agents waiting on the person, the assistant's phase, what is playing) and tells the page when the answer changes. What is playing comes
//! from `playerctl --follow`, one long-lived process that sleeps until a player says something; nothing here polls.

use std::io::{BufRead, BufReader};
use std::process::{Command, Stdio};
use std::sync::Mutex;
use std::time::Duration;

use helix_shell_core::island::{choose, parse_media_line, IslandView};
use tauri::{AppHandle, Emitter};

use crate::{phase_name, shell_state};

static LAST: Mutex<Option<IslandView>> = Mutex::new(None);

fn compute(app: &AppHandle) -> IslandView {
    let state = shell_state(app);
    let shell = state.lock().expect("shell state");
    let activity = shell.activity.view();
    choose(
        activity.pending.len(),
        activity.enabled,
        phase_name(shell.session.view().phase),
        shell.media.as_ref(),
    )
}

/// Recomputes the pill and tells the page if it changed.
pub fn publish(app: &AppHandle) {
    let view = compute(app);
    let mut last = LAST.lock().expect("island");
    if last.as_ref() != Some(&view) {
        eprintln!("ISLAND {:?}", view.kind);
        *last = Some(view.clone());
        let _ = app.emit("island-view", view);
    }
}

#[tauri::command]
pub fn island_view(app: AppHandle) -> IslandView {
    compute(&app)
}

/// Follows what is playing. If `playerctl` is missing the pill simply never shows music.
pub fn watch_media(app: AppHandle) {
    std::thread::spawn(move || loop {
        let format = "{{status}}\t{{artist}}\t{{title}}";
        let spawned = Command::new("playerctl")
            .args(["--follow", "metadata", "--format", format])
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn();
        let Ok(mut child) = spawned else {
            eprintln!("ISLAND no playerctl: no music in the pill");
            return;
        };
        if let Some(out) = child.stdout.take() {
            for line in BufReader::new(out).lines().map_while(Result::ok) {
                shell_state(&app).lock().expect("shell state").media = parse_media_line(&line);
                publish(&app);
            }
        }
        let _ = child.wait();
        shell_state(&app).lock().expect("shell state").media = None;
        publish(&app);
        std::thread::sleep(Duration::from_secs(10));
    });
}

/// Play or pause whatever is playing, as clicking the music in the pill does.
#[tauri::command]
pub fn media_toggle() {
    let _ = Command::new("playerctl")
        .arg("play-pause")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status();
}
