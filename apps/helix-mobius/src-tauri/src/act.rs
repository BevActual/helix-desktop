//! What the desktop assistant is doing with a request Mobius handed over (`org.helix.Shell.Act1`,
//! protocols/dbus/org.helix.Shell.Act.xml). Mobius used to send the request and say "confirm in the bar"; now it shows the task where it
//! was asked: working, waiting for approval, done, or why it could not. It only reads and can Stop. Approval stays in the shell.

use std::collections::HashMap;
use std::sync::Mutex;
use std::time::Duration;

use futures_util::StreamExt;
use serde::Serialize;
use tauri::{AppHandle, Emitter};
use zbus::zvariant::{OwnedValue, Value};

const BUS: &str = "org.helix.Shell.Act";
const PATH: &str = "/org/helix/Shell/Act";
const IFACE: &str = "org.helix.Shell.Act1";

/// The request this window handed over last, and where in which chat it was asked. Only that task is followed here: another door
/// (Caps Lock) has its own surface.
#[derive(Clone, Default)]
struct Handoff {
    request: String,
    chat: String,
    /// How many messages the chat had at the time; the finished task is shown after that many.
    after: usize,
}

static MINE: Mutex<Handoff> = Mutex::new(Handoff {
    request: String::new(),
    chat: String::new(),
    after: 0,
});
static TRACK: Mutex<Track> = Mutex::new(Track {
    task: 0,
    recorded: 0,
    in_progress: false,
    description: String::new(),
});

pub fn remember(request: &str, chat: &str, after: usize) {
    *MINE.lock().expect("act request") = Handoff {
        request: request.to_string(),
        chat: chat.to_string(),
        after,
    };
    *TRACK.lock().expect("act track") = Track::default();
}

/// What has been seen of the task being followed, to tell when it ended and how.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct Track {
    task: u64,
    /// The task number already written to the chat, so one task is recorded once.
    recorded: u64,
    in_progress: bool,
    /// The last "what will be done" the shell reported, kept for a task that is stopped before it finishes.
    description: String,
}

/// A task that has ended.
#[derive(Debug, PartialEq, Eq)]
pub struct Finished {
    pub task: u64,
    /// `done`, `failed` or `stopped`.
    pub outcome: &'static str,
    pub summary: String,
    pub detail: String,
    pub before: String,
    pub after_text: String,
}

/// Feeds one reported state in. Returns the task when this state is the moment it ended: the shell reached a result, or went back
/// to idle (Stop) while the task was in progress. Anything else, and a task already recorded, returns `None`.
pub fn advance(track: &mut Track, view: &ActView) -> Option<Finished> {
    if !view.mine {
        return None;
    }
    match view.phase.as_str() {
        "thinking" | "confirm" | "acting" => {
            if track.task != view.task {
                track.description.clear();
            }
            track.task = view.task;
            track.in_progress = true;
            if !view.description.is_empty() {
                track.description = view.description.clone();
            }
            None
        }
        "result" => {
            if track.recorded == view.task && view.task != 0 {
                return None;
            }
            track.recorded = view.task;
            track.in_progress = false;
            let summary = if !view.description.is_empty() {
                view.description.clone()
            } else {
                track.description.clone()
            };
            let detail = if !view.text.is_empty() {
                view.text.clone()
            } else {
                view.reason.clone()
            };
            Some(Finished {
                task: view.task,
                outcome: if view.failed { "failed" } else { "done" },
                summary,
                detail,
                before: view.before.clone(),
                after_text: view.after.clone(),
            })
        }
        "idle" if track.in_progress && track.recorded != track.task => {
            track.recorded = track.task;
            track.in_progress = false;
            Some(Finished {
                task: track.task,
                outcome: "stopped",
                summary: track.description.clone(),
                detail: String::new(),
                before: String::new(),
                after_text: String::new(),
            })
        }
        _ => None,
    }
}

#[derive(Clone, Debug, Default, PartialEq, Serialize)]
pub struct ActView {
    /// The assistant answered and this is the task Mobius handed over.
    pub mine: bool,
    pub task: u64,
    /// idle, thinking, confirm, acting or result.
    pub phase: String,
    pub request: String,
    pub text: String,
    pub description: String,
    pub reason: String,
    pub failed: bool,
    /// Display form of the previous value. Empty when the shell did not send one.
    pub before: String,
    /// Display form of the value after the write.
    pub after: String,
    /// The shell still holds this task and the live value still matches.
    pub undoable: bool,
    /// Undo already wrote the previous value back.
    pub restored: bool,
}

fn text(map: &HashMap<String, OwnedValue>, key: &str) -> String {
    match map.get(key).map(|value| &**value) {
        Some(Value::Str(text)) => text.to_string(),
        _ => String::new(),
    }
}

