//! The person's own tool servers (MCP, stdio): programs they chose to add, which a task can call once the policy or the person allows each tool.
//! Mobius starts one for the length of a task, asks it what tools it has, and calls them; it never starts one the person did not add, and a server
//! gets a minimal environment (no tokens, no session sockets) unless the person gives it more on its own command line.

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::path::PathBuf;
use std::process::Stdio;
use std::time::Duration;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::process::{Child, ChildStdin, ChildStdout, Command};

pub const MAX_SERVERS: usize = 12;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Server {
    pub name: String,
    pub command: String,
    #[serde(default)]
    pub args: Vec<String>,
    #[serde(default = "yes")]
    pub enabled: bool,
}
fn yes() -> bool {
    true
}

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct Tool {
    pub name: String,
    pub description: String,
    /// The names of the arguments the tool takes, required ones first.
    pub arguments: Vec<String>,
}

fn path() -> PathBuf {
    crate::data_home().join("helix/mobius/mcp-servers.json")
}

pub fn servers() -> Vec<Server> {
    std::fs::read(path())
        .ok()
        .and_then(|b| serde_json::from_slice::<Vec<Server>>(&b).ok())
        .unwrap_or_default()
        .into_iter()
        .filter(|s| valid(s).is_ok())
        .collect()
}

pub fn valid(server: &Server) -> Result<(), String> {
    if !crate::skills::valid_name(&server.name) {
        return Err("Name the server in lower-case words joined with dashes.".into());
    }
    if server.command.trim().is_empty() || server.command.chars().count() > 300 {
        return Err("Give the program to run.".into());
    }
    crate::terminal::safe_input(&server.command, 300)?;
    if server.args.len() > 20 {
        return Err("A server takes at most 20 arguments.".into());
    }
    for arg in &server.args {
        crate::terminal::safe_input(arg, 400)?;
    }
    Ok(())
}

fn write(list: &[Server]) -> Result<(), String> {
    let path = path();
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let temp = path.with_extension("json.tmp");
    std::fs::write(
        &temp,
        serde_json::to_vec_pretty(list).map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())?;
    std::fs::rename(&temp, &path).map_err(|e| e.to_string())
}

pub struct Session {
    child: Child,
    stdin: ChildStdin,
    lines: tokio::io::Lines<BufReader<ChildStdout>>,
    next: u64,
}

