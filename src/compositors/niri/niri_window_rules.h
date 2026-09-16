#pragma once

#include "compositors/niri/niri_config_binds.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace compositors::niri {

  inline constexpr std::string_view kWindowRulesIncludeFileName = "noctalia-window-rules.kdl";

  struct WindowRulesSettings {
    std::optional<int> cornerRadius;
    std::optional<bool> drawBorderWithBackground;
  };

  [[nodiscard]] std::filesystem::path windowRulesIncludePath();
  [[nodiscard]] std::string kdlForWindowRules(const WindowRulesSettings& settings);
  [[nodiscard]] std::string kdlForWindowCornerRadius(int radius);
  // Reads back the value from Noctalia's own managed include file; nullopt if it hasn't been written yet.
  [[nodiscard]] std::optional<int> currentWindowCornerRadius();
  [[nodiscard]] std::optional<int> currentWindowCornerRadiusFrom(const std::filesystem::path& configFile);

  InstallResult saveWindowRules(const WindowRulesSettings& settings);
  InstallResult saveWindowRulesInto(const std::filesystem::path& configFile, const WindowRulesSettings& settings);
  InstallResult saveWindowCornerRadius(int radius);
  InstallResult saveWindowCornerRadiusInto(const std::filesystem::path& configFile, int radius);

} // namespace compositors::niri
