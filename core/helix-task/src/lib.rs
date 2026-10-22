//! Agent tasks. A task is a goal, the policy it runs under and a record of what it did; it outlives the chat window it was started from.
//!
//! The policy is the answer to "what may this task do without asking me?" Everything outside it asks (or is refused), so an unattended task can
//! work inside a folder and a few commands but cannot reach beyond them. The decision is plain data in, verdict out, so the shell of the product
//! can be tested without a model.

mod policy;
mod sandbox;
mod store;
mod suggest;
mod task;

pub use policy::{Action, CloudUse, Context, Mode, Policy, Verdict, WebAccess};
pub use sandbox::bwrap_args;
pub use store::Store;
pub use suggest::suggest;
pub use task::{State, Step, Task};
