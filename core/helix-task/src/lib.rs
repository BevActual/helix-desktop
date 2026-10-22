//! Agent tasks. A task is a goal, the policy it runs under and a record of what it did; it outlives the chat window it was started from.
//!
//! The policy is the answer to "what may this task do without asking me?" Everything outside it asks (or is refused), so an unattended task can
//! work inside a folder and a few commands but cannot reach beyond them. The decision is plain data in, verdict out, so the shell of the product
//! can be tested without a model.

mod policy;
mod store;
mod task;

pub use policy::{Action, CloudUse, Context, Policy, Verdict, WebAccess};
pub use store::Store;
pub use task::{State, Step, Task};
