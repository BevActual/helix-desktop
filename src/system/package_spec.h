#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace helix::packages {

  enum class PackageState : std::uint8_t {
    Installed,
    Available,
    Update,
    Other,
  };

  enum class Kind : std::uint8_t { Fedora, Flatpak, Firmware };

  enum class FlatpakListKind : std::uint8_t { Installed, Updates, Catalog };

  enum class FwupdListKind : std::uint8_t { Installed, Updates };

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
    std::string iconPath;
    std::string iconName;
    std::vector<std::string> categories;
    bool verified = false;
    PackageState state = PackageState::Other;
  };

  struct FlatpakCatalogIndex {
    std::unordered_map<std::string, std::vector<std::string>> categories;
    std::unordered_set<std::string> verified;
  };

  struct FlatpakRemote {
    std::string name;
    std::string title;
    std::string url;
    std::string installation; // "user" or "system"
    int priority = 1;
    bool disabled = false;
    bool filtered = false;
    bool oci = false;
  };

  enum class FlatpakRemoteAction : std::uint8_t { Enable, Disable, Delete, AddFlathub };

  struct PackageDetails {
    PackageHit hit;
    std::string description;
    std::string developer;
    std::string license;
    std::string homepage;
    std::string bugtracker;
    std::string donation;
    std::string helpUrl;
    std::string contentRating;
    std::string releaseNotes;
    std::string verification;
    std::string downloadSize;
    std::string installedSize;
    std::string runtime;
    std::string permissions;
    std::vector<std::string> categories;
    std::vector<std::string> screenshotUrls;
  };

  struct PkconProgress {
    std::string status;
    int percent = -1;
  };

  [[nodiscard]] bool isValidRpmName(std::string_view name);
  [[nodiscard]] bool isValidPkconPackageId(std::string_view id);
  [[nodiscard]] bool isValidPkconPackageRef(std::string_view ref);
  [[nodiscard]] std::string pkconPackageId(const PackageHit& hit);
  [[nodiscard]] std::string pkconPackageRef(const PackageHit& hit);
  // Remove needs an installed-scoped PackageKit data field (installed:…); install/update keep the
  // repo from search/get-updates.
  [[nodiscard]] std::string pkconPackageRefForAction(const PackageHit& hit, bool removing);
  [[nodiscard]] bool isValidFlatpakAppId(std::string_view id);
  [[nodiscard]] bool isValidFlatpakRemoteName(std::string_view name);
  [[nodiscard]] bool isValidFwupdDeviceId(std::string_view id);
  [[nodiscard]] std::optional<FlatpakRemote> parseFlatpakRemoteLine(std::string_view line);
  [[nodiscard]] std::vector<FlatpakRemote> parseFlatpakRemotesOutput(std::string_view text);
  [[nodiscard]] bool hasUserFlathubRemote(const std::vector<FlatpakRemote>& remotes);
  [[nodiscard]] bool hasSafeSearchChars(std::string_view query);
  [[nodiscard]] bool isSafeSearchQuery(std::string_view query);
  [[nodiscard]] bool isProtectedPackage(std::string_view name);
  [[nodiscard]] bool isRpmDevelPackage(std::string_view name);
  [[nodiscard]] bool isRpmLibraryPackage(std::string_view name);
  [[nodiscard]] bool isRpmAppPackage(std::string_view name);
  [[nodiscard]] bool matchesRpmScope(std::string_view name, std::string_view scope);
  void rankRpmSearchHits(std::string_view query, std::vector<PackageHit>& hits);
  [[nodiscard]] std::string preferredFlatpakRemote(std::string_view remotes);
  [[nodiscard]] std::optional<PackageHit> parseFlatpakListLine(std::string_view line, FlatpakListKind kind);
  [[nodiscard]] std::vector<PackageHit> parseFlatpakListOutput(std::string_view text, FlatpakListKind kind);
  [[nodiscard]] std::vector<PackageHit> parseFlatpakSearchJson(std::string_view text);
  [[nodiscard]] std::vector<PackageHit> parseFwupdDevicesJson(std::string_view text, FwupdListKind kind);
  [[nodiscard]] PackageState stateFromInfo(std::string_view info);
  [[nodiscard]] std::optional<PackageHit> parsePkconLine(std::string_view line);
  [[nodiscard]] std::vector<PackageHit> parsePkconOutput(std::string_view text);
  [[nodiscard]] std::optional<PackageHit> parseRpmInstalledLine(std::string_view line);
  [[nodiscard]] std::vector<PackageHit> parseRpmInstalledOutput(std::string_view text);
  [[nodiscard]] std::string compactSummary(std::string_view text);
  [[nodiscard]] std::string plainDescription(std::string_view text);
  [[nodiscard]] std::optional<std::pair<std::string, std::string>>
  parseNameSummaryLine(std::string_view line, char separator);
  [[nodiscard]] std::unordered_map<std::string, std::string>
  parseNameSummaryOutput(std::string_view text, char separator);
  void applySummaryMap(std::vector<PackageHit>& hits, const std::unordered_map<std::string, std::string>& summaries);
  [[nodiscard]] std::vector<PackageHit> dedupeByName(std::vector<PackageHit> hits);
  void sortHits(std::vector<PackageHit>& hits);
  bool applyPkconProgressLine(std::string_view line, PkconProgress& progress);
  bool applyFlatpakProgressLine(std::string_view line, PkconProgress& progress);

  [[nodiscard]] std::vector<std::string> flatpakAppstreamRoots();
  [[nodiscard]] std::string resolveFlatpakIconPath(std::string_view appId, std::string_view remote = {});
  void attachFlatpakIcons(std::vector<PackageHit>& hits);
  [[nodiscard]] std::optional<std::string> extractAppstreamComponentXml(std::string_view xml, std::string_view appId);
  [[nodiscard]] PackageDetails parseAppstreamComponentXml(std::string_view componentXml, PackageHit base = {});
  [[nodiscard]] PackageDetails loadFlatpakAppstreamDetails(PackageHit base);
  [[nodiscard]] PackageDetails parseFlatpakRemoteInfo(std::string_view text, PackageHit base = {});
  [[nodiscard]] PackageDetails parseFlatpakPermissions(std::string_view text, PackageDetails details = {});
  [[nodiscard]] std::vector<std::string> humanizeFlatpakPermissions(std::string_view metadata);
  [[nodiscard]] FlatpakCatalogIndex loadFlatpakCatalogIndex();
  [[nodiscard]] std::unordered_map<std::string, std::vector<std::string>> loadFlatpakCategoryIndex();
  void attachFlatpakCategories(
      std::vector<PackageHit>& hits, const std::unordered_map<std::string, std::vector<std::string>>& index
  );
  void attachFlatpakCatalogIndex(std::vector<PackageHit>& hits, const FlatpakCatalogIndex& index);
  [[nodiscard]] std::string categoryDisplayName(std::string_view category);
  [[nodiscard]] PackageDetails parseRpmInfoOutput(std::string_view text, PackageHit base = {});
  [[nodiscard]] PackageDetails enrichFromFwupdDeviceJson(std::string_view text, PackageHit base);

} // namespace helix::packages
