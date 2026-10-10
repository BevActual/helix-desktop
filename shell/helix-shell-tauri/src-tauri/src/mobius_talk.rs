//! Quick Talk from the bar Mobius dropdown (Siri-style). Uses `org.helix.Inference` only —
//! never Agent Surface. Longer chats and remote routes stay in the full Mobius window.

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use futures_util::StreamExt;
use helix_shell_core::apps::chat::SYSTEM_PROMPT;
use helix_shell_core::apps::inference_routing;
use helix_shell_core::display::sanitize_for_display;
use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager};
use zbus::zvariant::{Array, Dict, Signature, Value};

use crate::ShellState;

const BUS_NAME: &str = "org.helix.Inference";
const OBJECT_PATH: &str = "/org/helix/Inference";
const INTERFACE: &str = "org.helix.Inference1";
const MAX_QUESTION: usize = 2000;
const MAX_HISTORY_TURNS: usize = 6;
const MAX_REPLY_CHARS: usize = 8_000;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TalkPhase {
    #[default]
    Idle,
    Thinking,
    Streaming,
    Done,
    Failed,
}

#[derive(Clone, Debug, Default)]
pub struct TalkState {
    pub generation: u64,
    pub operation: Option<u64>,
    pub phase: TalkPhase,
    pub reply: String,
    pub error: String,
    /// Recent turns for short in-bar context: ("user"|"assistant", text).
    pub history: Vec<(String, String)>,
}

#[derive(Clone, Serialize)]
pub struct TalkView {
    pub phase: &'static str,
    pub reply: String,
    pub error: String,
    pub history: Vec<TalkLine>,
}

#[derive(Clone, Serialize)]
pub struct TalkLine {
    pub role: String,
    pub text: String,
}

impl TalkState {
    pub fn view(&self) -> TalkView {
        TalkView {
            phase: match self.phase {
                TalkPhase::Idle => "idle",
                TalkPhase::Thinking => "thinking",
                TalkPhase::Streaming => "streaming",
                TalkPhase::Done => "done",
                TalkPhase::Failed => "failed",
            },
            reply: sanitize_for_display(&self.reply, MAX_REPLY_CHARS),
            error: sanitize_for_display(&self.error, 400),
            history: self
                .history
                .iter()
                .map(|(role, text)| TalkLine {
                    role: role.clone(),
                    text: sanitize_for_display(text, MAX_REPLY_CHARS),
                })
                .collect(),
        }
    }

}

fn shell_state(app: &AppHandle) -> Arc<Mutex<ShellState>> {
    app.state::<Arc<Mutex<ShellState>>>().inner().clone()
}

pub fn emit(app: &AppHandle) {
    let view = shell_state(app)
        .lock()
        .expect("shell state")
        .talk
        .view();
    let _ = app.emit("mobius-talk-view", view);
}

fn chat_args(
    messages: &[(String, String)],
) -> Result<HashMap<&'static str, Value<'static>>, String> {
    let variant = Signature::try_from("v").map_err(|e| e.to_string())?;
    let (string, any) = (
        Signature::try_from("s").map_err(|e| e.to_string())?,
        variant.clone(),
    );
    let mut list = Array::new(&variant);
    for (role, content) in messages {
        let mut entry = Dict::new(&string, &any);
        entry
            .append(
                Value::from("role".to_string()),
                Value::Value(Box::new(Value::from(role.clone()))),
            )
            .map_err(|e| e.to_string())?;
        entry
            .append(
                Value::from("content".to_string()),
                Value::Value(Box::new(Value::from(content.clone()))),
            )
            .map_err(|e| e.to_string())?;
        list.append(Value::Value(Box::new(Value::Dict(entry))))
            .map_err(|e| e.to_string())?;
    }
    let talk = inference_routing::load().talk.clamped();
    let mut args = HashMap::new();
    args.insert("messages", Value::Array(list));
    args.insert("task", Value::from("chat".to_string()));
    args.insert("temperature", Value::F64(talk.temperature));
    args.insert("max_tokens", Value::U32(talk.max_tokens.min(512)));
    args.insert("priority", Value::from("interactive".to_string()));
    Ok(args)
}

async fn inference_proxy() -> Result<zbus::Proxy<'static>, String> {
    let connection = zbus::Connection::session().await.map_err(|e| e.to_string())?;
    zbus::Proxy::new(&connection, BUS_NAME, OBJECT_PATH, INTERFACE)
        .await
        .map_err(|e| e.to_string())
}

