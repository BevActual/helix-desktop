#pragma once

#include "compositors/niri/niri_config_files.h"
#include "core/input/key_chord.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace compositors::niri {

  inline constexpr std::string_view kBindsIncludeFileName = "helix-binds.kdl";

  enum class BindKind : std::uint8_t { Panels, Capture, Media, System };

  struct RecommendedBind {
    std::string_view id;
    std::string_view ipcCommand;
    std::string_view key;
    bool allowWhenLocked = false;
    BindKind kind = BindKind::Panels;
    std::string_view fallbackKey = {};
  };

  struct ParsedBind {
    std::string key;
    std::string ipcCommand;
    std::string action;
    bool allowWhenLocked = false;
    std::vector<std::string> spawnArgs;
    bool spawnSh = false;
    std::filesystem::path sourcePath;
  };

  struct AppBind {
    std::string key;
    std::vector<std::string> spawnArgs;
    bool spawnSh = false;
  };

  struct BindStatus {
    RecommendedBind recommended;
    std::vector<std::string> boundKeys;
    bool keyConflict = false;
    bool inOwnedInclude = false;
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

  [[nodiscard]] std::filesystem::path bindsIncludePath();
  [[nodiscard]] std::span<const RecommendedBind> recommendedBinds();

  [[nodiscard]] std::string kdlLine(const RecommendedBind& bind);
  [[nodiscard]] std::string kdlBlock(const std::vector<RecommendedBind>& binds);
  [[nodiscard]] std::optional<std::string> ipcCommandFromAction(std::string_view action);
  [[nodiscard]] std::vector<ParsedBind> parseBinds(std::string_view kdl);

  [[nodiscard]] std::string niriKeyFromChord(const KeyChord& chord);
  [[nodiscard]] std::optional<KeyChord> niriKeyToChord(std::string_view key);

  [[nodiscard]] ConfigScan scanConfig();
  [[nodiscard]] ConfigScan scanConfigFile(const std::filesystem::path& path);
  [[nodiscard]] std::vector<BindStatus> bindStatuses(const ConfigScan& scan);
  [[nodiscard]] std::vector<RecommendedBind> missingRecommended(const ConfigScan& scan);

  InstallResult installMissing();
  InstallResult installMissingInto(const std::filesystem::path& configFile);
  InstallResult setRecommendedKey(std::string_view id, std::string_view niriKey);
  InstallResult
  setRecommendedKeyInto(const std::filesystem::path& configFile, std::string_view id, std::string_view niriKey);
  InstallResult clearRecommendedKey(std::string_view id);
  InstallResult clearRecommendedKeyInto(const std::filesystem::path& configFile, std::string_view id);
  InstallResult resetRecommendedKeys();
  InstallResult resetRecommendedKeysInto(const std::filesystem::path& configFile);

  [[nodiscard]] std::vector<AppBind> ownedAppBinds(const ConfigScan& scan);
  InstallResult setAppBind(std::string_view key, std::span<const std::string> spawnArgs, bool spawnSh = false);
  InstallResult setAppBindInto(
      const std::filesystem::path& configFile, std::string_view key, std::span<const std::string> spawnArgs,
      bool spawnSh = false
  );
  InstallResult clearAppBind(std::string_view key);
  InstallResult clearAppBindInto(const std::filesystem::path& configFile, std::string_view key);

} // namespace compositors::niri
