#pragma once

#include "theme/color_roles.h"
#include "theme/community_palettes.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace helix::theme {

  struct GeneratedPalette;

  [[nodiscard]] std::filesystem::path customPaletteDir();
  [[nodiscard]] std::filesystem::path customPalettePath(std::string_view name);
  [[nodiscard]] std::vector<AvailablePalette> availableCustomPalettes();

  [[nodiscard]] std::string suggestCustomPaletteName(std::string_view wallpaperPath, std::string_view scheme);
  [[nodiscard]] std::string allocateCustomPaletteName(std::string_view base);
  [[nodiscard]] std::string allocateCustomPaletteName(std::string_view wallpaperPath, std::string_view scheme);
  [[nodiscard]] bool saveCustomPaletteFromGenerated(
      std::string_view name, const GeneratedPalette& palette, std::string* errorOut = nullptr
  );
  [[nodiscard]] bool applyEditablePaletteRole(::Palette& palette, ColorRole role, const ::Color& color);

} // namespace helix::theme
