//! The messages of the Agent Client Protocol (version 1) that Mobius uses, as plain data.
//!
//! Only the subset Mobius needs is typed. Anything else an agent sends (an update type from a newer protocol, say) is kept as raw JSON, so
//! an agent that is ahead of this crate is not an error. Names follow the published schema; `_meta` is ignored.

use serde::{Deserialize, Serialize};
use serde_json::Value;

/// The protocol version this client speaks.
pub const PROTOCOL_VERSION: u32 = 1;

// ---- JSON-RPC -------------------------------------------------------------------------------------------------------------------------

/// An error answer, from either side.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct RpcError {
    pub code: i64,
    pub message: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub data: Option<Value>,
}

impl RpcError {
    pub const METHOD_NOT_FOUND: i64 = -32601;
    pub const INVALID_PARAMS: i64 = -32602;
    pub const INTERNAL: i64 = -32603;
    /// The protocol's code for "a resource the request named does not exist".
    pub const RESOURCE_NOT_FOUND: i64 = -32002;

    pub fn new(code: i64, message: impl Into<String>) -> RpcError {
        RpcError {
            code,
            message: message.into(),
            data: None,
        }
    }

    pub fn method_not_found(method: &str) -> RpcError {
        RpcError::new(
            Self::METHOD_NOT_FOUND,
            format!("Method not supported: {method}"),
        )
    }

    pub fn invalid_params(message: impl Into<String>) -> RpcError {
        RpcError::new(Self::INVALID_PARAMS, message)
    }

    /// The host refused, for a reason the person may be shown.
    pub fn refused(message: impl Into<String>) -> RpcError {
        RpcError::new(Self::INTERNAL, message)
    }
}

// ---- content --------------------------------------------------------------------------------------------------------------------------

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum ContentBlock {
    Text {
        text: String,
    },
    Image {
        data: String,
        #[serde(rename = "mimeType")]
        mime_type: String,
    },
    Audio {
        data: String,
        #[serde(rename = "mimeType")]
        mime_type: String,
    },
    ResourceLink {
        uri: String,
        name: String,
    },
    Resource {
        resource: Value,
    },
}

impl ContentBlock {
    pub fn text(text: impl Into<String>) -> ContentBlock {
        ContentBlock::Text { text: text.into() }
    }
}

// ---- starting up ----------------------------------------------------------------------------------------------------------------------

#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct FsCapabilities {
    #[serde(rename = "readTextFile")]
    pub read_text_file: bool,
    #[serde(rename = "writeTextFile")]
    pub write_text_file: bool,
}

