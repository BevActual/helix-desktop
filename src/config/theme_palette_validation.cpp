#include "config/theme_palette_validation.h"

#include "util/file_utils.h"

#include <algorithm>
#include <array>
#include <string>

namespace helix::config {

namespace {

  constexpr std::array<const char*, 10> kBuiltinPaletteNames{
      "Ayu",
      "Catppuccin",
      "Dracula",
      "Eldritch",
      "Gruvbox",
      "Kanagawa",
      "Helix",
      "Nord",
      "Rosé Pine",
      "Tokyo-Night",
  };

} // namespace

bool isKnownBuiltinPalette(std::string_view name) {
  return std::ranges::find(kBuiltinPaletteNames, name) != kBuiltinPaletteNames.end();
}

std::filesystem::path customPaletteJsonPath(std::string_view name) {
  const std::string dir = FileUtils::configDir();
  if (dir.empty()) {
    return {};
  }
  return std::filesystem::path(dir) / "palettes" / (std::string(name) + ".json");
}

} // namespace helix::config