impl Session {
    pub async fn start(server: &Server) -> Result<Self, String> {
        valid(server)?;
        let mut command = Command::new(&server.command);
        command
            .args(&server.args)
            .env_clear()
            .env("PATH", std::env::var_os("PATH").unwrap_or_default())
            .env("HOME", std::env::var_os("HOME").unwrap_or_default())
            .env("LANG", "C.UTF-8")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .kill_on_drop(true);
        let mut child = command
            .spawn()
            .map_err(|e| format!("Could not start {}: {e}", server.name))?;
        let stdin = child.stdin.take().ok_or("No input pipe.")?;
        let stdout = child.stdout.take().ok_or("No output pipe.")?;
        let mut session = Self {
            child,
            stdin,
            lines: BufReader::new(stdout).lines(),
            next: 1,
        };
        session
            .request("initialize", json!({"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "mobius", "version": "1"}}), 15)
            .await?;
        session
            .send(&json!({"jsonrpc": "2.0", "method": "notifications/initialized"}))
            .await?;
        Ok(session)
    }

    async fn send(&mut self, message: &Value) -> Result<(), String> {
        let mut line = message.to_string();
        line.push('\n');
        self.stdin
            .write_all(line.as_bytes())
            .await
            .map_err(|e| e.to_string())?;
        self.stdin.flush().await.map_err(|e| e.to_string())
    }

    /// One request and its answer; anything else the server says in the meantime (notifications, its own requests) is ignored.
    async fn request(
        &mut self,
        method: &str,
        params: Value,
        seconds: u64,
    ) -> Result<Value, String> {
        let id = self.next;
        self.next += 1;
        self.send(&json!({"jsonrpc": "2.0", "id": id, "method": method, "params": params}))
            .await?;
        let wait = async {
            loop {
                let line = self
                    .lines
                    .next_line()
                    .await
                    .map_err(|e| e.to_string())?
                    .ok_or("The tool server stopped.")?;
                let Ok(message) = serde_json::from_str::<Value>(&line) else {
                    continue;
                };
                if message.get("id") == Some(&json!(id)) && message.get("method").is_none() {
                    if let Some(error) = message.get("error") {
                        return Err(error
                            .get("message")
                            .and_then(|m| m.as_str())
                            .unwrap_or("The tool server refused.")
                            .to_string());
                    }
                    return Ok(message.get("result").cloned().unwrap_or(Value::Null));
                }
            }
        };
        tokio::time::timeout(Duration::from_secs(seconds), wait)
            .await
            .map_err(|_| "The tool server did not answer in time.".to_string())?
    }

    pub async fn tools(&mut self) -> Result<Vec<Tool>, String> {
        let result = self.request("tools/list", json!({}), 15).await?;
        Ok(result["tools"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(parse_tool)
            .take(60)
            .collect())
    }

    /// Calls a tool and returns its text (an error the tool reports comes back as `Err`).
    pub async fn call(
        &mut self,
        name: &str,
        arguments: Value,
        seconds: u64,
    ) -> Result<String, String> {
        let result = self
            .request(
                "tools/call",
                json!({"name": name, "arguments": arguments}),
                seconds,
            )
            .await?;
        let text: String = result["content"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(|c| (c["type"] == "text").then(|| c["text"].as_str().unwrap_or("")))
            .collect::<Vec<_>>()
            .join("\n");
        let text: String = text.chars().take(20_000).collect();
        if result["isError"] == true {
            Err(if text.is_empty() {
                "The tool reported an error.".into()
            } else {
                text
            })
        } else {
            Ok(text)
        }
    }

    pub async fn stop(mut self) {
        let _ = self.child.kill().await;
    }
}

fn parse_tool(value: &Value) -> Option<Tool> {
    let name = value["name"].as_str()?;
    if name.is_empty()
        || name.len() > 80
        || !name
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '-' | '.'))
    {
        return None;
    }
    let schema = &value["inputSchema"];
    let required: Vec<&str> = schema["required"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|r| r.as_str())
        .collect();
    let mut arguments: Vec<String> = required.iter().map(|r| format!("{r}*")).collect();
    for key in schema["properties"]
        .as_object()
        .into_iter()
        .flat_map(|p| p.keys())
    {
        if !required.contains(&key.as_str()) {
            arguments.push(key.clone());
        }
    }
    arguments.truncate(12);
    Some(Tool {
        name: name.into(),
        description: value["description"]
            .as_str()
            .unwrap_or("")
            .split_whitespace()
            .collect::<Vec<_>>()
            .join(" ")
            .chars()
            .take(160)
            .collect(),
        arguments,
    })
}

/// What a planner is told about the servers' tools.
pub fn index_text(found: &[(String, Vec<Tool>)]) -> String {
    let mut text = String::new();
    for (server, tools) in found {
        for tool in tools.iter().take(30) {
            text.push_str(&format!(
                "- {server}/{}({}): {}\n",
                tool.name,
                tool.arguments.join(", "),
                tool.description
            ));
        }
    }
    if text.is_empty() {
        return text;
    }
    format!("\n\nTool servers the person added. Call one with {{\"tool\":\"mcp\",\"server\":\"name\",\"name\":\"tool\",\"args\":{{...}}}}; * marks a required argument. Each call asks the person first unless they allowed it:\n{text}")
}

#[derive(Serialize)]
pub struct ServerView {
    #[serde(flatten)]
    server: Server,
    tools: Vec<Tool>,
    error: String,
}

#[tauri::command]
pub fn mcp_servers() -> Vec<Server> {
    servers()
}

#[tauri::command]
pub fn mcp_server_save(server: Server) -> Result<Vec<Server>, String> {
    valid(&server)?;
    let mut list = servers();
    match list.iter_mut().find(|s| s.name == server.name) {
        Some(existing) => *existing = server,
        None => {
            if list.len() >= MAX_SERVERS {
                return Err(
                    "There are already as many tool servers as Mobius keeps. Remove one first."
                        .into(),
                );
            }
            list.push(server);
        }
    }
    write(&list)?;
    Ok(list)
}

#[tauri::command]
pub fn mcp_server_delete(name: String) -> Result<Vec<Server>, String> {
    let mut list = servers();
    list.retain(|s| s.name != name);
    write(&list)?;
    Ok(list)
}

/// Starts the server once and lists what it offers, so the person sees what they are adding.
#[tauri::command]
pub async fn mcp_server_test(name: String) -> Result<Vec<Tool>, String> {
    let server = servers()
        .into_iter()
        .find(|s| s.name == name)
        .ok_or("That server is not set up.")?;
    let mut session = Session::start(&server).await?;
    let tools = session.tools().await;
    session.stop().await;
    tools
}

#[cfg(test)]
mod tests {
    use super::*;

    fn server(name: &str, command: &str) -> Server {
        Server {
            name: name.into(),
            command: command.into(),
            args: vec![],
            enabled: true,
        }
    }

    #[test]
    fn a_server_needs_a_slug_name_and_a_plain_command() {
        assert!(valid(&server("git-tools", "python3")).is_ok());
        assert!(valid(&server("Git", "python3")).is_err());
        assert!(valid(&server("../x", "python3")).is_err());
        assert!(valid(&server("a", "  ")).is_err());
        let mut many = server("a", "x");
        many.args = vec!["a".into(); 21];
        assert!(valid(&many).is_err());
        assert!(valid(&server("a", "echo \u{202e}hidden")).is_err());
    }

    #[test]
    fn tools_are_read_from_a_list_and_odd_ones_dropped() {
        let tool = parse_tool(&json!({"name": "add", "description": "Add  two\nnumbers", "inputSchema": {"required": ["a"], "properties": {"a": {}, "b": {}}}})).unwrap();
        assert_eq!(tool.description, "Add two numbers");
        assert_eq!(tool.arguments, ["a*", "b"]);
        assert!(parse_tool(&json!({"name": "bad name"})).is_none());
        assert!(parse_tool(&json!({"description": "no name"})).is_none());
        let index = index_text(&[("calc".into(), vec![tool])]);
        assert!(
            index.contains("- calc/add(a*, b): Add two numbers")
                && index.contains("\"tool\":\"mcp\"")
        );
        assert_eq!(index_text(&[]), "");
    }

    #[tokio::test]
    async fn a_session_lists_and_calls_the_tools_of_a_real_server() {
        let script = r#"
import json, sys
for line in sys.stdin:
    m = json.loads(line)
    if "id" not in m: continue
    if m["method"] == "initialize":
        r = {"protocolVersion": "2024-11-05", "capabilities": {"tools": {}}, "serverInfo": {"name": "t", "version": "1"}}
    elif m["method"] == "tools/list":
        r = {"tools": [{"name": "add", "description": "Adds", "inputSchema": {"required": ["a", "b"], "properties": {"a": {}, "b": {}}}}]}
    elif m["method"] == "tools/call":
        a = m["params"]["arguments"]
        if m["params"]["name"] == "add":
            r = {"content": [{"type": "text", "text": str(a["a"] + a["b"])}]}
        else:
            r = {"isError": True, "content": [{"type": "text", "text": "no such tool"}]}
    else:
        r = {}
    print(json.dumps({"jsonrpc": "2.0", "id": m["id"], "result": r}), flush=True)
"#;
        let file = std::env::temp_dir().join(format!("mobius-mcp-stub-{}.py", std::process::id()));
        std::fs::write(&file, script).unwrap();
        let server = Server {
            name: "calc".into(),
            command: "python3".into(),
            args: vec![file.to_string_lossy().to_string()],
            enabled: true,
        };
        let mut session = Session::start(&server).await.unwrap();
        let tools = session.tools().await.unwrap();
        assert_eq!(tools.len(), 1);
        assert_eq!(tools[0].arguments, ["a*", "b*"]);
        assert_eq!(
            session
                .call("add", json!({"a": 2, "b": 40}), 10)
                .await
                .unwrap(),
            "42"
        );
        assert_eq!(
            session.call("nope", json!({}), 10).await.unwrap_err(),
            "no such tool"
        );
        session.stop().await;
    }

    #[tokio::test]
    async fn a_server_that_is_not_there_is_an_error_not_a_hang() {
        assert!(Session::start(&server("ghost", "/nonexistent/program"))
            .await
            .is_err());
    }
}
