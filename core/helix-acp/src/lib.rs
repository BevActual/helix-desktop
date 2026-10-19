//! A client for the Agent Client Protocol (ACP): a way for an application to drive a coding agent that runs as a subprocess.
//!
//! The application launches the agent and talks JSON-RPC 2.0 over its standard input and output, one message to a line. It starts a session
//! in a project folder, sends prompts, and receives what the agent does as events: its words and thoughts, the tools it calls, its plan. The
//! agent may call back to ask for a file, a terminal, or permission; the application's [`Handler`] decides each of those, so everything
//! the agent touches passes through the application. The agent keeps its own sign-in: nothing here reads an agent's credentials.
//!
//! Written from the protocol's published specification (Apache-2.0). `tests/` checks what this crate sends against the published schema.

mod client;
mod types;

pub use client::{
    BoxFuture, Client, Error, Event, Handler, RefuseAll, Spawn, MAX_AGENT_REQUESTS, MAX_LINE,
};
pub use types::*;
