//! JSON Schema and prompt catalog of the desktop tool calls a client may make (port of the former C++
//! `helix::agent::toolCallSchema` / `toolCatalog`). Built from the msg command schema and the risk policy, the same two
//! sources the service enforces, so a model constrained by it can only name real commands. It narrows what a model can say;
//! the service still decides what is allowed.
//!
//! `msg_schema.json` is a dump of the msg command table (`src/cli/schema_msg.h`) in the shape `src/cli/schema_json.cpp` emits.

use std::collections::{BTreeMap, BTreeSet};

use serde::Deserialize;
use serde_json::{json, Value};

use super::risk::{RiskPolicy, Tier};

pub const DECLINE_TOOL: &str = "decline";
const MAX_ARG_CHARS: u64 = 500;

#[derive(Debug, Clone, Deserialize)]
pub struct Positional {
    pub name: String,
    #[serde(default)]
    pub required: bool,
    #[serde(default)]
    pub variadic: bool,
    #[serde(default, rename = "joinRemaining")]
    pub join_remaining: bool,
    #[serde(default)]
    pub choices: Vec<String>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct Command {
    pub name: String,
    #[serde(default)]
    pub summary: String,
    #[serde(default)]
    pub positionals: Vec<Positional>,
    #[serde(default)]
    pub subcommands: Vec<Command>,
}

#[derive(Deserialize)]
struct Root {
    commands: Vec<Command>,
}

/// The msg commands, in the order the CLI table lists them.
pub fn msg_commands() -> Vec<Command> {
    let root: Root = serde_json::from_str(include_str!("msg_schema.json")).expect("bundled msg_schema.json");
    root.commands
}

/// `protocols/ipc/tool-hints.toml`: a person's-words one-liner per tool. Descriptions only: they never affect what a tool may do.
#[derive(Debug, Clone, Default)]
pub struct ToolHints {
    hints: BTreeMap<String, String>,
}

impl ToolHints {
    pub fn parse(text: &str) -> Result<Self, String> {
        let table: toml::Table = text.parse().map_err(|error| format!("tool hints: {error}"))?;
        let section = table
            .get("hints")
            .and_then(toml::Value::as_table)
            .ok_or("tool hints: missing [hints]")?;
        let mut hints = BTreeMap::new();
        for (key, value) in section {
            match value.as_str() {
                Some(text) if !text.is_empty() => {
                    hints.insert(key.clone(), text.to_string());
                }
                _ => return Err(format!("tool hints: '{key}' must be a non-empty string")),
            }
        }
        Ok(Self { hints })
    }

    pub fn load(path: &std::path::Path) -> Result<Self, String> {
        let text = std::fs::read_to_string(path)
            .map_err(|_| format!("tool hints: cannot read {}", path.display()))?;
        Self::parse(&text)
    }

    pub fn embedded() -> Self {
        Self::parse(include_str!("../../../../protocols/ipc/tool-hints.toml")).expect("bundled tool-hints.toml")
    }

