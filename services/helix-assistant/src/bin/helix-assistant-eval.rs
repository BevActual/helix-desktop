//! Measures how well the model behind the assistant chooses a desktop action, through the real stack: the live tool catalog and grammar,
//! the shipped prompt (`build_messages`), and the Inference1 service the assistant itself uses. Nothing is run on the desktop.
//!
//!   helix-assistant-eval [CASES.json] [--model ID] [--limit N] [--category NAME] [--raw] [--json OUT] [--min-exact PERCENT]
//!
//! CASES defaults to `tests/eval/desktop_requests.json` (`tests/eval/desktop_requests_heldout.json` is the second set, for checking that
//! changes to the prompt or examples were not tuned to the first). A case has the accepted calls (`accept`, arguments default to none, so
//! an invented argument is not scored as right) or `"expect": "decline"` for a request the desktop cannot do. A call to a real tool that
//! this desktop's shell cannot carry out counts as a decline, as it does in the assistant, unless `--raw`. The run needs a session bus
//! with `helix-inference` on it and a model installed; with `--min-exact` it exits 1 below that exact-match rate.

#[path = "../bus.rs"]
mod bus;

use std::collections::BTreeSet;
use std::time::Instant;

use bus::{to_vardict, Waiter};
use helix_agent::assistant_core::{build_messages, choose_model, parse_reply, ReplyKind};
use helix_agent::risk::{RiskPolicy, Tier};
use helix_agent::shell_ipc::shell_supported_tools;
use helix_agent::tool_schema::{msg_commands, tool_call_schema, tool_catalog, ToolHints};
use serde_json::{json, Value as Json};
use zbus::proxy::Proxy;
use zbus::Connection;

const INFERENCE: &str = "org.helix.Inference";
const INFERENCE_PATH: &str = "/org/helix/Inference";
const INFERENCE_IFACE: &str = "org.helix.Inference1";

#[derive(Default)]
struct Args {
    cases: String,
    model: String,
    limit: usize,
    category: String,
    raw: bool,
    json_out: String,
    min_exact: Option<f64>,
}

fn parse_args() -> Result<Args, String> {
    let mut args = Args {
        cases: "tests/eval/desktop_requests.json".into(),
        ..Args::default()
    };
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        let mut value = |name: &str| it.next().ok_or(format!("{name} needs a value"));
        match arg.as_str() {
            "--model" => args.model = value("--model")?,
            "--limit" => {
                args.limit = value("--limit")?
                    .parse()
                    .map_err(|_| "--limit is a number")?
            }
            "--category" => args.category = value("--category")?,
            "--raw" => args.raw = true,
            "--json" => args.json_out = value("--json")?,
            "--min-exact" => {
                args.min_exact = Some(
                    value("--min-exact")?
                        .parse()
                        .map_err(|_| "--min-exact is a number")?,
                )
            }
            "-h" | "--help" => return Err(String::new()),
            other if other.starts_with('-') => return Err(format!("unknown option {other}")),
            path => args.cases = path.to_string(),
        }
    }
    Ok(args)
}

fn load_policy() -> Result<RiskPolicy, String> {
    match std::env::var("HELIX_RISK_POLICY")
        .ok()
        .filter(|v| !v.is_empty())
    {
        Some(path) => RiskPolicy::parse(
            &std::fs::read_to_string(&path)
                .map_err(|_| format!("risk policy: cannot read {path}"))?,
        ),
        None => Ok(RiskPolicy::embedded()),
    }
}

fn load_hints() -> Result<ToolHints, String> {
    match std::env::var("HELIX_TOOL_HINTS")
        .ok()
        .filter(|v| !v.is_empty())
    {
        Some(path) => ToolHints::parse(
            &std::fs::read_to_string(&path)
                .map_err(|_| format!("tool hints: cannot read {path}"))?,
        ),
        None => Ok(ToolHints::embedded()),
    }
}

struct Case {
    id: u64,
    category: String,
    request: String,
    /// Empty for a request that must be declined.
    accept: Vec<(String, Vec<String>)>,
}

fn load_cases(path: &str) -> Result<Vec<Case>, String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("cannot read {path}: {e}"))?;
    let doc: Json = serde_json::from_str(&text).map_err(|e| format!("{path}: {e}"))?;
    let cases = doc["cases"].as_array().ok_or("no \"cases\" array")?;
    cases
        .iter()
        .map(|c| {
            let accept = c["accept"]
                .as_array()
                .map(|list| {
                    list.iter()
                        .map(|a| {
                            let tool = a["tool"].as_str().unwrap_or_default().to_string();
                            let args = a["args"]
                                .as_array()
                                .map(|v| {
                                    v.iter()
                                        .filter_map(|x| x.as_str().map(str::to_string))
                                        .collect()
                                })
                                .unwrap_or_default();
                            (tool, args)
                        })
                        .collect()
                })
                .unwrap_or_default();
            Ok(Case {
                id: c["id"].as_u64().unwrap_or(0),
                category: c["category"].as_str().unwrap_or_default().to_string(),
                request: c["request"]
                    .as_str()
                    .ok_or("a case has no request")?
                    .to_string(),
                accept,
            })
        })
        .collect()
}

