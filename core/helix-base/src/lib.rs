//! Small leaf items every other Helix core crate shares. No dependencies.

pub mod display;

/// True inside a nested test session (`tools/nested/run.sh` sets `HELIX_NESTED_INNER`). The test session has its own bus and compositor but
/// shares the real user's systemd and login manager, so anything that would reach those (start or restart a unit, suspend, power off)
/// must not happen from it.
pub fn nested_test_session() -> bool {
    std::env::var_os("HELIX_NESTED_INNER").is_some()
}

/// Panels the page knows how to show. A command naming anything else is refused.
pub const PANELS: &[&str] = &[
    "assistant",
    "launcher",
    "control-center",
    "agents",
    "notifications",
    "quick-insert",
];

/// One window as the agent may hear of it: no title (a title is the program's own text), and an application name only the desktop entry or the
/// sanitized program name supplies.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WindowFact {
    pub id: u64,
    pub app: String,
    /// An installed application, and really that application.
    pub known: bool,
    pub focused: bool,
}
