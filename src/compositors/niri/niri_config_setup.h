#pragma once

#include "compositors/niri/niri_config_binds.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace compositors::niri {

  inline constexpr std::string_view kSetupIncludeFileName = "noctalia-setup.kdl";

  struct SetupItem {
    std::string_view id;
    std::string_view kdl;
    bool optional = false;
  };

  struct SetupStatus {
    SetupItem item;
    bool present = false;
  };

  struct SetupScan {
    std::filesystem::path configPath;
    std::filesystem::path setupIncludePath;
    bool configExists = false;
    bool hasSetupInclude = false;
    std::vector<SetupStatus> items;
  };

  [[nodiscard]] std::filesystem::path setupIncludePath();
  [[nodiscard]] std::span<const SetupItem> setupCatalog(bool includeBackdrop);
  [[nodiscard]] SetupScan scanSetup(bool includeBackdrop = true);
  [[nodiscard]] SetupScan scanSetupFile(const std::filesystem::path& path, bool includeBackdrop = true);
  [[nodiscard]] std::vector<SetupItem> missingSetup(const SetupScan& scan);
  [[nodiscard]] std::string kdlBlock(std::span<const SetupItem> items);

  InstallResult installSetup(bool includeBackdrop = true);
  InstallResult installSetupInto(const std::filesystem::path& configFile, bool includeBackdrop = true);

} // namespace compositors::niri
