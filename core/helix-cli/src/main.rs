//! `helix`: Helix's actions from a terminal or a script (`docs/design/north-star.md`, "open by design"; `docs/design/actions.md`).
//!
//! Everything the person can click is something they can script: find a setting by what it does, read it, change it, run any catalogue
//! action, read the one history of changes and undo one. Changes go through the desktop's Agent Surface like any other client's, so the
//! first time the person is asked to allow "helix command line" (an agent with a shell cannot quietly borrow it), and each change is in
//! the history under that name. Reading the history and undoing a change are the person's own Settings calls and need no grant.

use std::collections::HashMap;
use std::process::ExitCode;
use std::time::Duration;

use clap::{Parser, Subcommand};
use futures_util::StreamExt;
use serde_json::{json, Value};
use zbus::zvariant::{OwnedValue, Value as ZValue};

const BUS: &str = "org.helix.Desktop";
const PATH: &str = "/org/helix/Desktop";
const AGENT: &str = "org.helix.Desktop.AgentSurface1";
const CONFIGURATION: &str = "org.helix.Desktop.Configuration1";
const LABEL: &str = "helix command line";

#[derive(Parser)]
#[command(
    name = "helix",
    about = "Helix's actions, their history and Undo, from a terminal or a script."
)]
struct Cli {
    /// Print JSON (for scripts and agents) instead of sentences.
    #[arg(long, global = true)]
    json: bool,
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Every action Helix offers, with its risk.
    Actions,
    /// Find settings by what they do: `helix find night light`.
    Find {
        #[arg(required = true)]
        words: Vec<String>,
    },
    /// The current value of a setting: `helix get theme.mode`.
    Get { setting: String },
    /// Change a setting: `helix set theme.mode dark`. The change is recorded and can be undone.
    Set { setting: String, value: String },
    /// Run any catalogue action with its parameters as key=value (values read as JSON when they are): `helix do settings.find query=dock`.
    Do { action: String, params: Vec<String> },
    /// What changed, newest first, and who changed it.
    History {
        #[arg(short = 'n', long, default_value_t = 10)]
        limit: u32,
    },
    /// Put back what a history entry changed: `helix undo 42`.
    Undo { entry: u64 },
}

/// `true` → JSON true, `64` → 64, `dark` → "dark": a value is JSON when it parses as JSON, else text.
fn value_of(text: &str) -> Value {
    serde_json::from_str(text).unwrap_or_else(|_| Value::String(text.to_string()))
}

fn params_of(pairs: &[String]) -> Result<Value, String> {
    let mut out = serde_json::Map::new();
    for pair in pairs {
        let (key, value) = pair
            .split_once('=')
            .ok_or_else(|| format!("\"{pair}\" is not key=value"))?;
        out.insert(key.trim().to_string(), value_of(value));
    }
    Ok(Value::Object(out))
}

/// The tier an action needs first; the desktop may ask for a stricter one, which is then requested and the call made again.
fn first_tier(action: &str) -> &'static str {
    if action.ends_with(".find") || action.ends_with(".get") || action.ends_with(".list") {
        "observe"
    } else {
        "act"
    }
}

struct Surface {
    proxy: zbus::Proxy<'static>,
    completed: zbus::proxy::SignalStream<'static>,
    tiers: Vec<String>,
}

impl Surface {
    async fn connect(connection: &zbus::Connection) -> Result<Self, String> {
        let proxy = zbus::Proxy::new(connection, BUS, PATH, AGENT)
            .await
            .map_err(|e| e.to_string())?;
        let completed = proxy
            .receive_signal("Completed")
            .await
            .map_err(|e| e.to_string())?;
        let (session,): (HashMap<String, OwnedValue>,) =
            proxy
                .call("Hello", &(LABEL, "1"))
                .await
                .map_err(|e| format!("The Helix desktop does not answer: {e}"))?;
        let tiers = session
            .get("tiers")
            .and_then(|v| <Vec<String>>::try_from(v.try_clone().ok()?).ok())
            .unwrap_or_default();
        Ok(Self {
            proxy,
            completed,
            tiers,
        })
    }