/// What this client can do for the agent. The agent must not call what is not offered.
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct ClientCapabilities {
    pub fs: FsCapabilities,
    pub terminal: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct ClientInfo {
    pub name: String,
    pub title: String,
    pub version: String,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct InitializeRequest {
    #[serde(rename = "protocolVersion")]
    pub protocol_version: u32,
    #[serde(rename = "clientCapabilities")]
    pub client_capabilities: ClientCapabilities,
    #[serde(rename = "clientInfo")]
    pub client_info: ClientInfo,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct AgentInfo {
    #[serde(default)]
    pub name: String,
    #[serde(default)]
    pub title: Option<String>,
    #[serde(default)]
    pub version: String,
}

/// One way the agent can be signed in. Mobius only shows these; the agent runs its own sign-in.
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct AuthMethod {
    #[serde(default)]
    pub id: String,
    #[serde(default)]
    pub name: String,
    #[serde(default)]
    pub description: Option<String>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct InitializeResponse {
    #[serde(rename = "protocolVersion")]
    pub protocol_version: u32,
    #[serde(rename = "agentCapabilities", default)]
    pub agent_capabilities: Value,
    #[serde(rename = "authMethods", default)]
    pub auth_methods: Vec<AuthMethod>,
    #[serde(rename = "agentInfo", default)]
    pub agent_info: Option<AgentInfo>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct NewSessionRequest {
    pub cwd: String,
    #[serde(rename = "mcpServers")]
    pub mcp_servers: Vec<Value>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct NewSessionResponse {
    #[serde(rename = "sessionId")]
    pub session_id: String,
    #[serde(default)]
    pub modes: Option<Value>,
}

// ---- a turn ---------------------------------------------------------------------------------------------------------------------------

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct PromptRequest {
    #[serde(rename = "sessionId")]
    pub session_id: String,
    pub prompt: Vec<ContentBlock>,
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum StopReason {
    EndTurn,
    MaxTokens,
    MaxTurnRequests,
    Refusal,
    Cancelled,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct PromptResponse {
    #[serde(rename = "stopReason")]
    pub stop_reason: StopReason,
}

#[derive(Clone, Copy, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ToolKind {
    Read,
    Edit,
    Delete,
    Move,
    Search,
    Execute,
    Think,
    Fetch,
    SwitchMode,
    #[default]
    Other,
}

#[derive(Clone, Copy, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ToolStatus {
    #[default]
    Pending,
    InProgress,
    Completed,
    Failed,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct ToolLocation {
    pub path: String,
    #[serde(default)]
    pub line: Option<u32>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct ToolCall {
    #[serde(rename = "toolCallId")]
    pub id: String,
    pub title: String,
    #[serde(default)]
    pub kind: ToolKind,
    #[serde(default)]
    pub status: ToolStatus,
    #[serde(default)]
    pub locations: Vec<ToolLocation>,
    /// What the tool is doing, as the agent describes it: text, a diff or a terminal. Kept raw; the host shows what it understands.
    #[serde(default)]
    pub content: Vec<Value>,
    #[serde(rename = "rawInput", default)]
    pub raw_input: Option<Value>,
}

/// A change to a tool call already announced. Every field but the id is optional.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct ToolCallUpdate {
    #[serde(rename = "toolCallId")]
    pub id: String,
    #[serde(default)]
    pub title: Option<String>,
    #[serde(default)]
    pub kind: Option<ToolKind>,
    #[serde(default)]
    pub status: Option<ToolStatus>,
    #[serde(default)]
    pub locations: Option<Vec<ToolLocation>>,
    #[serde(default)]
    pub content: Option<Vec<Value>>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct PlanEntry {
    pub content: String,
    pub priority: String,
    pub status: String,
}

/// What an agent reports while it works on a prompt.
#[derive(Clone, Debug, PartialEq)]
pub enum SessionUpdate {
    AgentMessage(ContentBlock),
    AgentThought(ContentBlock),
    UserMessage(ContentBlock),
    ToolCall(ToolCall),
    ToolCallUpdate(ToolCallUpdate),
    Plan(Vec<PlanEntry>),
    Usage {
        used: u64,
        size: u64,
    },
    /// An update this crate does not type, kept whole.
    Other(Value),
}

impl SessionUpdate {
    /// Reads the `update` object of a `session/update` notification. Never fails: what cannot be read becomes [`SessionUpdate::Other`].
    pub fn from_value(value: &Value) -> SessionUpdate {
        let kind = value
            .get("sessionUpdate")
            .and_then(Value::as_str)
            .unwrap_or("");
        let chunk = |value: &Value| {
            value
                .get("content")
                .and_then(|c| serde_json::from_value::<ContentBlock>(c.clone()).ok())
        };
        let parsed = match kind {
            "agent_message_chunk" => chunk(value).map(SessionUpdate::AgentMessage),
            "agent_thought_chunk" => chunk(value).map(SessionUpdate::AgentThought),
            "user_message_chunk" => chunk(value).map(SessionUpdate::UserMessage),
            "tool_call" => serde_json::from_value(value.clone())
                .ok()
                .map(SessionUpdate::ToolCall),
            "tool_call_update" => serde_json::from_value(value.clone())
                .ok()
                .map(SessionUpdate::ToolCallUpdate),
            "plan" => value
                .get("entries")
                .and_then(|e| serde_json::from_value(e.clone()).ok())
                .map(SessionUpdate::Plan),
            "usage_update" => match (
                value.get("used").and_then(Value::as_u64),
                value.get("size").and_then(Value::as_u64),
            ) {
                (Some(used), Some(size)) => Some(SessionUpdate::Usage { used, size }),
                _ => None,
            },
            _ => None,
        };
        parsed.unwrap_or_else(|| SessionUpdate::Other(value.clone()))
    }
}

// ---- what the agent asks of the host --------------------------------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum PermissionKind {
    AllowOnce,
    AllowAlways,
    RejectOnce,
    RejectAlways,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct PermissionOption {
    #[serde(rename = "optionId")]
    pub id: String,
    pub name: String,
    pub kind: PermissionKind,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct RequestPermission {
    #[serde(rename = "sessionId")]
    pub session_id: String,
    #[serde(rename = "toolCall")]
    pub tool_call: ToolCallUpdate,
    pub options: Vec<PermissionOption>,
}

/// The person's answer to a permission request: one of the offered options, or none because the turn was cancelled.
#[derive(Clone, Debug, PartialEq)]
pub enum PermissionOutcome {
    Cancelled,
    Selected(String),
}

impl PermissionOutcome {
    pub fn to_value(&self) -> Value {
        match self {
            PermissionOutcome::Cancelled => {
                serde_json::json!({"outcome": {"outcome": "cancelled"}})
            }
            PermissionOutcome::Selected(id) => {
                serde_json::json!({"outcome": {"outcome": "selected", "optionId": id}})
            }
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct ReadTextFile {
    #[serde(rename = "sessionId")]
    pub session_id: String,
    pub path: String,
    #[serde(default)]
    pub line: Option<u32>,
    #[serde(default)]
    pub limit: Option<u32>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct WriteTextFile {
    #[serde(rename = "sessionId")]
    pub session_id: String,
    pub path: String,
    pub content: String,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct CreateTerminal {
    #[serde(rename = "sessionId")]
    pub session_id: String,
    pub command: String,
    #[serde(default)]
    pub args: Vec<String>,
    #[serde(default)]
    pub env: Vec<Value>,
    #[serde(default)]
    pub cwd: Option<String>,
    #[serde(rename = "outputByteLimit", default)]
    pub output_byte_limit: Option<u64>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct TerminalRef {
    #[serde(rename = "sessionId")]
    pub session_id: String,
    #[serde(rename = "terminalId")]
    pub terminal_id: String,
}

/// A request from the agent that the host must answer.
#[derive(Clone, Debug, PartialEq)]
pub enum AgentRequest {
    ReadTextFile(ReadTextFile),
    WriteTextFile(WriteTextFile),
    RequestPermission(RequestPermission),
    CreateTerminal(CreateTerminal),
    TerminalOutput(TerminalRef),
    WaitForTerminalExit(TerminalRef),
    KillTerminal(TerminalRef),
    ReleaseTerminal(TerminalRef),
}

impl AgentRequest {
    /// Reads a request by its method. `None` for a method this crate does not know, which the client answers as "not supported".
    pub fn parse(method: &str, params: &Value) -> Option<Result<AgentRequest, RpcError>> {
        fn read<T: serde::de::DeserializeOwned>(params: &Value) -> Result<T, RpcError> {
            serde_json::from_value(params.clone())
                .map_err(|e| RpcError::invalid_params(format!("Could not read the request: {e}")))
        }
        Some(match method {
            "fs/read_text_file" => read(params).map(AgentRequest::ReadTextFile),
            "fs/write_text_file" => read(params).map(AgentRequest::WriteTextFile),
            "session/request_permission" => read(params).map(AgentRequest::RequestPermission),
            "terminal/create" => read(params).map(AgentRequest::CreateTerminal),
            "terminal/output" => read(params).map(AgentRequest::TerminalOutput),
            "terminal/wait_for_exit" => read(params).map(AgentRequest::WaitForTerminalExit),
            "terminal/kill" => read(params).map(AgentRequest::KillTerminal),
            "terminal/release" => read(params).map(AgentRequest::ReleaseTerminal),
            _ => return None,
        })
    }
}
