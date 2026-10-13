//! Desktop capture target resolution for Agent Surface `desktop-capture`.

use crate::niri::Niri;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CaptureScope {
    /// A window on the configured agent desk workspace.
    AgentDesk,
    /// The output that currently has keyboard focus.
    FocusedOutput,
    /// A specific connector name (e.g. `DP-1`).
    Output(String),
    /// Every output (callers may stitch; shell may fall back to focused until stitching exists).
    AllOutputs,
}

/// How agents should capture when `desktop-capture` is called with no target.
pub fn default_scope(agent_desk_capable: bool, placement_enabled: bool) -> CaptureScope {
    if agent_desk_capable && placement_enabled {
        CaptureScope::AgentDesk
    } else {
        CaptureScope::FocusedOutput
    }
}

pub fn resolve_scope(
    target: Option<&str>,
    agent_desk_capable: bool,
    placement_enabled: bool,
) -> CaptureScope {
    let trimmed = target.unwrap_or("").trim();
    if trimmed.is_empty() {
        return default_scope(agent_desk_capable, placement_enabled);
    }
    let lower = trimmed.to_ascii_lowercase();
    match lower.as_str() {
        "agent-desk" | "agent" | "desk" => CaptureScope::AgentDesk,
        "all" => CaptureScope::AllOutputs,
        "focused" | "output" => CaptureScope::FocusedOutput,
        name if is_connector_name(name) => CaptureScope::Output(trimmed.to_string()),
        _ => CaptureScope::FocusedOutput,
    }
}

fn is_connector_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 32
        && name
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
}

/// Prefer a focused window on the desk, otherwise the highest window id (stable “top” choice).
pub fn pick_agent_desk_window(niri: &Niri, workspace_id: u64) -> Option<u64> {
    let on_desk: Vec<_> = niri
        .windows()
        .filter(|w| w.workspace == Some(workspace_id))
        .collect();
    if on_desk.is_empty() {
        return None;
    }
    if let Some(focused) = on_desk.iter().find(|w| w.focused) {
        return Some(focused.id);
    }
    on_desk.iter().map(|w| w.id).max()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_prefers_agent_desk_when_enabled() {
        assert_eq!(
            default_scope(true, true),
            CaptureScope::AgentDesk
        );
        assert_eq!(
            default_scope(true, false),
            CaptureScope::FocusedOutput
        );
    }

    #[test]
    fn parses_targets() {
        assert_eq!(
            resolve_scope(None, true, true),
            CaptureScope::AgentDesk
        );
        assert_eq!(
            resolve_scope(Some("agent-desk"), false, false),
            CaptureScope::AgentDesk
        );
        assert_eq!(resolve_scope(Some("all"), true, true), CaptureScope::AllOutputs);
        assert_eq!(
            resolve_scope(Some("DP-1"), true, true),
            CaptureScope::Output("DP-1".into())
        );
    }

    #[test]
    fn picks_focused_window_on_desk() {
        let mut niri = Niri::default();
        niri.apply(&serde_json::json!({
            "WindowsChanged": {"windows": [
                {"id": 1, "workspace_id": 9, "is_focused": false},
                {"id": 2, "workspace_id": 9, "is_focused": true},
                {"id": 3, "workspace_id": 8, "is_focused": false},
            ]}
        }));
        assert_eq!(pick_agent_desk_window(&niri, 9), Some(2));
    }
}
