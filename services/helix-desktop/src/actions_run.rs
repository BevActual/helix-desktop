//! The actions catalogue beyond Settings (`docs/design/actions.md`): the built-in Models actions, actions other apps add with manifests
//! (called over `org.helix.ActionProvider1`), the tier each call needs, and Undo for all of them.

use std::sync::Arc;

use helix_actions::manifest::{self, Manifest};
use helix_actions::{models, settings, Actor, Risk};
use helix_agent::risk::Tier as RiskTier;
use serde_json::{json, Value};
use tokio::sync::Mutex;
use zbus::zvariant::OwnedValue;

use crate::state::State;

const INFERENCE_BUS: &str = "org.helix.Inference";
const INFERENCE_PATH: &str = "/org/helix/Inference";
const INFERENCE_IFACE: &str = "org.helix.Inference1";
const PROVIDER_IFACE: &str = "org.helix.ActionProvider1";

/// The manifests in the data directories, read fresh (they are small, and an app installed a moment ago should be there).
pub fn manifests() -> Vec<Manifest> {
    let (user, system) = manifest::data_dirs();
    manifest::load(&user, &system)
}

fn tier_of(risk: Risk) -> RiskTier {
    match risk {
        Risk::Observe => RiskTier::Observe,
        Risk::ObservePrivate => RiskTier::ObservePrivate,
        Risk::Act => RiskTier::Act,
        Risk::Confirm => RiskTier::Confirm,
        Risk::Blocked => RiskTier::Blocked,
    }
}

fn text<'a>(params: &'a Value, key: &str) -> &'a str {
    params.get(key).and_then(Value::as_str).unwrap_or_default()
}

/// The tier this exact call needs, the sentence a confirmation shows for it, and whether the action exists at all.
pub fn tier(
    action: &str,
    params: &Value,
    rows: &[Value],
    manifests: &[Manifest],
) -> (RiskTier, String, bool) {
    let setting = text(params, "setting");
    let row = rows
        .iter()
        .find(|row| row.get("id").and_then(Value::as_str) == Some(setting));
    let (risk, preview) = match action {
        settings::FIND => (
            Risk::Observe,
            format!("Find settings: {}", text(params, "query")),
        ),
        settings::GET => (settings::risk_to_read(setting), format!("Read {setting}")),
        settings::SET => {
            let value = params.get("value").cloned().unwrap_or(Value::Null);
            let preview = match row {
                Some(row) => {
                    settings::sentence(row, row.get("value").unwrap_or(&Value::Null), &value)
                }
                None => format!("Change {setting}"),
            };
            (settings::risk_to_change(setting), preview)
        }
        models::LIST => (Risk::Observe, "List models".into()),
        models::UNLOAD => (Risk::Act, "Free model memory".into()),
        models::USE => {
            let task = text(params, "task");
            // Acting on the desktop is the most sensitive task: choosing its model is the person's call.
            let risk = if task == "act" {
                Risk::Confirm
            } else {
                Risk::Act
            };
            let before = helix_config::inference_routing::load().route(task).model;
            (risk, models::sentence(task, &before, text(params, "model")))
        }
        models::DOWNLOAD => (
            Risk::Confirm,
            format!(
                "Download {} from {}",
                text(params, "file"),
                text(params, "repo")
            ),
        ),
        other => match manifest::find(manifests, other) {
            Some((provider, found)) => (found.risk, format!("{}: {}", provider.title, found.title)),
            None => return (RiskTier::Blocked, other.to_string(), false),
        },
    };
    (tier_of(risk), preview, true)
}

fn now_seconds() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_secs())
}

async fn inference(connection: &zbus::Connection) -> Result<zbus::Proxy<'static>, String> {
    zbus::Proxy::new(connection, INFERENCE_BUS, INFERENCE_PATH, INFERENCE_IFACE)
        .await
        .map_err(|e| e.to_string())
}

/// The models the model service has installed (empty when it is not running).
async fn installed(connection: &zbus::Connection) -> Vec<Value> {
    let Ok(proxy) = inference(connection).await else {
        return Vec::new();
    };
    let Ok(rows): Result<Vec<OwnedValue>, _> = proxy.call("ListModels", &()).await else {
        return Vec::new();
    };
    rows.iter()
        .filter_map(crate::dbus_value::owned_to_json)
        .collect()
}

