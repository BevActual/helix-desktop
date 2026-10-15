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

/// The request this window handed over last. Only that task is shown here: another door (Caps Lock) has its own surface.
static MINE: Mutex<String> = Mutex::new(String::new());

pub fn remember(request: &str) {
    *MINE.lock().expect("act request") = request.to_string();
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
    let failed = matches!(map.get("failed").map(|value| &**value), Some(Value::Bool(true)));
    ActView {
        mine: !mine.is_empty() && request == mine,
        task,
        phase: text(map, "phase"),
        request,
        text: text(map, "text"),
        description: text(map, "description"),
        reason: text(map, "reason"),
        failed,
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
        Ok(map) => view_from(&map, &MINE.lock().expect("act request")),
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
                        let _ = app.emit("act-state", read().await);
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
}
