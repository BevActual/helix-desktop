#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace compositors::niri {

  inline constexpr std::string_view kBindsIncludeFileName = "noctalia-binds.kdl";

  struct RecommendedBind {
    std::string_view id;
    std::string_view ipcCommand;
    std::string_view key;
  };

  struct ParsedBind {
    std::string key;
    std::string ipcCommand;
    std::string action;
  };

  struct BindStatus {
    RecommendedBind recommended;
    std::vector<std::string> boundKeys;
    bool keyConflict = false;
  };

  struct ConfigScan {
    std::filesystem::path configPath;
    std::filesystem::path bindsIncludePath;
    std::vector<ParsedBind> binds;
    bool configExists = false;
    bool hasBindsInclude = false;
  };

  struct InstallResult {
    bool ok = false;
    int added = 0;
    int skippedConflicts = 0;
    std::string error;
  };

  [[nodiscard]] std::filesystem::path configPath();
  [[nodiscard]] std::filesystem::path bindsIncludePath();
  [[nodiscard]] std::span<const RecommendedBind> recommendedBinds();

  [[nodiscard]] std::string kdlLine(const RecommendedBind& bind);
  [[nodiscard]] std::string kdlBlock(const std::vector<RecommendedBind>& binds);
  [[nodiscard]] std::optional<std::string> ipcCommandFromAction(std::string_view action);
  [[nodiscard]] std::vector<ParsedBind> parseBinds(std::string_view kdl);
  [[nodiscard]] std::vector<std::filesystem::path>
  parseIncludes(std::string_view kdl, const std::filesystem::path& fromFile);

  [[nodiscard]] ConfigScan scanConfig();
  [[nodiscard]] ConfigScan scanConfigFile(const std::filesystem::path& path);
  [[nodiscard]] std::vector<BindStatus> bindStatuses(const ConfigScan& scan);
  [[nodiscard]] std::vector<RecommendedBind> missingRecommended(const ConfigScan& scan);

  InstallResult installMissing();
  InstallResult installMissingInto(const std::filesystem::path& configFile);

} // namespace compositors::niri