/// Reads the shell's answer. Anything missing is simply empty, so an older or newer shell never breaks the page.
pub fn view_from(map: &HashMap<String, OwnedValue>, mine: &str) -> ActView {
    let request = text(map, "request");
    let task = match map.get("task").map(|value| &**value) {
        Some(Value::U64(task)) => *task,
        _ => 0,
    };
    let failed = matches!(
        map.get("failed").map(|value| &**value),
        Some(Value::Bool(true))
    );
    let undoable = matches!(
        map.get("undoable").map(|value| &**value),
        Some(Value::Bool(true))
    );
    let restored = matches!(
        map.get("restored").map(|value| &**value),
        Some(Value::Bool(true))
    );
    ActView {
        mine: !mine.is_empty() && request == mine,
        task,
        phase: text(map, "phase"),
        request,
        text: text(map, "text"),
        description: text(map, "description"),
        reason: text(map, "reason"),
        failed,
        before: text(map, "before"),
        after: text(map, "after"),
        undoable,
        restored,
    }
}

async fn proxy() -> zbus::Result<zbus::Proxy<'static>> {
    let connection = crate::connection().await?;
    zbus::Proxy::new(&connection, BUS, PATH, IFACE).await
}

async fn read() -> ActView {
    let Ok(proxy) = proxy().await else {
        return ActView::default();
    };
    let state: zbus::Result<HashMap<String, OwnedValue>> = proxy.call("GetState", &()).await;
    match state {
        Ok(map) => view_from(&map, &MINE.lock().expect("act request").request),
        Err(_) => ActView::default(),
    }
}

/// Follows the shell's `Changed` signal for as long as Mobius runs, and tells the page. A shell that restarts is picked up again.
pub fn watch(app: AppHandle) {
    tauri::async_runtime::spawn(async move {
        loop {
            if let Ok(proxy) = proxy().await {
                if let Ok(mut changes) = proxy.receive_signal("Changed").await {
                    while changes.next().await.is_some() {
                        let view = read().await;
                        let (finished, restored_here) = {
                            let mut track = TRACK.lock().expect("act track");
                            let finished = advance(&mut track, &view);
                            let restored_here =
                                view.restored && view.mine && track.recorded == view.task;
                            (finished, restored_here)
                        };
                        let _ = app.emit("act-state", &view);
                        if let Some(done) = finished {
                            let handoff = MINE.lock().expect("act request").clone();
                            crate::record_desktop_task(
                                &app,
                                &handoff.chat,
                                helix_chat::chat::DesktopTask {
                                    id: done.task,
                                    after: handoff.after,
                                    request: handoff.request,
                                    outcome: done.outcome.to_string(),
                                    summary: done.summary,
                                    detail: done.detail,
                                    before: done.before,
                                    after_text: done.after_text,
                                    undone: false,
                                    at: 0,
                                },
                            );
                        }
                        if restored_here {
                            crate::mark_open_task_undone(&app, view.task);
                        }
                    }
                }
            }
            tokio::time::sleep(Duration::from_secs(2)).await;
        }
    });
}

#[tauri::command]
pub async fn chat_act_state() -> ActView {
    read().await
}

/// Cancels the task that is thinking, waiting for approval or running. It cannot approve anything.
#[tauri::command]
pub async fn chat_act_stop() -> Result<(), String> {
    let proxy = proxy().await.map_err(|error| error.to_string())?;
    proxy
        .call_method("Stop", &())
        .await
        .map(|_| ())
        .map_err(|error| error.to_string())
}

