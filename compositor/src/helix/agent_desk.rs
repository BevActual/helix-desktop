//! Helix agent desk: a named workspace for agent-side windows.

use niri_config::workspace::WorkspaceName;
use niri_config::{Workspace, WorkspaceReference};
use niri_ipc::HelixAgentDesk;

use crate::layout::workspace::WorkspaceId;
use crate::niri::State;

impl State {
    pub fn helix_agent_desk_name(&self) -> String {
        self.niri.config.borrow().helix.agent_desk_name.clone()
    }

    pub fn helix_ensure_agent_desk_workspace(&mut self) {
        let name = self.helix_agent_desk_name();
        if name.is_empty() {
            return;
        }
        let ws = Workspace {
            name: WorkspaceName(name),
            open_on_output: None,
            layout: None,
        };
        self.niri.layout.ensure_named_workspace(&ws);
    }

    pub fn helix_agent_desk_workspace_id(&self) -> Option<WorkspaceId> {
        let name = self.helix_agent_desk_name();
        if name.is_empty() {
            return None;
        }
        self.niri
            .layout
            .find_workspace_by_name(&name)
            .map(|(_, ws)| ws.id())
    }

    pub fn helix_place_on_agent_desk(&mut self, count: u32) {
        self.helix_ensure_agent_desk_workspace();
        self.niri.helix_place_on_agent_desk += count;
    }

    pub fn helix_focus_agent_desk(&mut self) {
        self.helix_ensure_agent_desk_workspace();
        let name = self.helix_agent_desk_name();
        if name.is_empty() {
            return;
        }
        self.do_action(
            niri_config::Action::FocusWorkspace(WorkspaceReference::Name(name)),
            false,
        );
    }

    pub fn helix_agent_desk_is_named(&self, workspace_name: Option<&str>) -> bool {
        let expected = self.helix_agent_desk_name();
        !expected.is_empty() && workspace_name == Some(expected.as_str())
    }

    pub fn helix_agent_desk_info(&self) -> HelixAgentDesk {
        let name = self.helix_agent_desk_name();
        let workspace_id = self.helix_agent_desk_workspace_id().map(|ws| ws.get());
        let mut window_count = 0u32;
        self.niri.layout.with_windows(|_, _, ws_id, _| {
            if ws_id.map(|w| w.get()) == workspace_id {
                window_count += 1;
            }
        });
        let focused_ws_id = self.niri.layout.active_workspace().map(|ws| ws.id().get());
        let is_focused = workspace_id.is_some_and(|id| focused_ws_id == Some(id));
        HelixAgentDesk {
            name,
            workspace_id,
            window_count,
            is_focused,
            placements_pending: self.niri.helix_place_on_agent_desk,
        }
    }
}
