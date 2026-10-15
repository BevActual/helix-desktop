//! Custom HSL-space schemes (C++ `custom_schemes.cpp`): vibrant, faithful, soft, dysfunctional, muted.

use crate::custom::GeneratedPalette;
use crate::wallpaper::Scheme;

pub fn generate_custom(_rgb112: &[u8], _scheme: Scheme) -> GeneratedPalette {
    GeneratedPalette::default()
}
