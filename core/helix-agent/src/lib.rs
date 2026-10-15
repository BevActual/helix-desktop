//! Agent Surface policy for the shell side: risk tiers, grants, confirmation, the assistant session and tool schema.

pub mod activity;
pub mod agent_confirm;
pub mod agent_msg;
pub mod agent_status;
pub mod approvals;
pub mod assistant;
pub mod assistant_core;
pub mod audit_log;
pub mod change_ledger;
pub mod confirm;
pub mod confirmer;
pub mod grant_store;
pub mod grants;
pub mod peer;
pub mod risk;
pub mod shell_ipc;
pub mod tool_schema;

// Modules that live in other crates but are named `crate::<module>` throughout this crate.
pub use helix_base::display;
