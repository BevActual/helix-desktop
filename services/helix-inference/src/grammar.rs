//! Converts the tool-call schema from the desktop (`{"oneOf": [ {properties: {tool: {const}, args: {prefixItems, minItems, items}, reason}} ]}`)
//! into a GBNF grammar, so the engine can only produce text that conforms to it. Port of `native/inference/schema_grammar.cpp`.
//! Only the subset that schema uses is supported (array items are plain strings or string enums); anything else is an error rather than a silently
//! wrong grammar. The output is compact JSON in a fixed key order, `{"tool":"<name>","args":[...]}` with an optional `,"reason":"<text>"`. Free-text
//! arguments are non-empty and bounded (1 to 120 characters, a reason up to 200), so a model stuck inside a string cannot run on to the token limit.

use serde_json::Value;

/// `{"reason": ..., "tool": ..., "args": ...}` instead of `{"tool": ..., "args": ..., "reason": ...}`: a reason written before the tool can act as a
/// short chain of thought. It is then required and kept short (1 to 160 characters).
#[derive(Clone, Copy, Debug, Default)]
pub struct Options {
    pub reason_first: bool,
}

/// A GBNF double-quoted literal containing exactly `text`.
fn literal(text: &str) -> String {
    let mut out = String::from("\"");
    for ch in text.chars() {
        if ch == '"' || ch == '\\' {
            out.push('\\');
        }
        out.push(ch);
    }
    out.push('"');
    out
}

/// One array element: a JSON string, either any string or one of a fixed set.
fn item_rule(item: &Value) -> Result<String, String> {
    if let Some(values) = item.get("enum") {
        let mut alternatives = Vec::new();
        for value in values.as_array().ok_or("an enum is a list")? {
            let text = value.as_str().ok_or("only string enums are supported")?;
            alternatives.push(literal(text));
        }
        if alternatives.is_empty() {
            return Err("an enum with no values can never be satisfied".into());
        }
        return Ok(format!("\"\\\"\" ({}) \"\\\"\"", alternatives.join(" | ")));
    }
    if item.get("type").and_then(Value::as_str) == Some("string") {
        return Ok("str-nonempty".into());
    }
    Err("array items must be strings or string enums".into())
}

fn rule_name(tool: &str) -> Result<String, String> {
    let mut name = String::from("call-");
    for ch in tool.chars() {
        if !(ch.is_ascii_lowercase() || ch.is_ascii_digit() || ch == '-') {
            return Err("tool names must be lowercase letters, digits and hyphens".into());
        }
        name.push(ch);
    }
    Ok(name)
}

/// `args` for one tool: every allowed element count from `minItems` up to the number of declared positions, and, when the schema allows further
/// items, any number of extra strings after the declared ones.
fn args_rule(args: &Value) -> Result<String, String> {
    let prefix: Vec<Value> = args
        .get("prefixItems")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    let min_items = args.get("minItems").and_then(Value::as_u64).unwrap_or(0) as usize;
    let open = args.get("items").is_some_and(|i| !i.is_boolean());
    if min_items > prefix.len() && !open {
        return Err("minItems exceeds the declared positions".into());
    }
    let mut alternatives = Vec::new();
    for count in min_items..=prefix.len() {
        let mut body = String::from("\"[\" ws");
        for (i, item) in prefix.iter().take(count).enumerate() {
            body.push_str(if i == 0 { " " } else { " \",\" ws " });
            body.push_str(&item_rule(item)?);
        }
        if open && count == prefix.len() {
            body.push_str(if count == 0 {
                " (str-nonempty (\",\" ws str-nonempty)*)?"
            } else {
                " (\",\" ws str-nonempty)*"
            });
        }
        body.push_str(" ws \"]\"");
        alternatives.push(body);
    }
    Ok(alternatives.join(" | "))
}