    async fn wait(&mut self, operation: u64) -> Result<(bool, Value), String> {
        // Long enough for the person to answer an approval.
        let deadline = tokio::time::sleep(Duration::from_secs(120));
        tokio::pin!(deadline);
        loop {
            tokio::select! {
                _ = &mut deadline => return Err("No answer from the desktop (is an approval waiting for you?).".into()),
                signal = self.completed.next() => {
                    let Some(signal) = signal else { return Err("The desktop went away.".into()) };
                    let (op, ok, body): (u64, bool, String) = signal.body().deserialize().map_err(|e| e.to_string())?;
                    if op == operation {
                        return Ok((ok, serde_json::from_str(&body).unwrap_or(Value::String(body))));
                    }
                }
            }
        }
    }

    async fn ensure(&mut self, tier: &str) -> Result<(), String> {
        if self.tiers.iter().any(|t| t == tier) {
            return Ok(());
        }
        eprintln!("Asking you to allow the helix command line ({tier})…");
        let (operation,): (u64,) = self
            .proxy
            .call("RequestAccess", &(vec![tier.to_string()],))
            .await
            .map_err(|e| e.to_string())?;
        let (ok, result) = self.wait(operation).await?;
        if !ok {
            return Err(result
                .get("error")
                .and_then(Value::as_str)
                .unwrap_or("Not allowed.")
                .to_string());
        }
        if let Some(granted) = result.get("granted").and_then(Value::as_array) {
            self.tiers.extend(
                granted
                    .iter()
                    .filter_map(|v| v.as_str().map(str::to_string)),
            );
        }
        Ok(())
    }

    async fn call(&mut self, action: &str, params: &Value) -> Result<Value, String> {
        let mut options: HashMap<&str, ZValue<'_>> = HashMap::new();
        options.insert("params", ZValue::from(params.to_string()));
        options.insert("reason", ZValue::from(format!("{LABEL}: {action}")));
        let (operation,): (u64,) = self
            .proxy
            .call("Action", &(action, options))
            .await
            .map_err(|e| e.to_string())?;
        let (ok, result) = self.wait(operation).await?;
        if !ok {
            return Err(result
                .get("error")
                .and_then(Value::as_str)
                .unwrap_or("The action failed.")
                .to_string());
        }
        let content = result
            .get("content")
            .and_then(Value::as_str)
            .unwrap_or_default();
        Ok(serde_json::from_str(content).unwrap_or_else(|_| Value::String(content.to_string())))
    }

    /// Runs an action, asking once for a stricter grant when the desktop says this call needs one.
    async fn run(&mut self, action: &str, params: &Value) -> Result<Value, String> {
        self.ensure(first_tier(action)).await?;
        let mut outcome = self.call(action, params).await;
        for needed in ["observe-private", "confirm"] {
            if matches!(&outcome, Err(why) if why.contains(&format!("no {needed} grant"))) {
                self.ensure(needed).await?;
                outcome = self.call(action, params).await;
            }
        }
        outcome
    }
}

async fn configuration(connection: &zbus::Connection) -> Result<zbus::Proxy<'static>, String> {
    zbus::Proxy::new(connection, BUS, PATH, CONFIGURATION)
        .await
        .map_err(|e| e.to_string())
}

fn print_settings(found: &Value) {
    let rows = found
        .get("settings")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    if rows.is_empty() {
        println!("No setting matches.");
    }
    for row in rows {
        let text = |key: &str| {
            row.get(key)
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_string()
        };
        let value = row
            .get("value")
            .map_or_else(|| "(private)".to_string(), |v| v.to_string());
        let change = match text("change").as_str() {
            "act" => String::new(),
            "confirm" => "  · asks you first".into(),
            "blocked" => "  · not for scripts or agents".into(),
            other => format!("  · {other}"),
        };
        println!(
            "{:<40} {}  = {value}{change}",
            text("setting"),
            text("title")
        );
    }
}

