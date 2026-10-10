#pragma once

#include "compositors/niri/niri_config_binds.h"
#include "system/display_config.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace compositors::niri {

  inline constexpr std::string_view kOutputsIncludeFileName = "helix-outputs.kdl";

  struct ParsedOutput {
    std::string name;
    bool enabled = true;
    bool vrrEnabled = false;
    int x = 0;
    int y = 0;
    double scale = 1.0;
    display_config::Transform transform = display_config::Transform::Normal;
    display_config::Mode mode;
  };

  [[nodiscard]] std::filesystem::path outputsIncludePath();
  [[nodiscard]] std::string kdlForOutputs(std::span<const display_config::Output> outputs);
  [[nodiscard]] std::vector<ParsedOutput> parseOutputs(std::string_view kdl);
  [[nodiscard]] std::string layoutFingerprint(std::span<const display_config::Output> outputs);
  [[nodiscard]] std::string layoutFingerprint(std::span<const ParsedOutput> outputs);
  [[nodiscard]] bool savedLayoutMatches(
      std::span<const display_config::Output> outputs, const std::filesystem::path& configFile = {}
  );

  InstallResult saveOutputs(std::span<const display_config::Output> outputs);
  InstallResult saveOutputsInto(
      const std::filesystem::path& configFile, std::span<const display_config::Output> outputs
  );

} // namespace compositors::niri
