//! Shared test fixtures (`helix-desktop` unit tests only).

use std::sync::Mutex;

/// Serializes tests that mutate process environment variables (`XDG_*`, `HELIX_*`).
pub static ENV_LOCK: Mutex<()> = Mutex::new(());