async fn run(cli: Cli) -> Result<(), String> {
    let connection = zbus::Connection::session()
        .await
        .map_err(|e| format!("No desktop session: {e}"))?;
    match cli.command {
        Command::Actions => {
            let proxy = zbus::Proxy::new(&connection, BUS, PATH, AGENT)
                .await
                .map_err(|e| e.to_string())?;
            let text: String = proxy
                .call("ListActions", &())
                .await
                .map_err(|e| e.to_string())?;
            let actions: Value = serde_json::from_str(&text).map_err(|e| e.to_string())?;
            if cli.json {
                println!("{actions}");
            } else {
                for action in actions.as_array().into_iter().flatten() {
                    let text = |key: &str| {
                        action
                            .get(key)
                            .and_then(Value::as_str)
                            .unwrap_or_default()
                            .to_string()
                    };
                    println!("{:<16} {:<22} {}", text("id"), text("title"), text("risk"));
                }
            }
        }
        Command::Find { words } => {
            let found = Surface::connect(&connection)
                .await?
                .run("settings.find", &json!({ "query": words.join(" ") }))
                .await?;
            if cli.json {
                println!("{found}")
            } else {
                print_settings(&found)
            }
        }
        Command::Get { setting } => {
            let found = Surface::connect(&connection)
                .await?
                .run("settings.get", &json!({ "setting": setting }))
                .await?;
            if cli.json {
                println!("{found}");
            } else {
                println!(
                    "{}",
                    found
                        .get("value")
                        .map_or_else(|| "(none)".to_string(), Value::to_string)
                );
            }
        }
        Command::Set { setting, value } => {
            let done = Surface::connect(&connection)
                .await?
                .run(
                    "settings.set",
                    &json!({ "setting": setting, "value": value_of(&value) }),
                )
                .await?;
            if cli.json {
                println!("{done}");
            } else {
                let sentence = done.get("done").and_then(Value::as_str).unwrap_or_default();
                let entry = done.get("undo").and_then(Value::as_u64).unwrap_or(0);
                println!("{sentence}");
                if entry > 0 {
                    println!("Undo with: helix undo {entry}");
                }
            }
        }
        Command::Do { action, params } => {
            let params = params_of(&params)?;
            let out = Surface::connect(&connection)
                .await?
                .run(&action, &params)
                .await?;
            println!(
                "{}",
                if cli.json {
                    out.to_string()
                } else {
                    serde_json::to_string_pretty(&out).unwrap_or_default()
                }
            );
        }
        Command::History { limit } => {
            let proxy = configuration(&connection).await?;
            let text: String = proxy
                .call("ActionHistory", &(limit,))
                .await
                .map_err(|e| e.to_string())?;
            let history: Value = serde_json::from_str(&text).map_err(|e| e.to_string())?;
            if cli.json {
                println!("{history}");
            } else {
                for entry in history.as_array().into_iter().flatten() {
                    let id = entry.get("id").and_then(Value::as_u64).unwrap_or(0);
                    let sentence = entry
                        .get("sentence")
                        .and_then(Value::as_str)
                        .unwrap_or_default();
                    let by = entry
                        .pointer("/by/label")
                        .and_then(Value::as_str)
                        .unwrap_or_default();
                    let undone = entry.get("undone_by").is_some_and(|u| !u.is_null());
                    println!(
                        "{id:>5}  {sentence}  · {by}{}",
                        if undone { "  (undone)" } else { "" }
                    );
                }
            }
        }
        Command::Undo { entry } => {
            let proxy = configuration(&connection).await?;
            let done: String = proxy
                .call("UndoAction", &(entry,))
                .await
                .map_err(|e| match e {
                    zbus::Error::MethodError(_, Some(text), _) => text,
                    other => other.to_string(),
                })?;
            if cli.json {
                println!("{}", json!({ "done": done }))
            } else {
                println!("{done}")
            }
        }
    }
    Ok(())
}

#[tokio::main(flavor = "current_thread")]
async fn main() -> ExitCode {
    match run(Cli::parse()).await {
        Ok(()) => ExitCode::SUCCESS,
        Err(why) => {
            eprintln!("helix: {why}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn values_are_json_when_they_can_be_and_words_otherwise() {
        assert_eq!(value_of("true"), json!(true));
        assert_eq!(value_of("64"), json!(64));
        assert_eq!(value_of("dark"), json!("dark"));
        assert_eq!(value_of("\"64\""), json!("64"));
    }

    #[test]
    fn parameters_are_key_value_pairs() {
        assert_eq!(
            params_of(&["query=dock size".into(), "page=dock".into()]).unwrap(),
            json!({"query": "dock size", "page": "dock"})
        );
        assert_eq!(
            params_of(&["value=true".into()]).unwrap(),
            json!({"value": true})
        );
        assert!(params_of(&["nonsense".into()]).is_err());
    }

    #[test]
    fn reading_asks_for_less_than_changing() {
        assert_eq!(first_tier("settings.find"), "observe");
        assert_eq!(first_tier("settings.get"), "observe");
        assert_eq!(first_tier("models.list"), "observe");
        assert_eq!(first_tier("settings.set"), "act");
    }
}
