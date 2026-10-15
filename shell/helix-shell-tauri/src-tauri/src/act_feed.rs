//! `org.helix.Shell.Act1` (protocols/dbus/org.helix.Shell.Act.xml): what the desktop assistant is doing right now.
//!
//! An app that hands a request to the assistant (Mobius) used to learn nothing afterwards, so the person was told to go and look at the
//! bar. This feed lets that app show the task where it was asked: working, waiting for approval, done, or why it could not. It is
//! read-only apart from `Stop`, which only cancels. Approving is never possible through it; that stays in the shell's approval surface.
//!
//! Served by the process that holds the assistant session (the monolith, or `panels` in the split shell), never by the bar.

use std::collections::HashMap;
use std::sync::{Mutex, OnceLock};

use tauri::AppHandle;
use zbus::zvariant::{OwnedValue, Value};

use crate::AssistantView;

pub const BUS_NAME: &str = "org.helix.Shell.Act";
pub const OBJECT_PATH: &str = "/org/helix/Shell/Act";
const INTERFACE: &str = "org.helix.Shell.Act1";
/// The methods and signals this object serves. A test checks them against the XML contract.
#[cfg(test)]
const METHODS: &[&str] = &["GetState", "Stop"];
#[cfg(test)]
const SIGNALS: &[&str] = &["Changed"];

#[derive(Clone, Debug, Default, PartialEq, Eq)]
struct State {
    task: u64,
    phase: String,
    request: String,
    text: String,
    description: String,
    reason: String,
    failed: bool,
}

static STATE: Mutex<State> = Mutex::new(State {
    task: 0,
    phase: String::new(),
    request: String::new(),
    text: String::new(),
    description: String::new(),
    reason: String::new(),
    failed: false,
});
static CONNECTION: OnceLock<zbus::Connection> = OnceLock::new();

impl State {
    fn begin(&mut self, request: &str) {
        *self = State {
            task: self.task + 1,
            phase: "thinking".into(),
            request: request.to_string(),
            ..State::default()
        };
    }

    /// Returns whether anything a client can see changed.
    fn follow(&mut self, view: &AssistantView) -> bool {
        let next = State {
            task: self.task,
            phase: view.phase.to_string(),
            request: self.request.clone(),
            text: view.text.clone(),
            description: view.description.clone(),
            reason: view.reason.clone(),
            failed: view.failed,
        };
        let changed = *self != next;
        *self = next;
        changed
    }

    fn to_map(&self) -> HashMap<String, OwnedValue> {
        let mut map = HashMap::new();
        let mut put = |key: &str, value: Value<'_>| {
            if let Ok(owned) = OwnedValue::try_from(value) {
                map.insert(key.to_string(), owned);
            }
        };
        put("task", Value::from(self.task));
        put("phase", Value::from(self.phase.clone()));
        put("request", Value::from(self.request.clone()));
        put("text", Value::from(self.text.clone()));
        put("description", Value::from(self.description.clone()));
        put("reason", Value::from(self.reason.clone()));
        put("failed", Value::from(self.failed));
        map
    }
}

/// A new request was handed to the assistant: it is a new task.
pub(crate) fn note_request(request: &str) {
    STATE.lock().expect("act state").begin(request);
}

/// The assistant's view changed (every `emit_view`). Tells clients only when something they can see differs.
pub(crate) fn publish(view: &AssistantView) {
    let changed = STATE.lock().expect("act state").follow(view);
    if !changed {
        return;
    }
    let Some(connection) = CONNECTION.get().cloned() else {
        return;
    };
    tauri::async_runtime::spawn(async move {
        let _ = connection
            .emit_signal(None::<&str>, OBJECT_PATH, INTERFACE, "Changed", &())
            .await;
    });
}

struct Act {
    app: AppHandle,
}

#[zbus::interface(name = "org.helix.Shell.Act1")]
impl Act {
    async fn get_state(&self) -> HashMap<String, OwnedValue> {
        STATE.lock().expect("act state").to_map()
    }

    async fn stop(&self) -> zbus::fdo::Result<()> {
        crate::control::on_main(&self.app, |app| {
            let _ = crate::assistant_cancel(app.clone());
        })
    }
}

/// Claims the name for as long as the shell runs. A shell that only draws the bar never calls this.
pub fn serve(app: AppHandle) {
    tauri::async_runtime::spawn(async move {
        let built = zbus::connection::Builder::session()
            .and_then(|builder| builder.name(BUS_NAME))
            .and_then(|builder| builder.serve_at(OBJECT_PATH, Act { app }));
        let connection = match built {
            Ok(builder) => builder.build().await,
            Err(error) => Err(error),
        };
        match connection {
            Ok(connection) => {
                eprintln!("ACT serving {BUS_NAME}");
                let _ = CONNECTION.set(connection);
                std::future::pending::<()>().await;
            }
            Err(error) => eprintln!("ACT could not serve {BUS_NAME}: {error}"),
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn view(phase: &'static str, text: &str, description: &str, failed: bool) -> AssistantView {
        AssistantView {
            phase,
            text: text.into(),
            description: description.into(),
            reason: String::new(),
            failed,
            unverified: false,
            armed: false,
        }
    }

    #[test]
    fn the_served_interface_is_exactly_the_contract() {
        let interfaces = helix_desktop_api::parse_interfaces(include_str!(
            "../../../../protocols/dbus/org.helix.Shell.Act.xml"
        ));
        let contract = &interfaces[INTERFACE];
        let mut served: Vec<&str> = METHODS.to_vec();
        let mut declared: Vec<&str> = contract.methods.iter().map(String::as_str).collect();
        served.sort_unstable();
        declared.sort_unstable();
        assert_eq!(served, declared);
        assert_eq!(
            SIGNALS.to_vec(),
            contract.signals.iter().map(String::as_str).collect::<Vec<_>>()
        );
    }

    #[test]
    fn a_task_goes_from_asked_to_done_and_each_new_request_is_a_new_task() {
        let mut state = State::default();
        state.begin("Turn the popups off");
        assert_eq!((state.task, state.phase.as_str()), (1, "thinking"));
        assert!(state.follow(&view("confirm", "", "Turn the popups off", false)));
        assert_eq!(state.request, "Turn the popups off", "the request survives view updates");
        assert!(state.follow(&view("result", "Done", "Turn the popups off", false)));
        assert!(!state.follow(&view("result", "Done", "Turn the popups off", false)), "no change, no signal");
        state.begin("Dim the screen");
        assert_eq!((state.task, state.phase.as_str(), state.text.as_str()), (2, "thinking", ""));
    }

    #[test]
    fn the_state_map_carries_every_documented_key() {
        let mut state = State::default();
        state.begin("x");
        let map = state.to_map();
        for key in ["task", "phase", "request", "text", "description", "reason", "failed"] {
            assert!(map.contains_key(key), "{key}");
        }
    }
}
