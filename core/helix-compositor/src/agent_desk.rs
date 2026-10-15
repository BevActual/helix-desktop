//! Agent desk policy shared by the shell and Mobius (display workspace for agent windows).

use crate::niri::Niri;

/// Default named workspace the compositor creates for agent-side windows.
pub const DEFAULT_NAME: &str = "Helix Agent";

/// Whether agent-desk placement is enabled for this session.
pub fn placement_enabled() -> bool {
    crate::compositor_config::agent_desk_placement_enabled()
}

/// Live glance for the bar island when the agent desk has windows but is not the active workspace.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AgentDeskGlance {
    pub windows: u32,
    pub label: String,
}

pub fn glance_from_niri(niri: &Niri) -> Option<AgentDeskGlance> {
    if !placement_enabled() {
        return None;
    }
    for workspace in niri.workspace_strip() {
        if !workspace.agent_desk || workspace.windows == 0 || workspace.active {
            continue;
        }
        let label = if workspace.name.is_empty() {
            DEFAULT_NAME.to_string()
        } else {
            workspace.name.clone()
        };
        return Some(AgentDeskGlance {
            windows: workspace.windows as u32,
            label,
        });
    }
    None
}