    pub fn find(&self, command: &str) -> Option<&str> {
        self.hints.get(command).map(String::as_str)
    }
}

/// Optional free text is something a model copies from the signature as a placeholder, so only required arguments, enums and
/// these few meaningful optionals are offered.
fn meaningful_optional(command: &str, positional: &str) -> bool {
    matches!(
        (command, positional),
        ("panel-open", "context") | ("panel-toggle", "context") | ("settings-open", "context") | ("settings-toggle", "context")
    )
}

fn offered_positionals(command: &Command) -> Vec<&Positional> {
    command
        .positionals
        .iter()
        .filter(|p| p.required || !p.choices.is_empty() || meaningful_optional(&command.name, &p.name))
        .collect()
}

fn string_schema(choices: &[String], allowed: Option<&BTreeSet<String>>) -> Value {
    if choices.is_empty() && allowed.is_none() {
        return json!({"type": "string", "minLength": 1, "maxLength": MAX_ARG_CHARS});
    }
    let mut values: Vec<Value> = choices
        .iter()
        .filter(|choice| allowed.is_none_or(|set| set.contains(*choice)))
        .map(|choice| Value::String(choice.clone()))
        .collect();
    if choices.is_empty() {
        if let Some(set) = allowed {
            values.extend(set.iter().cloned().map(Value::String));
        }
    }
    json!({"type": "string", "enum": values})
}

fn call_schema(command: &Command, allowed_first: Option<&BTreeSet<String>>) -> Value {
    let mut prefix: Vec<Value> = Vec::new();
    let mut required = 0usize;
    let mut open = false;
    if !command.subcommands.is_empty() {
        let names: BTreeSet<String> = command.subcommands.iter().map(|s| s.name.clone()).collect();
        prefix.push(string_schema(&[], Some(allowed_first.unwrap_or(&names))));
        required = 1;
        open = true;
    } else {
        for (i, positional) in offered_positionals(command).into_iter().enumerate() {
            prefix.push(string_schema(
                &positional.choices,
                if i == 0 { allowed_first } else { None },
            ));
            if positional.required {
                required = i + 1;
            }
            if positional.variadic || positional.join_remaining {
                open = true;
            }
        }
    }
    let mut args = json!({"type": "array", "minItems": required});
    if !prefix.is_empty() {
        args["prefixItems"] = Value::Array(prefix.clone());
    }
    args["items"] = if open { string_schema(&[], None) } else { Value::Bool(false) };
    if !open {
        args["maxItems"] = json!(prefix.len());
    }
    json!({
        "type": "object",
        "properties": {
            "tool": {"const": command.name},
            "args": args,
            "reason": {"type": "string", "maxLength": 200},
        },
        "required": ["tool", "args"],
        "additionalProperties": false,
    })
}

/// Choices of a command that is blocked as a whole but has granted choices (`session lock`).
fn granted_choices(policy: &RiskPolicy, name: &str, tiers: &BTreeSet<Tier>) -> BTreeSet<String> {
    policy
        .choice_tiers(name)
        .into_iter()
        .filter(|(_, tier)| tiers.contains(tier))
        .map(|(choice, _)| choice)
        .collect()
}

pub fn tool_call_schema(
    commands: &[Command],
    policy: &RiskPolicy,
    tiers: &BTreeSet<Tier>,
    only_tools: Option<&BTreeSet<String>>,
    include_decline: bool,
) -> Value {
    let mut alternatives: Vec<Value> = Vec::new();
    for command in commands {
        let name = &command.name;
        if !policy.knows(name) || only_tools.is_some_and(|only| !only.contains(name)) {
            continue;
        }
        if tiers.contains(&policy.tier_for(name, &[])) {
            alternatives.push(call_schema(command, None));
            continue;
        }
        let granted = granted_choices(policy, name, tiers);
        if !granted.is_empty() {
            alternatives.push(call_schema(command, Some(&granted)));
        }
    }
    if include_decline {
        alternatives.push(json!({
            "type": "object",
            "properties": {
                "tool": {"const": DECLINE_TOOL},
                "args": {"type": "array", "minItems": 0, "maxItems": 0, "items": false},
                "reason": {"type": "string", "minLength": 1, "maxLength": 200},
            },
            "required": ["tool", "args", "reason"],
            "additionalProperties": false,
        }));
    }
    json!({
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "title": "Helix desktop tool call",
        "oneOf": alternatives,
    })
}

/// One line per tool, for a model's system prompt; lists exactly the tools [`tool_call_schema`] offers for `tiers`.
pub fn tool_catalog(
    commands: &[Command],
    policy: &RiskPolicy,
    tiers: &BTreeSet<Tier>,
    only_tools: Option<&BTreeSet<String>>,
    include_decline: bool,
    hints: Option<&ToolHints>,
) -> String {
    let mut out = String::new();
    for command in commands {
        let name = &command.name;
        if !policy.knows(name) || only_tools.is_some_and(|only| !only.contains(name)) {
            continue;
        }
        let whole = tiers.contains(&policy.tier_for(name, &[]));
        let granted = if whole {
            BTreeSet::new()
        } else {
            let granted = granted_choices(policy, name, tiers);
            if granted.is_empty() {
                continue;
            }
            granted
        };
        out.push_str("- ");
        out.push_str(name);
        for (i, positional) in offered_positionals(command).into_iter().enumerate() {
            let mut label = positional.name.clone();
            if i == 0 && !whole {
                label.push_str(": ");
                label.push_str(&granted.iter().cloned().collect::<Vec<_>>().join("|"));
            } else if !positional.choices.is_empty() {
                label.push_str(": ");
                label.push_str(&positional.choices.join("|"));
            }
            if positional.required {
                out.push_str(&format!(" <{label}>"));
            } else {
                out.push_str(&format!(" [{label}]"));
            }
        }
        if !command.subcommands.is_empty() {
            let names: Vec<&str> = command.subcommands.iter().map(|s| s.name.as_str()).collect();
            out.push_str(&format!(" <{}> ...", names.join("|")));
        }
        match hints.and_then(|h| h.find(name)) {
            Some(hint) => {
                out.push_str(" - ");
                out.push_str(hint);
            }
            None if !command.summary.is_empty() => {
                out.push_str(" - ");
                out.push_str(&command.summary);
            }
            None => {}
        }
        out.push('\n');
    }
    if include_decline {
        out.push_str("- decline - Use this when none of the other tools can do what was asked, or it would be unsafe. Put the reason in \"reason\".\n");
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tiers(list: &[Tier]) -> BTreeSet<Tier> {
        list.iter().copied().collect()
    }

    /// The C++ implementation's output (`tests/golden/tool_schema.json`) is the reference.
    #[test]
    fn matches_the_cpp_goldens() {
        let golden: Vec<Value> =
            serde_json::from_str(include_str!("../../tests/golden/tool_schema.json")).unwrap();
        let policy = RiskPolicy::embedded();
        let hints = ToolHints::embedded();
        let commands = msg_commands();
        let cases: [(&str, BTreeSet<Tier>, bool, bool); 4] = [
            ("observe", tiers(&[Tier::Observe]), false, false),
            (
                "observe+act_decline_hints",
                tiers(&[Tier::Observe, Tier::ObservePrivate, Tier::Act]),
                true,
                true,
            ),
            (
                "all_decline",
                tiers(&[Tier::Observe, Tier::ObservePrivate, Tier::Act, Tier::Confirm]),
                true,
                false,
            ),
            ("blocked_only", tiers(&[Tier::Blocked]), false, false),
        ];
        assert_eq!(golden.len(), cases.len());
        for (entry, (name, set, decline, use_hints)) in golden.iter().zip(cases) {
            assert_eq!(entry["name"], name);
            let schema = tool_call_schema(&commands, &policy, &set, None, decline);
            assert_eq!(schema, entry["schema"], "schema for {name}");
            let catalog = tool_catalog(&commands, &policy, &set, None, decline, use_hints.then_some(&hints));
            assert_eq!(catalog, entry["catalog"].as_str().unwrap(), "catalog for {name}");
        }
    }
}
