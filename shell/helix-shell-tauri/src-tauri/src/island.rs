//! The live-status pill in the middle of the bar. What it shows is decided by `helix_shell_model::island`; this file gathers the inputs
//! (agents waiting on the person, the assistant's phase, what is playing) and tells the page when the answer changes. What is playing comes
//! from `playerctl --follow`, one long-lived process that sleeps until a player says something; nothing here polls.

use std::io::{BufRead, BufReader};
use std::process::{Command, Stdio};
use std::sync::Mutex;
use std::time::Duration;

use helix_compositor::agent_desk::glance_from_niri;
use helix_shell_model::island::{
    choose, parse_media_line, with_agent_display, with_agent_help, with_change, IslandView,
    RecentChange,
};
use tauri::{AppHandle, Emitter};

use crate::{phase_name, shell_state};

static LAST: Mutex<Option<IslandView>> = Mutex::new(None);
static LAST_GLOW: Mutex<bool> = Mutex::new(false);

/// How long the screen edges stay lit after an agent changed something.
pub const GLOW_AFTER_ACT: Duration = Duration::from_secs(4);

/// Whether the screen edges should be lit: the assistant is thinking or acting, or an agent changed something a moment ago.
fn glow(app: &AppHandle) -> (bool, Option<std::time::Instant>) {
    let state = shell_state(app);
    let shell = state.lock().expect("shell state");
    let working = matches!(
        phase_name(shell.session.view().phase),
        "thinking" | "acting"
    );
    let until = shell
        .glow_until
        .filter(|until| std::time::Instant::now() < *until);
    (working || until.is_some(), until)
}

/// Tells the compositor when the light should change, and puts it out again when its time is up.
fn publish_glow(app: &AppHandle) {
    let (on, until) = glow(app);
    {
        let mut last = LAST_GLOW.lock().expect("glow");
        if *last != on {
            *last = on;
            eprintln!("GLOW {}", if on { "on" } else { "off" });
            std::thread::spawn(move || {
                let _ = helix_compositor::compositor_ipc::set_agent_glow(on);
            });
        }
    }
    if let Some(until) = until {
        let app = app.clone();
        tauri::async_runtime::spawn(async move {
            tokio::time::sleep(
                until.saturating_duration_since(std::time::Instant::now())
                    + Duration::from_millis(50),
            )
            .await;
            publish_glow(&app);
        });
    }
}

fn compute(app: &AppHandle) -> IslandView {
    let state = shell_state(app);
    let shell = state.lock().expect("shell state");
    let activity = shell.activity.view();
    let agent_desk = glance_from_niri(&shell.niri);
    let change = shell
        .recent_change
        .as_ref()
        .filter(|(_, until)| std::time::Instant::now() < *until)
        .map(|(change, _)| change.clone());
    let view = choose(
        activity.pending.len(),
        activity.enabled,
        phase_name(shell.session.view().phase),
        agent_desk.as_ref(),
        shell.media.as_ref(),
    );
    drop(shell);
    let view = with_agent_display(view, crate::agent_display::window_count());
    let view = with_change(view, change.as_ref());
    with_agent_help(view, crate::agent_display::help_reason().as_deref())
}

/// Recomputes the pill (and the light at the screen edges) and tells the page if it changed.
pub fn publish(app: &AppHandle) {
    publish_glow(app);
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

/// How long an agent's change stays in the pill.
const CHANGE_SHOWN: Duration = Duration::from_secs(8);

/// Reads the newest entry of the history of actions; an agent's change made just now (and not undone) goes into the pill with Undo for a
/// few seconds. Called when Agent Surface activity changes, which the desktop signals after an agent's change.
pub async fn note_recent_change(app: &AppHandle) {
    let Some(desktop) = crate::REGISTERED.get() else {
        return;
    };
    let Ok(history) = desktop.action_history(1).await else {
        return;
    };
    let Some(entry) = history.as_array().and_then(|rows| rows.first()) else {
        return;
    };
    let id = entry.get("id").and_then(|v| v.as_u64()).unwrap_or(0);
    let by = entry.get("by");
    let agent = by.and_then(|b| b.get("kind")).and_then(|k| k.as_str()) == Some("agent");
    let undone = entry.get("undone_by").is_some_and(|u| !u.is_null());
    let at = entry.get("at").and_then(|v| v.as_u64()).unwrap_or(0);
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_secs());
    {
        let state = shell_state(app);
        let mut shell = state.lock().expect("shell state");
        if id == 0 || id <= shell.seen_change {
            return;
        }
        shell.seen_change = id;
        if !agent || undone || now.saturating_sub(at) > 10 {
            return;
        }
        let change = RecentChange {
            entry: id,
            sentence: entry
                .get("sentence")
                .and_then(|v| v.as_str())
                .unwrap_or_default()
                .to_string(),
            by: by
                .and_then(|b| b.get("label"))
                .and_then(|l| l.as_str())
                .unwrap_or("An agent")
                .to_string(),
        };
        eprintln!("ISLAND change {id}");
        shell.recent_change = Some((change, std::time::Instant::now() + CHANGE_SHOWN));
    }
    publish(app);
    // Leaves on its own once its time is up.
    let app = app.clone();
    tauri::async_runtime::spawn(async move {
        tokio::time::sleep(CHANGE_SHOWN + Duration::from_millis(100)).await;
        publish(&app);
    });
}

/// Undo on the pill: puts back what the agent changed, through the same history the person's Settings use.
#[tauri::command]
pub async fn island_undo(app: AppHandle) -> Result<String, String> {
    let entry = {
        let state = shell_state(&app);
        let mut shell = state.lock().expect("shell state");
        shell.recent_change.take().map(|(change, _)| change.entry)
    };
    publish(&app);
    let entry = entry.ok_or("Nothing to undo.")?;
    let desktop = crate::REGISTERED
        .get()
        .ok_or("this shell is not the registered desktop")?;
    let done = desktop
        .undo_action(entry)
        .await
        .map_err(|e| e.to_string())?;
    eprintln!("ISLAND undid {entry}");
    Ok(done)
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
                emit_media(&app);
            }
        }
        let _ = child.wait();
        shell_state(&app).lock().expect("shell state").media = None;
        publish(&app);
        emit_media(&app);
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

#[tauri::command]
pub fn media_next() {
    let _ = Command::new("playerctl")
        .arg("next")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status();
}

#[tauri::command]
pub fn media_previous() {
    let _ = Command::new("playerctl")
        .arg("previous")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status();
}

#[derive(Clone, serde::Serialize)]
pub struct MediaView {
    pub playing: bool,
    pub artist: String,
    pub title: String,
}

#[tauri::command]
pub fn media_view(app: AppHandle) -> Option<MediaView> {
    shell_state(&app)
        .lock()
        .expect("shell state")
        .media
        .as_ref()
        .map(|m| MediaView {
            playing: m.playing,
            artist: m.artist.clone(),
            title: m.title.clone(),
        })
}

fn emit_media(app: &AppHandle) {
    let view = media_view(app.clone());
    let _ = app.emit("media-view", view);
}