pub fn from_tool_schema(schema: &Value, options: Options) -> Result<String, String> {
    let one_of = schema
        .get("oneOf")
        .and_then(Value::as_array)
        .ok_or("expected a oneOf of tool calls")?;
    let (mut calls, mut rules) = (Vec::new(), String::new());
    for alternative in one_of {
        let properties = alternative
            .get("properties")
            .ok_or("a tool call has properties")?;
        let tool = properties
            .get("tool")
            .and_then(|t| t.get("const"))
            .and_then(Value::as_str)
            .ok_or("a tool call names its tool")?;
        let name = rule_name(tool)?;
        if calls.contains(&name) {
            return Err(format!("duplicate tool: {tool}"));
        }
        let head = "\"{\" ws ";
        let tool_key = format!(
            "\"\\\"tool\\\"\" ws \":\" ws {}",
            literal(&format!("\"{tool}\""))
        );
        let args_key = format!(
            "\"\\\"args\\\"\" ws \":\" ws ({})",
            args_rule(properties.get("args").ok_or("a tool call has args")?)?
        );
        let reason_key = "\"\\\"reason\\\"\" ws \":\" ws ";
        if options.reason_first {
            // The reason comes first and is always present, so it is written before the model commits to a tool.
            rules.push_str(&format!(
                "{name} ::= {head}{reason_key}reason-short ws \",\" ws {tool_key} ws \",\" ws {args_key} ws \"}}\"\n"
            ));
        } else {
            // A tool whose schema requires `reason` must carry it; otherwise it is optional.
            let required = alternative
                .get("required")
                .and_then(Value::as_array)
                .is_some_and(|keys| keys.iter().any(|k| k == "reason"));
            let reason = if required {
                format!("ws \",\" ws {reason_key}str-nonempty")
            } else {
                "reason".to_string()
            };
            rules.push_str(&format!(
                "{name} ::= {head}{tool_key} ws \",\" ws {args_key} {reason} ws \"}}\"\n"
            ));
        }
        calls.push(name);
    }
    if calls.is_empty() {
        return Err("no tools to choose from".into());
    }
    Ok(format!(
        "root ::= call\ncall ::= {}\n{rules}reason ::= (ws \",\" ws \"\\\"reason\\\"\" ws \":\" ws str)?\n\
         str ::= \"\\\"\" char{{0,200}} \"\\\"\"\n\
         str-nonempty ::= \"\\\"\" char{{1,120}} \"\\\"\"\n\
         reason-short ::= \"\\\"\" char{{1,160}} \"\\\"\"\n\
         char ::= [^\"\\\\\\x7F\\x00-\\x1F] | \"\\\\\" ([\"\\\\/bfnrt] | \"u\" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F])\n\
         ws ::= [ ]?\n",
        calls.join(" | ")
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn schema() -> Value {
        json!({"oneOf": [
            {"type": "object", "required": ["tool", "args"], "properties": {
                "tool": {"const": "open-app"},
                "args": {"prefixItems": [{"enum": ["files", "terminal"]}], "minItems": 1},
                "reason": {"type": "string"}}},
            {"type": "object", "required": ["tool", "args", "reason"], "properties": {
                "tool": {"const": "type-text"},
                "args": {"prefixItems": [{"type": "string"}], "minItems": 0, "items": {"type": "string"}}}}
        ]})
    }

    #[test]
    fn a_tool_schema_becomes_a_grammar() {
        let g = from_tool_schema(&schema(), Options::default()).unwrap();
        assert!(g.starts_with("root ::= call\ncall ::= call-open-app | call-type-text\n"));
        // An enum becomes alternatives of quoted literals; free text is bounded and non-empty.
        assert!(g.contains(r#""\"" ("files" | "terminal") "\"""#));
        assert!(g.contains("str-nonempty"));
        // The first tool's reason is optional, the second's is required.
        assert!(g.contains("call-open-app ::= \"{\" ws \"\\\"tool\\\"\" ws \":\" ws \"\\\"open-app\\\"\" ws \",\" ws \"\\\"args\\\"\" ws \":\" ws (\"[\" ws \"\\\"\" (\"files\" | \"terminal\") \"\\\"\" ws \"]\") reason ws \"}\"\n"));
        assert!(g.contains("ws \",\" ws \"\\\"reason\\\"\" ws \":\" ws str-nonempty ws \"}\""));
        // The second allows extra strings after the declared position.
        assert!(g.contains("(\",\" ws str-nonempty)*"));
        assert!(g.ends_with("ws ::= [ ]?\n"));
    }

    #[test]
    fn reason_first_puts_the_reason_before_the_tool() {
        let g = from_tool_schema(&schema(), Options { reason_first: true }).unwrap();
        let line = g.lines().find(|l| l.starts_with("call-open-app")).unwrap();
        let reason = line.find("reason-short").unwrap();
        let tool = line.find("\\\"tool\\\"").unwrap();
        assert!(reason < tool);
    }

    #[test]
    fn what_cannot_be_constrained_is_refused() {
        let bad = |v: Value| from_tool_schema(&v, Options::default()).unwrap_err();
        assert!(bad(json!({})).contains("oneOf"));
        assert!(bad(json!({"oneOf": []})).contains("no tools"));
        let tool = |name: &str, args: Value| json!({"oneOf": [{"properties": {"tool": {"const": name}, "args": args}}]});
        assert!(bad(tool("Bad Name", json!({}))).contains("lowercase"));
        assert!(bad(tool("x", json!({"prefixItems": [{"type": "integer"}]}))).contains("strings"));
        assert!(bad(tool("x", json!({"prefixItems": [{"enum": []}]}))).contains("never"));
        assert!(bad(tool("x", json!({"prefixItems": [{"enum": [1]}]}))).contains("string enums"));
        assert!(bad(tool("x", json!({"minItems": 2}))).contains("minItems"));
        let dup = json!({"oneOf": [
            {"properties": {"tool": {"const": "x"}, "args": {}}},
            {"properties": {"tool": {"const": "x"}, "args": {}}}]});
        assert!(bad(dup).contains("duplicate"));
    }
}