/// Writes the previous value back for a task the shell still holds. The shell refuses when it does not, and nothing is written.
#[tauri::command]
pub async fn chat_act_undo(app: tauri::AppHandle, task: u64) -> Result<(), String> {
    let proxy = proxy().await.map_err(|error| error.to_string())?;
    proxy
        .call_method("Undo", &(task,))
        .await
        .map_err(|error| error.to_string())?;
    crate::mark_open_task_undone(&app, task);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn map(pairs: &[(&str, Value<'static>)]) -> HashMap<String, OwnedValue> {
        pairs
            .iter()
            .map(|(key, value)| {
                (
                    (*key).to_string(),
                    OwnedValue::try_from(value.clone()).expect("owned"),
                )
            })
            .collect()
    }

    #[test]
    fn only_the_task_this_window_handed_over_is_mine() {
        let state = map(&[
            ("task", Value::from(3u64)),
            ("phase", Value::from("confirm")),
            ("request", Value::from("Dim the screen")),
            ("description", Value::from("Make the screen dimmer")),
        ]);
        let view = view_from(&state, "Dim the screen");
        assert!(view.mine);
        assert_eq!(
            (view.task, view.phase.as_str(), view.description.as_str()),
            (3, "confirm", "Make the screen dimmer")
        );
        assert!(!view_from(&state, "Something else").mine);
        assert!(!view_from(&state, "").mine, "nothing handed over yet");
    }

    #[test]
    fn a_missing_or_odd_answer_is_an_idle_view_not_an_error() {
        let view = view_from(&HashMap::new(), "x");
        assert_eq!(view, ActView::default());
        let odd = map(&[("phase", Value::from(7u32)), ("failed", Value::from("yes"))]);
        let view = view_from(&odd, "x");
        assert_eq!((view.phase.as_str(), view.failed), ("", false));
        assert!(view.before.is_empty() && view.after.is_empty() && !view.undoable);
    }

    #[test]
    fn a_pair_is_read_and_a_missing_one_is_not_undoable() {
        let state = map(&[
            ("before", Value::from("40%, muted")),
            ("after", Value::from("45%")),
            ("undoable", Value::from(true)),
        ]);
        let view = view_from(&state, "");
        assert_eq!(
            (view.before.as_str(), view.after.as_str(), view.undoable),
            ("40%, muted", "45%", true)
        );
        assert!(!view_from(&HashMap::new(), "").undoable);
        assert!(!view_from(&HashMap::new(), "").restored);
    }

    #[test]
    fn a_failure_is_carried_through() {
        let state = map(&[
            ("phase", Value::from("result")),
            ("request", Value::from("x")),
            ("text", Value::from("Could not reach the service")),
            ("failed", Value::from(true)),
        ]);
        let view = view_from(&state, "x");
        assert!(view.failed && view.mine);
        assert_eq!(view.text, "Could not reach the service");
    }

    fn state(phase: &str, task: u64) -> ActView {
        ActView {
            mine: true,
            task,
            phase: phase.into(),
            request: "Dim the screen".into(),
            ..ActView::default()
        }
    }

    #[test]
    fn a_task_is_recorded_once_when_it_reaches_a_result() {
        let mut track = Track::default();
        assert_eq!(advance(&mut track, &state("thinking", 1)), None);
        let mut confirm = state("confirm", 1);
        confirm.description = "Make the screen dimmer".into();
        assert_eq!(advance(&mut track, &confirm), None);
        assert_eq!(advance(&mut track, &state("acting", 1)), None);
        let mut result = state("result", 1);
        result.text = "Done. The screen is dimmer.".into();
        let done = advance(&mut track, &result).expect("finished");
        assert_eq!((done.task, done.outcome), (1, "done"));
        assert_eq!(
            done.summary, "Make the screen dimmer",
            "what was confirmed, though the result has no description"
        );
        assert_eq!(done.detail, "Done. The screen is dimmer.");
        assert_eq!(
            advance(&mut track, &result),
            None,
            "the same result again records nothing"
        );
        assert_eq!(
            advance(&mut track, &state("idle", 1)),
            None,
            "dismissing it afterwards records nothing"
        );
    }

    #[test]
    fn a_failure_carries_the_reason() {
        let mut track = Track::default();
        advance(&mut track, &state("acting", 4));
        let mut failed = state("result", 4);
        failed.failed = true;
        failed.reason = "no such setting".into();
        let done = advance(&mut track, &failed).unwrap();
        assert_eq!(
            (done.outcome, done.detail.as_str()),
            ("failed", "no such setting")
        );
    }

    #[test]
    fn stopping_a_task_in_progress_is_recorded_as_stopped() {
        let mut track = Track::default();
        let mut confirm = state("confirm", 2);
        confirm.description = "Make the screen dimmer".into();
        advance(&mut track, &confirm);
        let done = advance(&mut track, &state("idle", 2)).expect("stopped");
        assert_eq!(
            (done.outcome, done.summary.as_str(), done.detail.as_str()),
            ("stopped", "Make the screen dimmer", "")
        );
        assert_eq!(advance(&mut track, &state("idle", 2)), None);
    }

    #[test]
    fn nothing_is_recorded_for_a_task_this_window_did_not_hand_over() {
        let mut track = Track::default();
        let mut other = state("result", 9);
        other.mine = false;
        assert_eq!(advance(&mut track, &other), None);
        let mut idle = state("idle", 0);
        idle.mine = false;
        assert_eq!(advance(&mut track, &idle), None);
    }

    #[test]
    fn a_new_hand_over_starts_clean_even_if_the_shell_restarted_its_counter() {
        let mut track = Track::default();
        advance(&mut track, &state("acting", 1));
        advance(&mut track, &state("result", 1));
        // The shell restarted: its counter is 1 again for the next request, which remember() resets for.
        track = Track::default();
        advance(&mut track, &state("acting", 1));
        assert!(advance(&mut track, &state("result", 1)).is_some());
    }
}
