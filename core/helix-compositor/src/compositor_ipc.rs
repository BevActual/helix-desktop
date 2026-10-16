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
    stream
        .set_write_timeout(Some(Duration::from_secs(2)))
        .ok()?;
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

/// True when the compositor answers `HelixCapabilities` (helix-niri, not stock niri).
pub fn is_helix_compositor() -> bool {
    request_json("\"HelixCapabilities\"").is_some()
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

/// Queue the next opened window(s) on the Helix agent desk without stealing focus.
pub fn place_on_agent_desk(count: u8) -> Result<(), String> {
    if !crate::compositor_config::agent_desk_placement_enabled() {
        return Ok(());
    }
    if !helix_capabilities().agent_desk {
        return Err("agent desk is not available on this compositor".into());
    }
    let line = serde_json::json!({"Action": {"HelixPlaceOnAgentDesk": {"count": count.max(1)}}})
        .to_string();
    if request_json(&line).is_none() {
        return Err("the compositor did not accept agent desk placement".into());
    }
    Ok(())
}

/// Switch keyboard focus to the agent desk workspace.
pub fn focus_agent_desk() -> Result<(), String> {
    if !helix_capabilities().agent_desk {
        return Err("agent desk is not available on this compositor".into());
    }
    let line = serde_json::json!({"Action": {"HelixFocusAgentDesk": {}}}).to_string();
    if request_json(&line).is_none() {
        return Err("the compositor did not focus the agent desk".into());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn focus_fails_when_compositor_does_not_report_agent_desk() {
        std::env::remove_var("NIRI_SOCKET");
        let err = focus_agent_desk().unwrap_err();
        assert!(err.contains("agent desk"));
    }

    #[test]
    fn place_is_noop_when_placement_pref_is_off() {
        std::env::remove_var("NIRI_SOCKET");
        let root = std::env::temp_dir().join(format!("helix-place-off-{}", std::process::id()));
        let helix = root.join("helix");
        std::fs::create_dir_all(&helix).unwrap();
        std::fs::write(
            helix.join("compositor-prefs.toml"),
            "agent_desk_placement = false\n",
        )
        .unwrap();
        let prev_cfg = std::env::var("XDG_CONFIG_HOME").ok();
        let prev_env = std::env::var("HELIX_AGENT_DESK").ok();
        std::env::set_var("XDG_CONFIG_HOME", root.to_string_lossy().as_ref());
        std::env::remove_var("HELIX_AGENT_DESK");
        assert!(place_on_agent_desk(1).is_ok());
        if let Some(prev) = prev_cfg {
            std::env::set_var("XDG_CONFIG_HOME", prev);
        } else {
            std::env::remove_var("XDG_CONFIG_HOME");
        }
        if let Some(prev) = prev_env {
            std::env::set_var("HELIX_AGENT_DESK", prev);
        }
        std::fs::remove_dir_all(root).ok();
    }
}