fn median(mut v: Vec<f64>) -> f64 {
    if v.is_empty() {
        return 0.0;
    }
    v.sort_by(|a, b| a.total_cmp(b));
    v[v.len() / 2]
}

fn percent(part: usize, whole: usize) -> f64 {
    if whole == 0 {
        0.0
    } else {
        100.0 * part as f64 / whole as f64
    }
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let args = match parse_args() {
        Ok(args) => args,
        Err(message) => {
            if !message.is_empty() {
                eprintln!("error: {message}");
            }
            eprintln!("usage: helix-assistant-eval [CASES.json] [--model ID] [--limit N] [--category NAME] [--raw] [--json OUT] [--min-exact PERCENT]");
            std::process::exit(2);
        }
    };
    match run(args).await {
        Ok(code) => std::process::exit(code),
        Err(message) => {
            eprintln!("error: {message}");
            std::process::exit(2);
        }
    }
}

async fn run(args: Args) -> Result<i32, String> {
    let mut cases = load_cases(&args.cases)?;
    if !args.category.is_empty() {
        cases.retain(|case| case.category == args.category);
    }
    if args.limit > 0 {
        cases.truncate(args.limit);
    }
    let policy = load_policy()?;
    let hints = load_hints()?;
    // The same tiers the assistant offers a client that holds observe and act.
    let offered: BTreeSet<Tier> = [Tier::Observe, Tier::ObservePrivate, Tier::Act]
        .into_iter()
        .collect();
    let commands = msg_commands();
    let schema = tool_call_schema(&commands, &policy, &offered, None, true).to_string();
    let catalog = tool_catalog(&commands, &policy, &offered, None, true, Some(&hints));
    let carried_out = if args.raw {
        Vec::new()
    } else {
        shell_supported_tools().into_iter().collect()
    };

    let connection = Connection::session()
        .await
        .map_err(|e| format!("no session bus: {e}"))?;
    let inference = Proxy::new(&connection, INFERENCE, INFERENCE_PATH, INFERENCE_IFACE)
        .await
        .map_err(|e| e.to_string())?;
    let mut done = Waiter::new(
        inference
            .receive_signal("Completed")
            .await
            .map_err(|e| e.to_string())?,
    );
    let models: Vec<zbus::zvariant::OwnedValue> = inference
        .call("ListModels", &())
        .await
        .map_err(|e| format!("helix-inference is not answering: {e}"))?;
    let models = Json::Array(models.iter().map(|m| bus::from_variant(m)).collect());
    let chosen = choose_model(&models, &args.model);
    if !chosen.error.is_empty() {
        return Err(chosen.error);
    }
    eprintln!(
        "model {}, {} cases from {}{}",
        chosen.id,
        cases.len(),
        args.cases,
        if args.raw {
            " (raw: no shell filter)"
        } else {
            ""
        }
    );

    let (mut exact, mut right_tool, mut invalid) = (0usize, 0usize, 0usize);
    let (mut act_cases, mut decline_cases, mut declined_ok, mut acted) =
        (0usize, 0usize, 0usize, 0usize);
    let mut latencies = Vec::new();
    let mut misses = Vec::new();
    let mut per_category: std::collections::BTreeMap<String, (usize, usize)> = Default::default();
    let mut rows = Vec::new();

    for (index, case) in cases.iter().enumerate() {
        let options = json!({
            "messages": build_messages(&catalog, &case.request),
            "schema": schema,
            "task": "act",
            "model": chosen.id,
            "max_tokens": 128,
            "temperature": 0.0,
            "priority": "interactive",
        });
        let started = Instant::now();
        let operation: u64 = inference
            .call("Action", &("chat", to_vardict(&options)))
            .await
            .map_err(|e| e.to_string())?;
        let answer = done.wait(operation, 300).await;
        let seconds = started.elapsed().as_secs_f64();
        // The first request pays for loading the model; it is not what a person waits for every time.
        if index > 0 {
            latencies.push(seconds);
        }
        let text = answer
            .result
            .get("text")
            .and_then(Json::as_str)
            .unwrap_or_default();
        let mut reply = parse_reply(text);
        if reply.kind == ReplyKind::Call
            && !carried_out.is_empty()
            && !carried_out.contains(&reply.tool)
        {
            reply.kind = ReplyKind::Decline;
        }
        let got = match reply.kind {
            ReplyKind::Call => format!("{} {:?}", reply.tool, reply.args),
            ReplyKind::Decline => "decline".to_string(),
            ReplyKind::Invalid => "invalid".to_string(),
        };
        if reply.kind == ReplyKind::Invalid || !answer.ok {
            invalid += 1;
        }
        let entry = per_category.entry(case.category.clone()).or_default();
        entry.1 += 1;
        let good;
        if case.accept.is_empty() {
            decline_cases += 1;
            good = reply.kind == ReplyKind::Decline;
            if good {
                declined_ok += 1;
            } else {
                acted += 1;
            }
            if !good {
                misses.push(format!(
                    "#{} \"{}\": acted instead of declining: {got}",
                    case.id, case.request
                ));
            }
        } else {
            act_cases += 1;
            let tool_ok = reply.kind == ReplyKind::Call
                && case.accept.iter().any(|(tool, _)| *tool == reply.tool);
            let exact_ok = reply.kind == ReplyKind::Call
                && case
                    .accept
                    .iter()
                    .any(|(tool, a)| *tool == reply.tool && *a == reply.args);
            right_tool += usize::from(tool_ok);
            exact += usize::from(exact_ok);
            good = exact_ok;
            if !exact_ok {
                let want: Vec<String> = case
                    .accept
                    .iter()
                    .map(|(t, a)| format!("{t} {a:?}"))
                    .collect();
                misses.push(format!(
                    "#{} \"{}\": got {got}, wanted {}",
                    case.id,
                    case.request,
                    want.join(" or ")
                ));
            }
        }
        entry.0 += usize::from(good);
        rows.push(json!({"id": case.id, "category": case.category, "request": case.request, "got": got, "ok": good, "seconds": seconds}));
    }

    println!("model {}", chosen.id);
    println!(
        "act requests      {act_cases}: exact {:.0}% ({exact}), right tool {:.0}% ({right_tool})",
        percent(exact, act_cases),
        percent(right_tool, act_cases)
    );
    println!("decline requests  {decline_cases}: declined correctly {declined_ok}, acted instead {acted}");
    println!("invalid replies   {invalid}");
    println!(
        "median latency    {:.2} s (first request excluded)",
        median(latencies.clone())
    );
    println!("by category:");
    for (category, (good, total)) in &per_category {
        println!("  {category:<14} {good}/{total}");
    }
    if !misses.is_empty() {
        println!("misses:");
        for miss in &misses {
            println!("  {miss}");
        }
    }
    if !args.json_out.is_empty() {
        let doc = json!({
            "model": chosen.id, "cases_file": args.cases, "raw": args.raw,
            "act": {"total": act_cases, "exact": exact, "right_tool": right_tool},
            "decline": {"total": decline_cases, "declined": declined_ok, "acted": acted},
            "invalid": invalid, "median_seconds": median(latencies), "rows": rows,
        });
        std::fs::write(
            &args.json_out,
            serde_json::to_string_pretty(&doc).unwrap_or_default(),
        )
        .map_err(|e| format!("cannot write {}: {e}", args.json_out))?;
    }
    if let Some(min) = args.min_exact {
        if percent(exact, act_cases) < min {
            eprintln!(
                "exact match {:.1}% is below the required {min}%",
                percent(exact, act_cases)
            );
            return Ok(1);
        }
    }
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_checked_in_case_files_parse_and_name_real_tools() {
        let root = concat!(env!("CARGO_MANIFEST_DIR"), "/../..");
        let policy = RiskPolicy::embedded();
        for file in ["desktop_requests.json", "desktop_requests_heldout.json"] {
            let cases = load_cases(&format!("{root}/tests/eval/{file}")).expect(file);
            assert!(cases.len() > 50, "{file} has cases");
            assert!(
                cases.iter().any(|c| c.accept.is_empty()),
                "{file} has requests to decline"
            );
            for case in cases.iter().filter(|c| !c.accept.is_empty()) {
                for (tool, _) in &case.accept {
                    assert!(
                        policy.knows(tool),
                        "{file} #{}: '{tool}' is not a command",
                        case.id
                    );
                }
            }
        }
    }

    #[test]
    fn the_numbers_are_sane_on_empty_input() {
        assert_eq!(median(vec![]), 0.0);
        assert_eq!(median(vec![3.0, 1.0, 2.0]), 2.0);
        assert_eq!(percent(1, 0), 0.0);
        assert_eq!(percent(1, 4), 25.0);
    }
}
