#pragma once

#include "compositors/niri/niri_config_binds.h"
#include "config/config_types.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace compositors::niri {

  inline constexpr std::string_view kOverviewIncludeFileName = "helix-overview.kdl";
  inline constexpr std::string_view kDefaultFlatOverviewColor = "#26233a";

  struct OverviewSettings {
    NiriOverviewPreset preset = NiriOverviewPreset::None;
    std::string flatColorHex;
  };

  [[nodiscard]] std::filesystem::path overviewIncludePath();
  [[nodiscard]] std::string sanitizedFlatColorHex(std::string_view hex);
  [[nodiscard]] bool overviewWantsBackdrop(NiriOverviewPreset preset);
  [[nodiscard]] std::string kdlForOverview(const OverviewSettings& settings);

  InstallResult saveNiriOverview(const OverviewSettings& settings);
  InstallResult saveNiriOverviewInto(const std::filesystem::path& configFile, const OverviewSettings& settings);

} // namespace compositors::niri
