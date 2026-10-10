#pragma once

#include "compositors/niri/niri_config_binds.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct Config;

namespace compositors::niri {

  class NiriRuntime;

  inline constexpr std::string_view kBlurIncludeFileName = "helix-blur.kdl";

  inline constexpr int kDefaultBlurPasses = 2;
  inline constexpr float kDefaultBlurOffset = 3.0F;
  inline constexpr float kDefaultBlurNoise = 0.03F;

  struct BlurSettings {
    bool enabled = false;
    int passes = kDefaultBlurPasses;
    float offset = kDefaultBlurOffset;
    float noise = kDefaultBlurNoise;
    bool refraction = false;
    bool helixNiri = false;
    bool highContrast = false;
    float refractionStrength = 1.0F;
    // `liquid-glass { }` children the compositor accepts. Empty means the build
    // predates parameter reporting and the default key set is written.
    std::vector<std::string> liquidGlassParams;
  };

  [[nodiscard]] std::filesystem::path blurIncludePath();
  [[nodiscard]] bool versionSupportsBlur(std::string_view version);
  [[nodiscard]] bool versionSupportsLiquidGlass(std::string_view version);
  [[nodiscard]] bool detectLiquidGlass(const NiriRuntime* runtime);
  [[nodiscard]] std::vector<std::string> detectLiquidGlassParams(const NiriRuntime* runtime);
  [[nodiscard]] bool blurIncludeExists(const std::filesystem::path& configFile = {});
  [[nodiscard]] std::string kdlForBlur(const BlurSettings& settings);
  [[nodiscard]] BlurSettings blurSettingsFromConfig(
      const Config& config, bool helixNiri, std::vector<std::string> liquidGlassParams = {}
  );

  InstallResult persistBlurFromConfig(const Config& config, const NiriRuntime* runtime);
  InstallResult saveNiriBlur(const BlurSettings& settings);
  InstallResult saveNiriBlurInto(const std::filesystem::path& configFile, const BlurSettings& settings);

} // namespace compositors::niri