/// Starts a quick Talk turn. Local Inference only; for long/remote chat open full Mobius.
pub async fn submit(app: AppHandle, text: String) -> Result<(), String> {
    let question: String = text.trim().chars().take(MAX_QUESTION).collect();
    if question.is_empty() {
        return Err("Write something to ask.".into());
    }

    let (generation, messages) = {
        let state = shell_state(&app);
        let mut shell = state.lock().expect("shell state");
        if matches!(
            shell.talk.phase,
            TalkPhase::Thinking | TalkPhase::Streaming
        ) {
            return Err("Wait for the reply, or stop it.".into());
        }
        shell.talk.generation = shell.talk.generation.saturating_add(1);
        let generation = shell.talk.generation;
        shell.talk.operation = None;
        shell.talk.phase = TalkPhase::Thinking;
        shell.talk.reply.clear();
        shell.talk.error.clear();
        shell.talk.history.push(("user".into(), question.clone()));
        if shell.talk.history.len() > MAX_HISTORY_TURNS * 2 {
            let drop = shell.talk.history.len() - MAX_HISTORY_TURNS * 2;
            shell.talk.history.drain(0..drop);
        }
        let mut messages = vec![("system".into(), SYSTEM_PROMPT.to_string())];
        messages.extend(shell.talk.history.iter().cloned());
        (generation, messages)
    };
    emit(&app);

    let proxy = match inference_proxy().await {
        Ok(p) => p,
        Err(reason) => {
            fail(&app, generation, &format!("Mobius is offline ({reason}). Open full Mobius or try again."));
            return Ok(());
        }
    };
    let args = match chat_args(&messages) {
        Ok(a) => a,
        Err(reason) => {
            fail(&app, generation, &reason);
            return Ok(());
        }
    };
    let operation: u64 = match proxy.call("Action", &("chat", args)).await {
        Ok(op) => op,
        Err(error) => {
            fail(
                &app,
                generation,
                "The model service is not running. It starts when you use it; try again in a moment.",
            );
            eprintln!("MOBIUS talk Action: {error}");
            return Ok(());
        }
    };

    let stale = {
        let state = shell_state(&app);
        let mut shell = state.lock().expect("shell state");
        if shell.talk.generation != generation {
            true
        } else {
            shell.talk.operation = Some(operation);
            shell.talk.phase = TalkPhase::Streaming;
            false
        }
    };
    if stale {
        let _ = proxy.call_method("Cancel", &(operation,)).await;
        return Ok(());
    }
    emit(&app);

    let (Ok(deltas), Ok(done)) = (
        proxy.receive_signal("Delta").await,
        proxy.receive_signal("Completed").await,
    ) else {
        fail(&app, generation, "Could not follow the model reply.");
        return Ok(());
    };

    enum TalkEv {
        Delta(zbus::Message),
        Done(zbus::Message),
    }
    let mut events = futures_util::stream::select(
        deltas.map(TalkEv::Delta),
        done.map(TalkEv::Done),
    );
    while let Some(event) = events.next().await {
        match event {
            TalkEv::Delta(message) => {
                if let Ok((op, chunk)) = message.body().deserialize::<(u64, String)>() {
                    if op != operation {
                        continue;
                    }
                    let cont = {
                        let state = shell_state(&app);
                        let mut shell = state.lock().expect("shell state");
                        if shell.talk.generation != generation {
                            false
                        } else {
                            shell.talk.reply.push_str(&chunk);
                            if shell.talk.reply.chars().count() > MAX_REPLY_CHARS {
                                shell.talk.reply =
                                    shell.talk.reply.chars().take(MAX_REPLY_CHARS).collect();
                            }
                            true
                        }
                    };
                    if !cont {
                        return Ok(());
                    }
                    emit(&app);
                }
            }
            TalkEv::Done(message) => {
                if let Ok((op, ok, result)) = message.body().deserialize::<(u64, bool, String)>() {
                    if op != operation {
                        continue;
                    }
                    finish(&app, generation, ok, &result);
                    return Ok(());
                }
            }
        }
    }
    fail(&app, generation, "The reply ended unexpectedly.");
    Ok(())
}

fn fail(app: &AppHandle, generation: u64, reason: &str) {
    let state = shell_state(app);
    let mut shell = state.lock().expect("shell state");
    if shell.talk.generation != generation {
        return;
    }
    shell.talk.phase = TalkPhase::Failed;
    shell.talk.error = reason.to_string();
    shell.talk.operation = None;
    // Drop the dangling user turn if we never got a reply started cleanly.
    if shell.talk.reply.is_empty() {
        if let Some(("user", _)) = shell.talk.history.last().map(|p| (p.0.as_str(), ())) {
            shell.talk.history.pop();
        }
    }
    drop(shell);
    emit(app);
}