/// Sets the model of `task` in the routing file and returns what it was.
fn set_task_model(task: &str, model: &str) -> Result<String, String> {
    let mut routing = helix_config::inference_routing::load();
    let mut route = routing.route(task);
    let before = std::mem::replace(&mut route.model, model.trim().to_string());
    routing.profiles.insert(task.to_string(), route);
    helix_config::inference_routing::save(&routing)?;
    Ok(before)
}

/// Runs a built-in Models action. Returns the reply and whether something changed.
pub async fn run_models(
    state: &Arc<Mutex<State>>,
    connection: Option<&zbus::Connection>,
    action: &str,
    params: &Value,
    by: &Actor,
) -> (Result<Value, String>, bool) {
    match action {
        models::LIST => {
            let routing = helix_config::inference_routing::load();
            let tasks: Vec<Value> = models::TASKS
                .iter()
                .map(|(id, title)| {
                    let route = routing.route(id);
                    json!({ "task": id, "title": title, "backend": route.backend, "model": route.model })
                })
                .collect();
            let installed = match connection {
                Some(conn) => installed(conn).await,
                None => Vec::new(),
            };
            (Ok(json!({ "tasks": tasks, "installed": installed })), false)
        }
        models::USE => {
            let (task, model) = (text(params, "task"), text(params, "model"));
            if models::task_title(task).is_none() {
                return (
                    Err(format!("\"{task}\" is not a task; use models.list.")),
                    false,
                );
            }
            if !models::model_ok(model) {
                return (Err("That model name is not valid.".into()), false);
            }
            let routing = helix_config::inference_routing::load();
            let local = routing.is_local_route(task);
            if local && !model.is_empty() {
                let names = match connection {
                    Some(conn) => installed(conn).await,
                    None => Vec::new(),
                };
                let known = names
                    .iter()
                    .any(|m| m.get("id").and_then(Value::as_str) == Some(model));
                if !names.is_empty() && !known {
                    return (
                        Err(format!(
                            "\"{model}\" is not installed on this computer; use models.list."
                        )),
                        false,
                    );
                }
            }
            match set_task_model(task, model) {
                Ok(before) => {
                    let mut state = state.lock().await;
                    let entry = state.history.record(
                        now_seconds(),
                        models::USE,
                        &format!("models.{task}"),
                        by,
                        json!(before),
                        json!(model),
                        helix_actions::Undo::Restore,
                        |b, a| {
                            models::sentence(
                                task,
                                b.as_str().unwrap_or_default(),
                                a.as_str().unwrap_or_default(),
                            )
                        },
                    );
                    let sentence = state
                        .history
                        .get(entry)
                        .map(|e| e.sentence.clone())
                        .unwrap_or_default();
                    (Ok(json!({ "done": sentence, "undo": entry })), true)
                }
                Err(why) => (Err(why), false),
            }
        }
        models::UNLOAD => {
            let Some(conn) = connection else {
                return (Err("The model service is not reachable.".into()), false);
            };
            match inference(conn).await {
                Ok(proxy) => match proxy.call::<_, _, bool>("Unload", &()).await {
                    Ok(true) => (Ok(json!({ "done": "Freed model memory" })), false),
                    Ok(false) => (
                        Err("The model is answering; try again when it is done.".into()),
                        false,
                    ),
                    Err(e) => (Err(e.to_string()), false),
                },
                Err(e) => (Err(e), false),
            }
        }
        models::DOWNLOAD => {
            let Some(conn) = connection else {
                return (Err("The model service is not reachable.".into()), false);
            };
            let pictures = params
                .get("pictures")
                .and_then(Value::as_bool)
                .unwrap_or(false);
            match inference(conn).await {
                Ok(proxy) => match proxy
                    .call::<_, _, u64>(
                        "Download",
                        &(text(params, "repo"), text(params, "file"), pictures),
                    )
                    .await
                {
                    Ok(id) => (
                        Ok(
                            json!({ "done": format!("Downloading {}", text(params, "file")), "download": id }),
                        ),
                        false,
                    ),
                    Err(zbus::Error::MethodError(_, Some(why), _)) => (Err(why), false),
                    Err(e) => (Err(e.to_string()), false),
                },
                Err(e) => (Err(e), false),
            }
        }
        other => (Err(format!("unknown action {other}")), false),
    }
}

