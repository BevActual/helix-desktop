//! helix-assistant "turn the volume up": plain English in, one desktop action out. Port of the C++ `helix-assistant`.
//!
//! The model (helix-inference) only chooses a tool call, constrained to the tools this client is allowed to use. Anything that
//! changes the desktop is shown first and runs only after a yes typed here. The call then goes through Agent Surface like any
//! other client's, so its grants, confirmations and audit apply on top. Design: docs/design/agent-and-inference.md.

mod bus;

use std::collections::BTreeSet;
use std::io::{BufRead, IsTerminal, Write};
use std::path::PathBuf;

use helix_agent::assistant_core::{
    build_messages, choose_model, describe, parse_reply, step_for, Reply, ReplyKind, Step,
};
use helix_agent::risk::{RiskPolicy, Tier};
use helix_agent::shell_ipc::shell_supported_tools;
use helix_agent::tool_schema::{
    msg_commands, tool_call_schema, tool_catalog, ToolHints, DECLINE_TOOL,
};
use serde_json::{json, Value as Json};
use zbus::proxy::Proxy;
use zbus::Connection;

use bus::{to_vardict, Waiter};

const DESKTOP: &str = "org.helix.Desktop";
const DESKTOP_PATH: &str = "/org/helix/Desktop";
const AGENT: &str = "org.helix.Desktop.AgentSurface1";
const INFERENCE: &str = "org.helix.Inference";
const INFERENCE_PATH: &str = "/org/helix/Inference";
const INFERENCE_IFACE: &str = "org.helix.Inference1";

/// Where the answer goes. Text mode talks to a person at a terminal. JSON mode (--json) is for the shell's assistant panel:
/// one JSON object on stdout per invocation, so the panel needs no terminal and the shell never holds the model's authority.
struct Output {
    json: bool,
}

impl Output {
    fn say(&self, line: &str) {
        if !self.json {
            println!("{line}");
        }
    }

    fn complain(&self, line: &str) {
        if !self.json {
            eprintln!("{line}");
        }
    }

    /// kind: done | confirm | explain | error
    fn result(&self, kind: &str, mut fields: Json) {
        if !self.json {
            return;
        }
        fields["kind"] = json!(kind);
        println!("{fields}");
    }

    fn fail(&self, text: &str, code: i32) -> i32 {
        self.complain(text);
        self.result("error", json!({"text": text}));
        code
    }
}

fn ask_yes_no(question: &str) -> bool {
    if !std::io::stdin().is_terminal() && std::env::var_os("HELIX_ASSISTANT_ANSWER_FROM_STDIN").is_none() {
        eprintln!("Not asking: no terminal to answer on. Nothing was changed.");
        return false;
    }
    print!("{question} [y/N] ");
    let _ = std::io::stdout().flush();
    let mut line = String::new();
    let _ = std::io::stdin().lock().read_line(&mut line);
    matches!(line.trim_end_matches(['\r', '\n']), "y" | "Y" | "yes")
}

fn state_dir() -> PathBuf {
    let var = |name: &str| std::env::var_os(name).filter(|v| !v.is_empty()).map(PathBuf::from);
    if let Some(helix) = var("HELIX_STATE_HOME") {
        return helix.join("helix");
    }
    if let Some(xdg) = var("XDG_STATE_HOME") {
        return xdg.join("helix");
    }
    if let Some(home) = var("HOME") {
        return home.join(".local/state/helix");
    }
    PathBuf::from("helix")
}

/// The model `inference-routing.toml` names for `task`; empty when unset or unreadable.
fn routed_model(task: &str) -> String {
    let Ok(text) = std::fs::read_to_string(state_dir().join("inference-routing.toml")) else {
        return String::new();
    };
    let Ok(table) = text.parse::<toml::Table>() else {
        return String::new();
    };
    table
        .get("profiles")
        .and_then(|p| p.get(task))
        .and_then(|p| p.get("model"))
        .and_then(toml::Value::as_str)
        .unwrap_or_default()
        .to_string()
}

