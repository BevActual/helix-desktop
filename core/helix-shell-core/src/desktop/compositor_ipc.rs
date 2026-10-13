//! One-shot questions to the compositor (`$NIRI_SOCKET`).

use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::time::Duration;

use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct AgentDeskStatus {
    pub name: String,
    pub workspace_id: Option<u64>,
    pub window_count: u32,
    pub is_focused: bool,
    pub placements_pending: u32,
    pub available: bool,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct HelixCaps {
    pub liquid_glass: bool,
    pub agent_desk: bool,
}

pub fn request_json(line: &str) -> Option<Value> {
    let path = std::env::var_os("NIRI_SOCKET")?;
    let mut stream = UnixStream::connect(path).ok()?;
    stream.set_read_timeout(Some(Duration::from_secs(2))).ok()?;
    stream.set_write_timeout(Some(Duration::from_secs(2))).ok()?;
    stream.write_all(format!("{line}\n").as_bytes()).ok()?;
    let mut reply = String::new();
    BufReader::new(stream).read_line(&mut reply).ok()?;
    let value: Value = serde_json::from_str(&reply).ok()?;
    value.get("Ok").cloned()
}

pub fn helix_capabilities() -> HelixCaps {
    let Some(ok) = request_json("\"HelixCapabilities\"") else {
        return HelixCaps::default();
    };
    serde_json::from_value(ok).unwrap_or_default()
}

pub fn agent_desk_status() -> AgentDeskStatus {
    let Some(ok) = request_json("\"HelixAgentDesk\"") else {
        return AgentDeskStatus::default();
    };
    let mut status: AgentDeskStatus = serde_json::from_value(ok).unwrap_or_default();
    status.available = true;
    status
}

/// Ask the compositor to reload its config (picks up `helix-setup.kdl` changes).
pub fn reload_config() {
    let line = serde_json::json!({"Action": {"LoadConfigFile": {}}}).to_string();
    let _ = request_json(&line);
}