fn finish(app: &AppHandle, generation: u64, ok: bool, result_json: &str) {
    let state = shell_state(app);
    let mut shell = state.lock().expect("shell state");
    if shell.talk.generation != generation {
        return;
    }
    shell.talk.operation = None;
    if !ok {
        let err = serde_json::from_str::<serde_json::Value>(result_json)
            .ok()
            .and_then(|v| {
                v.get("error")
                    .and_then(|e| e.as_str())
                    .map(str::to_string)
            })
            .unwrap_or_else(|| "The model could not answer.".into());
        shell.talk.phase = TalkPhase::Failed;
        shell.talk.error = err;
        if shell.talk.reply.is_empty() {
            if let Some(("user", _)) = shell.talk.history.last().map(|p| (p.0.as_str(), ())) {
                shell.talk.history.pop();
            }
        }
        drop(shell);
        emit(app);
        return;
    }
    if shell.talk.reply.is_empty() {
        if let Some(text) = serde_json::from_str::<serde_json::Value>(result_json)
            .ok()
            .and_then(|v| v.get("text").and_then(|t| t.as_str()).map(str::to_string))
        {
            shell.talk.reply = text;
        }
    }
    let reply = shell.talk.reply.clone();
    if !reply.is_empty() {
        shell.talk.history.push(("assistant".into(), reply));
        if shell.talk.history.len() > MAX_HISTORY_TURNS * 2 {
            let drop_n = shell.talk.history.len() - MAX_HISTORY_TURNS * 2;
            shell.talk.history.drain(0..drop_n);
        }
    }
    shell.talk.phase = TalkPhase::Done;
    drop(shell);
    emit(app);
}

pub async fn cancel(app: AppHandle) {
    let (generation, operation) = {
        let state = shell_state(&app);
        let mut shell = state.lock().expect("shell state");
        let op = shell.talk.operation.take();
        if matches!(
            shell.talk.phase,
            TalkPhase::Thinking | TalkPhase::Streaming
        ) {
            shell.talk.phase = TalkPhase::Idle;
            shell.talk.error.clear();
            if shell.talk.reply.is_empty() {
                if let Some(("user", _)) = shell.talk.history.last().map(|p| (p.0.as_str(), ())) {
                    shell.talk.history.pop();
                }
            }
        }
        (shell.talk.generation, op)
    };
    emit(&app);
    if let Some(operation) = operation {
        if let Ok(proxy) = inference_proxy().await {
            let _ = proxy.call_method("Cancel", &(operation,)).await;
        }
        let _ = generation;
    }
}

pub fn talk_view(app: &AppHandle) -> TalkView {
    shell_state(app).lock().expect("shell state").talk.view()
}

pub fn clear(app: &AppHandle) {
    {
        let state = shell_state(app);
        let mut shell = state.lock().expect("shell state");
        if matches!(
            shell.talk.phase,
            TalkPhase::Thinking | TalkPhase::Streaming
        ) {
            return;
        }
        shell.talk.history.clear();
        shell.talk.reply.clear();
        shell.talk.error.clear();
        shell.talk.operation = None;
        shell.talk.phase = TalkPhase::Idle;
    }
    emit(app);
}

/// Last user line in the bar Talk thread (for Act handoff).
pub fn last_user_text(app: &AppHandle) -> Option<String> {
    let state = shell_state(app);
    let shell = state.lock().expect("shell state");
    shell
        .talk
        .history
        .iter()
        .rev()
        .find(|(role, _)| role == "user")
        .map(|(_, text)| text.clone())
}

fn handoff_path() -> std::path::PathBuf {
    inference_routing::state_dir().join("mobius-bar-handoff.json")
}

/// Writes bar Talk history for the full Mobius window to pick up, then clears the bar thread.
pub fn write_handoff(app: &AppHandle) -> Result<(), String> {
    let history = {
        let state = shell_state(app);
        let mut shell = state.lock().expect("shell state");
        if matches!(
            shell.talk.phase,
            TalkPhase::Thinking | TalkPhase::Streaming
        ) {
            return Err("Wait for the reply, or stop it.".into());
        }
        if shell.talk.history.is_empty() {
            return Err("Nothing to continue yet — ask something first.".into());
        }
        // Include in-progress reply if Done left it only in reply (history already has assistant).
        let history = shell.talk.history.clone();
        shell.talk.history.clear();
        shell.talk.reply.clear();
        shell.talk.error.clear();
        shell.talk.phase = TalkPhase::Idle;
        history
    };
    emit(app);
    let messages: Vec<serde_json::Value> = history
        .into_iter()
        .map(|(role, content)| {
            serde_json::json!({
                "role": role,
                "content": content,
            })
        })
        .collect();
    let path = handoff_path();
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    let body = serde_json::json!({ "messages": messages });
    std::fs::write(&path, body.to_string()).map_err(|e| e.to_string())?;
    Ok(())
}
