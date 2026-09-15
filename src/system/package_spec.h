#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace noctalia::packages {

  enum class PackageState : std::uint8_t {
    Installed,
    Available,
    Update,
    Other,
  };

  enum class Kind : std::uint8_t { Fedora, Flatpak };

  enum class FlatpakListKind : std::uint8_t { Installed, Updates };

  struct PackageHit {
    std::string name;
    std::string title;
    std::string version;
    std::string release;
    std::string arch;
    std::string epoch;
    std::string repo;
    std::string installation;
    std::string info;
    std::string summary;
    PackageState state = PackageState::Other;
  };

  struct PkconProgress {
    std::string status;
    int percent = -1;
  };

  [[nodiscard]] bool isValidRpmName(std::string_view name);
  [[nodiscard]] bool isValidFlatpakAppId(std::string_view id);
  [[nodiscard]] bool hasSafeSearchChars(std::string_view query);
  [[nodiscard]] bool isSafeSearchQuery(std::string_view query);
  [[nodiscard]] bool isProtectedPackage(std::string_view name);
  [[nodiscard]] std::string preferredFlatpakRemote(std::string_view remotes);
  [[nodiscard]] std::optional<PackageHit> parseFlatpakListLine(std::string_view line, FlatpakListKind kind);
  [[nodiscard]] std::vector<PackageHit> parseFlatpakListOutput(std::string_view text, FlatpakListKind kind);
  [[nodiscard]] std::vector<PackageHit> parseFlatpakSearchJson(std::string_view text);
  [[nodiscard]] PackageState stateFromInfo(std::string_view info);
  [[nodiscard]] std::optional<PackageHit> parsePkconLine(std::string_view line);
  [[nodiscard]] std::vector<PackageHit> parsePkconOutput(std::string_view text);
  [[nodiscard]] std::optional<PackageHit> parseRpmInstalledLine(std::string_view line);
  [[nodiscard]] std::vector<PackageHit> parseRpmInstalledOutput(std::string_view text);
  [[nodiscard]] std::string compactSummary(std::string_view text);
  [[nodiscard]] std::optional<std::pair<std::string, std::string>> parseNameSummaryLine(
      std::string_view line, char separator
  );
  [[nodiscard]] std::unordered_map<std::string, std::string> parseNameSummaryOutput(
      std::string_view text, char separator
  );
  void applySummaryMap(std::vector<PackageHit>& hits, const std::unordered_map<std::string, std::string>& summaries);
  [[nodiscard]] std::vector<PackageHit> dedupeByName(std::vector<PackageHit> hits);
  void sortHits(std::vector<PackageHit>& hits);
  bool applyPkconProgressLine(std::string_view line, PkconProgress& progress);

} // namespace noctalia::packages
