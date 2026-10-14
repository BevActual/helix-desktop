//! Palette math and custom-palette storage, ported from the C++ `src/theme/` tree.
//!
//! Parity with the C++ implementation is the contract: `tests/` diffs this crate's
//! output against `helix-theme-palette` for a fixed set of inputs.

pub mod apply;
pub mod color;
pub mod contrast;
pub mod custom;
pub mod hct;
pub mod palette;
pub mod schedule;
pub mod template;
pub mod transform;

pub use color::Color;
