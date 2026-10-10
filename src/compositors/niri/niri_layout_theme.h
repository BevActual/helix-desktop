#pragma once

#include "compositors/niri/niri_config_binds.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace compositors::niri {

  inline constexpr std::string_view kLayoutThemeIncludeFileName = "helix-layout.kdl";

  struct LayoutThemeSettings {
    bool focusRing = false;
    bool border = false;
    // "#rrggbb"; active = focused, inactive = other, urgent = attention.
    std::string activeColorHex;
    std::string inactiveColorHex;
    std::string urgentColorHex;
    std::string tabInactiveColorHex;
    std::string insertHintColorHex;
    // When set, writes `gaps` into the owned layout include. Unset leaves the user's gaps alone.
    std::optional<int> gaps;
    // When set, writes equal `struts` on all four sides. Unset leaves the user's struts alone.
    std::optional<int> struts;
    // When set, writes `default-column-width { proportion ... }`. Unset leaves niri's default.
    std::optional<double> defaultColumnWidth;
  };

  [[nodiscard]] std::filesystem::path layoutThemeIncludePath();
  [[nodiscard]] std::string kdlForLayoutTheme(const LayoutThemeSettings& settings);

  InstallResult saveNiriLayoutTheme(const LayoutThemeSettings& settings);
  InstallResult saveNiriLayoutThemeInto(const std::filesystem::path& configFile, const LayoutThemeSettings& settings);

} // namespace compositors::niri