fn load_policy() -> Result<RiskPolicy, String> {
    match std::env::var("HELIX_RISK_POLICY").ok().filter(|v| !v.is_empty()) {
        Some(path) => {
            let text = std::fs::read_to_string(&path)
                .map_err(|_| format!("risk policy: cannot read {path}"))?;
            RiskPolicy::parse(&text)
        }
        None => Ok(RiskPolicy::embedded()),
    }
}

fn load_hints() -> Result<ToolHints, String> {
    match std::env::var("HELIX_TOOL_HINTS").ok().filter(|v| !v.is_empty()) {
        Some(path) => ToolHints::load(std::path::Path::new(&path)),
        None => Ok(ToolHints::embedded()),
    }
}

fn tiers_of(list: &Json) -> BTreeSet<Tier> {
    list.as_array()
        .into_iter()
        .flatten()
        .filter_map(Json::as_str)
        .filter_map(Tier::parse)
        .collect()
}

struct Args {
    dry_run: bool,
    warm: bool,
    execute: bool,
    json: bool,
    request: String,
    request_text: String,
    execute_args: Vec<String>,
}

fn parse_args(argv: &[String]) -> Args {
    let mut args = Args {
        dry_run: false,
        warm: false,
        execute: false,
        json: false,
        request: String::new(),
        request_text: String::new(),
        execute_args: Vec::new(),
    };
    let mut i = 0;
    while i < argv.len() {
        let arg = &argv[i];
        if args.execute && !args.execute_args.is_empty() {
            args.execute_args.push(arg.clone());
        } else if arg == "--dry-run" {
            args.dry_run = true;
        } else if arg == "--warm" {
            args.warm = true;
        } else if arg == "--json" {
            args.json = true;
        } else if arg == "--request" && i + 1 < argv.len() {
            i += 1;
            args.request_text = argv[i].clone();
        } else if arg == "--execute" {
            args.execute = true;
            if i + 1 < argv.len() {
                i += 1;
                args.execute_args.push(argv[i].clone());
            }
        } else {
            if !args.request.is_empty() {
                args.request.push(' ');
            }
            args.request.push_str(arg);
        }
        i += 1;
    }
    args
}

struct Desk<'a> {
    agent: &'a Proxy<'a>,
    agent_done: &'a mut Waiter,
    out: &'a Output,
    hints: &'a ToolHints,
}

impl Desk<'_> {
    /// Runs a call through Agent Surface and reports the outcome. The service decides; this only carries the result back.
    async fn run(&mut self, reply: &Reply, why: &str) -> Result<i32, String> {
        let why: String = why.chars().take(200).collect();
        let action = json!({"argv": [], "literal": reply.args, "reason": why});
        let operation: u64 = self
            .agent
            .call("Action", &(reply.tool.as_str(), to_vardict(&action)))
            .await
            .map_err(|e| e.to_string())?;
        let result = self.agent_done.wait(operation, 90).await; // a confirm-tier call can wait for the person
        if !result.ok {
            let error = result.result.get("error").and_then(Json::as_str).unwrap_or("failed");
            return Ok(self.out.fail(&format!("Not done: {error}"), 1));
        }
        let content = result.result.get("content").and_then(Json::as_str).unwrap_or("");
        let untrusted = result.result.get("trust").and_then(Json::as_str) == Some("untrusted");
        let shown = if content.is_empty() { "Done." } else { content };
        self.out.say(&format!(
            "{}{shown}",
            if untrusted { "(from outside sources, not verified)\n" } else { "" }
        ));
        self.out.result(
            "done",
            json!({
                "text": shown,
                "untrusted": untrusted,
                "description": describe(reply, Some(self.hints)),
            }),
        );
        Ok(0)
    }
}

