#pragma once

#include "compositors/niri/niri_config_binds.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace compositors::niri {

  inline constexpr std::string_view kBlurIncludeFileName = "noctalia-blur.kdl";

  inline constexpr int kDefaultBlurPasses = 2;
  inline constexpr float kDefaultBlurOffset = 3.0F;
  inline constexpr float kDefaultBlurNoise = 0.03F;

  struct BlurSettings {
    bool enabled = false;
    int passes = kDefaultBlurPasses;
    float offset = kDefaultBlurOffset;
    float noise = kDefaultBlurNoise;
  };

  [[nodiscard]] std::filesystem::path blurIncludePath();
  [[nodiscard]] bool versionSupportsBlur(std::string_view version);
  [[nodiscard]] bool blurIncludeExists(const std::filesystem::path& configFile = {});
  [[nodiscard]] std::string kdlForBlur(const BlurSettings& settings);

  InstallResult saveNiriBlur(const BlurSettings& settings);
  InstallResult saveNiriBlurInto(const std::filesystem::path& configFile, const BlurSettings& settings);

} // namespace compositors::niri
