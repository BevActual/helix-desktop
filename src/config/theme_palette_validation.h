#pragma once

#include <filesystem>
#include <string_view>

namespace helix::config {

/// Builtin palette ids shipped with Helix (keep aligned with `palette_names::BUILTIN_PALETTE_NAMES` in Rust).
[[nodiscard]] bool isKnownBuiltinPalette(std::string_view name);

/// `<config>/palettes/<name>.json` (C++ `customPalettePath` without linking `custom_palettes.cpp`).
[[nodiscard]] std::filesystem::path customPaletteJsonPath(std::string_view name);

} // namespace helix::config
