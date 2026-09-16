#pragma once

#include "compositors/niri/niri_config_binds.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace compositors::niri {

  inline constexpr std::string_view kLayoutThemeIncludeFileName = "noctalia-layout.kdl";

  struct LayoutThemeSettings {
    bool focusRing = false;
    bool border = false;
    // "#rrggbb"; active = focused monitor's ring/border, inactive = other monitors'.
    std::string activeColorHex;
    std::string inactiveColorHex;
  };

  [[nodiscard]] std::filesystem::path layoutThemeIncludePath();
  [[nodiscard]] std::string kdlForLayoutTheme(const LayoutThemeSettings& settings);

  InstallResult saveNiriLayoutTheme(const LayoutThemeSettings& settings);
  InstallResult saveNiriLayoutThemeInto(const std::filesystem::path& configFile, const LayoutThemeSettings& settings);

} // namespace compositors::niri
