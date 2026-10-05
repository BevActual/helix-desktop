#pragma once

#include "config/config_types.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wallpaper::video {

  inline constexpr std::string_view kBinary = "mpvpaper";
  inline constexpr std::string_view kWildcardConnector = "*";

  struct LaunchSpec {
    std::string connector;
    std::string path;
    bool mute = true;
    bool hardwareDecode = true;
    WallpaperVideoAutoPause autoPause = WallpaperVideoAutoPause::Full;
    std::string extraMpvOptions;
    bool autoModeSupported = false;
  };

  [[nodiscard]] bool binaryAvailable();
  [[nodiscard]] bool helpSupportsAutoMode(std::string_view helpText);
  [[nodiscard]] std::filesystem::path resolveDirectory(std::string_view configured);
  [[nodiscard]] std::string mpvOptionString(const LaunchSpec& spec);
  [[nodiscard]] std::vector<std::string> autoPauseArgs(const LaunchSpec& spec);
  [[nodiscard]] std::vector<std::string> buildCommand(const LaunchSpec& spec);
  [[nodiscard]] std::unordered_map<std::string, std::string> parseAssignmentsJson(std::string_view json);
  [[nodiscard]] std::string serializeAssignmentsJson(const std::unordered_map<std::string, std::string>& assignments);

} // namespace wallpaper::video
