//! Palette math and custom-palette storage (successor to the removed C++ `src/theme/` tree).
//!
//! Parity with the C++ implementation is the contract: `tests/` diffs this crate's
//! output against `helix-theme-palette` for a fixed set of inputs.

pub mod apply;
pub mod color;
pub mod contrast;
pub mod custom;
pub mod custom_schemes;
pub mod firefox_theme;
pub mod hct;
pub mod hook_runner;
pub mod kde;
pub mod palette;
pub mod schedule;
pub mod template;
pub mod transform;
pub mod wallpaper;

pub use color::Color;
