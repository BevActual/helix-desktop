#pragma once

#include "compositors/niri/niri_config_binds.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace compositors::niri {

  inline constexpr std::string_view kWindowRulesIncludeFileName = "noctalia-window-rules.kdl";

  [[nodiscard]] std::filesystem::path windowRulesIncludePath();
  [[nodiscard]] std::string kdlForWindowCornerRadius(int radius);
  // Reads back the value from Noctalia's own managed include file; nullopt if it hasn't been written yet.
  [[nodiscard]] std::optional<int> currentWindowCornerRadius();
  [[nodiscard]] std::optional<int> currentWindowCornerRadiusFrom(const std::filesystem::path& configFile);

  InstallResult saveWindowCornerRadius(int radius);
  InstallResult saveWindowCornerRadiusInto(const std::filesystem::path& configFile, int radius);

} // namespace compositors::niri