async fn run(args: Args, out: &Output) -> Result<i32, String> {
    let policy = load_policy()?;
    let hints = load_hints()?;
    let connection = Connection::session()
        .await
        .map_err(|e| format!("No session bus: {e}"))?;
    let agent = Proxy::new(&connection, DESKTOP, DESKTOP_PATH, AGENT)
        .await
        .map_err(|e| e.to_string())?;
    let inference = Proxy::new(&connection, INFERENCE, INFERENCE_PATH, INFERENCE_IFACE)
        .await
        .map_err(|e| e.to_string())?;
    // Subscribed before any call, so a fast reply is never missed.
    let mut agent_done = Waiter::new(agent.receive_signal("Completed").await.map_err(|e| e.to_string())?);
    let mut chat_done = Waiter::new(inference.receive_signal("Completed").await.map_err(|e| e.to_string())?);

    let session: std::collections::HashMap<String, zbus::zvariant::OwnedValue> = agent
        .call("Hello", &("Helix assistant", "1"))
        .await
        .map_err(|e| e.to_string())?;
    let session: Json = bus::from_variant(&zbus::zvariant::Value::from(session));
    let mut granted = tiers_of(&session["tiers"]);
    if !granted.contains(&Tier::Observe) || !granted.contains(&Tier::Act) {
        if !session["confirmer"].as_bool().unwrap_or(false) {
            return Ok(out.fail(
                "The assistant has no permission yet, and the desktop shell is not running to ask you.",
                1,
            ));
        }
        out.say(
            "The assistant needs your permission to read the desktop state and make everyday changes. \
             Look for the approval dialog.",
        );
        let operation: u64 = agent
            .call("RequestAccess", &(vec!["observe", "act"],))
            .await
            .map_err(|e| e.to_string())?;
        let answer = agent_done.wait(operation, 90).await;
        if !answer.ok {
            let error = answer.result.get("error").and_then(Json::as_str).unwrap_or("denied");
            return Ok(out.fail(&format!("No permission was given: {error}"), 1));
        }
        granted = tiers_of(&answer.result["granted"]);
    }

    let mut desk = Desk { agent: &agent, agent_done: &mut agent_done, out, hints: &hints };

    if args.execute {
        // A call the person has just been shown and approved. It gets the same checks as a model's reply: the tool must exist
        // and be within this client's grants, and the service still applies its own.
        let tool = args.execute_args.first().cloned().unwrap_or_default();
        let reply = Reply::call(&tool, args.execute_args.iter().skip(1).cloned().collect());
        let shell_tools = shell_supported_tools();
        if step_for(&policy, &granted, &reply) == Step::Refuse
            || reply.tool == DECLINE_TOOL
            || (!shell_tools.is_empty() && !shell_tools.contains(&reply.tool))
        {
            let text = "That is not something this assistant is allowed to do.";
            out.say(text);
            out.result("explain", json!({"text": text}));
            return Ok(3);
        }
        let why = if args.request_text.is_empty() { "approved in the assistant panel" } else { &args.request_text };
        return desk.run(&reply, why).await;
    }

    // Offer the model only what this client may do, and only the tiers the evaluation covered.
    let offered: BTreeSet<Tier> = [Tier::Observe, Tier::ObservePrivate, Tier::Act]
        .into_iter()
        .filter(|tier| granted.contains(tier))
        .collect();
    if offered.is_empty() {
        return Ok(out.fail("The assistant has no usable permission.", 1));
    }
    // The model is shown every tool: it is best at finding the right one, and a request for something this desktop cannot do
    // then comes back as that tool, which is turned into a decline below.
    let carried_out = shell_supported_tools();
    let commands = msg_commands();
    let schema = tool_call_schema(&commands, &policy, &offered, None, true);
    let catalog = tool_catalog(&commands, &policy, &offered, None, true, Some(&hints));

    // HELIX_ASSISTANT_MODEL wins when set and installed; otherwise the routing file's `act` profile.
    let mut preferred = std::env::var("HELIX_ASSISTANT_MODEL").unwrap_or_default();
    if preferred.is_empty() {
        preferred = routed_model("act");
    }
    let models: Vec<zbus::zvariant::OwnedValue> = inference
        .call("ListModels", &())
        .await
        .map_err(|e| e.to_string())?;
    let models = Json::Array(models.iter().map(|m| bus::from_variant(m)).collect());
    let chosen = choose_model(&models, &preferred);
    if !chosen.error.is_empty() {
        return Ok(out.fail(&chosen.error, 1));
    }
    let request = if args.warm { "Turn the volume up.".to_string() } else { args.request.clone() };
    let options = json!({
        "messages": build_messages(&catalog, &request),
        "schema": schema.to_string(),
        "task": "act",
        "model": chosen.id,
        "max_tokens": if args.warm { 1 } else { 128 },
        "temperature": 0.0,
        "priority": if args.warm { "background" } else { "interactive" },
    });
    let chat: u64 = inference
        .call("Action", &("chat", to_vardict(&options)))
        .await
        .map_err(|e| e.to_string())?;
    let answer = chat_done.wait(chat, 300).await;
    if args.warm {
        let truncated = answer.result.get("truncated").and_then(Json::as_bool).unwrap_or(false);
        if !answer.ok && !truncated {
            let error = answer.result.get("error").and_then(Json::as_str).unwrap_or("unknown error");
            return Ok(out.fail(&format!("Could not warm the model: {error}"), 1));
        }
        let text = format!("Model {} is ready.", chosen.id);
        out.say(&text);
        out.result("done", json!({"text": text}));
        return Ok(0);
    }
    if !answer.ok {
        let error = answer.result.get("error").and_then(Json::as_str).unwrap_or("unknown error");
        return Ok(out.fail(&format!("The model could not answer: {error}"), 1));
    }

    let mut reply = parse_reply(answer.result.get("text").and_then(Json::as_str).unwrap_or(""));
    if reply.kind == ReplyKind::Call && !carried_out.is_empty() && !carried_out.contains(&reply.tool) {
        // A real tool, but not one this desktop's shell can carry out: say so, and do nothing.
        reply.kind = ReplyKind::Decline;
        reply.args.clear();
        reply.reason = format!("this desktop cannot do that yet ({})", describe(&reply, Some(&hints)));
    }
    match step_for(&policy, &granted, &reply) {
        Step::Explain => {
            let text = if reply.kind == ReplyKind::Decline {
                format!("I can't do that: {}", reply.reason)
            } else {
                format!("I couldn't work out what to do: {}", reply.error)
            };
            out.say(&text);
            out.result("explain", json!({"text": text}));
            return Ok(3);
        }
        Step::Refuse => {
            let text = format!(
                "I can't do that: {} is not something this assistant is allowed to use.",
                reply.tool
            );
            out.say(&text);
            out.result("explain", json!({"text": text}));
            return Ok(3);
        }
        Step::AskFirst => {
            out.say(&format!("About to: {}", describe(&reply, Some(&hints))));
            if !reply.reason.is_empty() {
                out.say(&format!("Because: {}", reply.reason));
            }
            if out.json {
                // The panel shows this, and comes back with --execute if the person agrees.
                out.result(
                    "confirm",
                    json!({
                        "description": describe(&reply, Some(&hints)),
                        "reason": reply.reason,
                        "tool": reply.tool,
                        "args": reply.args,
                    }),
                );
                return Ok(0);
            }
            if args.dry_run {
                out.say("(dry run: nothing was done)");
                return Ok(0);
            }
            if !ask_yes_no("Go ahead?") {
                out.say("Cancelled. Nothing was changed.");
                return Ok(0);
            }
        }
        Step::Run => {
            out.say(&format!("Checking: {}", describe(&reply, Some(&hints))));
            if args.dry_run {
                out.say("(dry run: nothing was done)");
                return Ok(0);
            }
        }
    }
    desk.run(&reply, &request).await
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let mut args = parse_args(&argv);
    let out = Output { json: args.json };
    if args.warm {
        args.request = "Turn the volume up.".into(); // any request: the prompt prefix is what gets cached
    }
    if args.request.is_empty() && !args.execute {
        eprintln!(
            "usage: helix-assistant [--dry-run] [--json] <what you want done>\n       helix-assistant --warm    load the \
             model and prepare its memory, so the first request is fast\n       helix-assistant --json --request <text> \
             --execute <tool> [arguments...]    run a call that was shown and approved"
        );
        std::process::exit(2);
    }
    if args.request.chars().count() > 500 {
        std::process::exit(out.fail("That request is too long (500 characters at most).", 2));
    }
    let code = match run(args, &out).await {
        Ok(code) => code,
        Err(message) => out.fail(&message, 1),
    };
    std::process::exit(code);
}
