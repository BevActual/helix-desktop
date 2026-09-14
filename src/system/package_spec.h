#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noctalia::packages {

  enum class PackageState : std::uint8_t {
    Installed,
    Available,
    Update,
    Other,
  };

  struct PackageHit {
    std::string name;
    std::string version;
    std::string release;
    std::string arch;
    std::string epoch;
    std::string repo;
    std::string info;
    PackageState state = PackageState::Other;
  };

  struct PkconProgress {
    std::string status;
    int percent = -1;
  };

  [[nodiscard]] bool isValidRpmName(std::string_view name);
  [[nodiscard]] bool hasSafeSearchChars(std::string_view query);
  [[nodiscard]] bool isSafeSearchQuery(std::string_view query);
  [[nodiscard]] bool isProtectedPackage(std::string_view name);
  [[nodiscard]] PackageState stateFromInfo(std::string_view info);
  [[nodiscard]] std::optional<PackageHit> parsePkconLine(std::string_view line);
  [[nodiscard]] std::vector<PackageHit> parsePkconOutput(std::string_view text);
  [[nodiscard]] std::vector<PackageHit> dedupeByName(std::vector<PackageHit> hits);
  void sortHits(std::vector<PackageHit>& hits);
  bool applyPkconProgressLine(std::string_view line, PkconProgress& progress);

} // namespace noctalia::packages
