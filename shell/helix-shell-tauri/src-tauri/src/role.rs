//! Which half of the split shell this process is.
//!
//! - **Monolith** (default): legacy single surface; bar+panels in one process.
//! - **Bar**: Top layer only; forwards panel open/close to `helix-shell-panels`.
//! - **Panels**: Overlay only; serves the panels IPC socket.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShellRole {
    Monolith,
    Bar,
    Panels,
}

impl ShellRole {
    pub fn from_env() -> Self {
        match std::env::var("HELIX_SHELL_ROLE")
            .unwrap_or_default()
            .to_ascii_lowercase()
            .as_str()
        {
            "bar" => Self::Bar,
            "panels" | "panel" => Self::Panels,
            _ => Self::Monolith,
        }
    }

    pub fn is_bar_side(self) -> bool {
        matches!(self, Self::Bar | Self::Monolith)
    }

    pub fn claims_shell1(self) -> bool {
        matches!(self, Self::Bar | Self::Monolith)
    }

    pub fn serves_panels_socket(self) -> bool {
        matches!(self, Self::Panels)
    }

    pub fn forwards_panels(self) -> bool {
        matches!(self, Self::Bar)
    }
}
