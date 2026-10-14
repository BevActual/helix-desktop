//! Palette math and custom-palette storage, ported from the C++ `src/theme/` tree.
//!
//! Parity with the C++ implementation is the contract: `tests/` diffs this crate's
//! output against `helix-theme-palette` for a fixed set of inputs.

pub mod color;
pub mod contrast;
pub mod custom;
pub mod palette;

pub use color::Color;