/// Calls an app's action over `org.helix.ActionProvider1.Invoke(action, params)`; its reply is JSON.
pub async fn invoke_provider(
    connection: &zbus::Connection,
    provider: &manifest::Provider,
    action: &str,
    params: &Value,
) -> Result<Value, String> {
    let proxy = zbus::Proxy::new(
        connection,
        provider.bus.clone(),
        provider.path.clone(),
        PROVIDER_IFACE,
    )
    .await
    .map_err(|e| e.to_string())?;
    let args = (action, params.to_string());
    let call = proxy.call::<_, _, String>("Invoke", &args);
    let reply = tokio::time::timeout(std::time::Duration::from_secs(60), call)
        .await
        .map_err(|_| format!("{} did not answer.", provider.title))?
        .map_err(|e| match e {
            zbus::Error::MethodError(_, Some(why), _) => why,
            other => format!("{} could not be reached: {other}", provider.title),
        })?;
    Ok(serde_json::from_str(&reply).unwrap_or(Value::String(reply)))
}

/// What Undo of a history entry needs to do.
pub enum UndoStep {
    /// Done already (under the lock): the sentence.
    Done(String),
    /// Call an app's action, then mark `entry` undone.
    Call {
        entry: u64,
        provider: manifest::Provider,
        action: String,
        params: Value,
        sentence: String,
    },
}

/// Undoes what can be undone under the lock (a setting, a task's model), or says which app's action to call.
pub fn undo_step(state: &mut State, entry: u64, by: &Actor) -> Result<UndoStep, String> {
    let found = match state.history.undo_plan(entry) {
        helix_actions::history::UndoPlan::Restore(found) => found.clone(),
        helix_actions::history::UndoPlan::Refused(why) => return Err(why),
    };
    if let Some(call) = &found.undo_call {
        let action = call
            .get("action")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_string();
        let params = call.get("params").cloned().unwrap_or_else(|| json!({}));
        let manifests = manifests();
        let (provider, _) = manifest::find(&manifests, &action)
            .ok_or("The app that did this is no longer installed.")?;
        return Ok(UndoStep::Call {
            entry,
            provider: provider.clone(),
            action,
            params,
            sentence: found.sentence,
        });
    }
    match found.action.as_str() {
        settings::SET => {
            crate::configuration::apply_setting(state, &found.target, found.before.clone(), by)?;
        }
        models::USE => {
            let task = found.target.trim_start_matches("models.");
            let back = found.before.as_str().unwrap_or_default();
            let now = set_task_model(task, back)?;
            state.history.record(
                now_seconds(),
                models::USE,
                &found.target,
                by,
                json!(now),
                json!(back),
                helix_actions::Undo::Restore,
                |b, a| {
                    models::sentence(
                        task,
                        b.as_str().unwrap_or_default(),
                        a.as_str().unwrap_or_default(),
                    )
                },
            );
        }
        other => return Err(format!("{other} cannot be undone here.")),
    }
    state.history.mark_undone(entry, by);
    Ok(UndoStep::Done(format!("Undid {}", found.sentence)))
}

/// Undo of any history entry, for the person (Settings, the bar, the command line).
pub async fn undo(state: &Arc<Mutex<State>>, entry: u64, by: &Actor) -> Result<String, String> {
    let (step, bus) = {
        let mut guard = state.lock().await;
        let step = undo_step(&mut guard, entry, by)?;
        (step, guard.bus.clone())
    };
    match step {
        UndoStep::Done(sentence) => Ok(sentence),
        UndoStep::Call {
            entry,
            provider,
            action,
            params,
            sentence,
        } => {
            let conn = bus.ok_or("The desktop has no bus connection.")?;
            invoke_provider(&conn, &provider, &action, &params).await?;
            let mut guard = state.lock().await;
            guard.history.mark_undone(entry, by);
            guard.history.record_call(
                now_seconds(),
                &action,
                by,
                &format!("Undid {sentence}"),
                None,
            );
            Ok(format!("Undid {sentence}"))
        }
    }
}
