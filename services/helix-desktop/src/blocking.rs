//! Runs blocking work (blocking zbus calls, hook processes) from async handlers.
//!
//! The blocking zbus API drives its own executor and panics when entered from a tokio worker
//! ("Cannot start a runtime from within a runtime"). `block_in_place` steps off the runtime
//! context for the duration of the closure. Outside a multi-thread runtime the closure just runs.

pub fn run<R>(work: impl FnOnce() -> R) -> R {
    match tokio::runtime::Handle::try_current() {
        Ok(handle) if handle.runtime_flavor() == tokio::runtime::RuntimeFlavor::MultiThread => {
            tokio::task::block_in_place(work)
        }
        _ => work(),
    }
}
