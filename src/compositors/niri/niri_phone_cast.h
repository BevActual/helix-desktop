#pragma once

#include "compositors/niri/niri_config_binds.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace compositors::niri {

  inline constexpr std::string_view kPhoneCastIncludeFileName = "noctalia-phone-cast.kdl";
  // Window title suffix shared by UxPlay and scrcpy so niri can match the phone silhouette.
  inline constexpr std::string_view kPhoneMirrorWindowTitle = "Helix Phone";
  // app-id of the shell-owned iPhone mirror toplevel (AirPlay picture + HID input).
  inline constexpr const char* kPhoneMirrorAppId = "helix-phone";
  inline constexpr int kPhoneMirrorWidth = 390;
  inline constexpr int kPhoneMirrorHeight = 844;
  inline constexpr int kPhoneMirrorCornerRadius = 48;

  [[nodiscard]] std::filesystem::path phoneCastIncludePath();
  [[nodiscard]] std::string kdlForPhoneCast();

  // Idempotent: writes the include and ensures config.kdl pulls it in.
  InstallResult ensurePhoneCastWindowRules();
  InstallResult ensurePhoneCastWindowRulesInto(const std::filesystem::path& configFile);

} // namespace compositors::niri
