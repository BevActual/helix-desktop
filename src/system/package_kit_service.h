#pragma once

#include "system/package_spec.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace helix::packages {

  enum class Action : std::uint8_t { Install, Remove, Update };

  struct CommandResult {
    bool success = false;
    bool cancelled = false;
    std::string error;
    std::vector<PackageHit> hits;
    std::vector<FlatpakRemote> remotes;
    PackageDetails details;
  };

  using CommandCompletion = std::function<void(CommandResult result)>;
  using ProgressCallback = std::function<void(PkconProgress progress)>;

  [[nodiscard]] bool backendAvailable();
  [[nodiscard]] bool flatpakAvailable();
  [[nodiscard]] bool firmwareAvailable();
  [[nodiscard]] bool tabAvailable();
  [[nodiscard]] bool busy() noexcept;
  void cancelCurrent();

  // Worker-thread completion. Query/name must already be validated.
  [[nodiscard]] bool search(std::string query, CommandCompletion onComplete, Kind kind = Kind::Fedora);
  [[nodiscard]] bool listCatalog(CommandCompletion onComplete, Kind kind = Kind::Flatpak);
  [[nodiscard]] bool getDetails(PackageHit hit, CommandCompletion onComplete, Kind kind = Kind::Flatpak);
  [[nodiscard]] bool listUpdates(CommandCompletion onComplete, Kind kind = Kind::Fedora);
  [[nodiscard]] bool listInstalled(CommandCompletion onComplete, Kind kind = Kind::Fedora);
  [[nodiscard]] bool updateAll(CommandCompletion onComplete, ProgressCallback onProgress = {}, Kind kind = Kind::Fedora);
  [[nodiscard]] bool updatePackages(
      std::vector<std::string> packageRefs, CommandCompletion onComplete, ProgressCallback onProgress = {},
      Kind kind = Kind::Fedora
  );
  [[nodiscard]] bool mutate(
      Action action, PackageHit hit, CommandCompletion onComplete, ProgressCallback onProgress = {},
      Kind kind = Kind::Fedora, bool deleteData = false
  );
  [[nodiscard]] bool listFlatpakRemotes(CommandCompletion onComplete);
  [[nodiscard]] bool mutateFlatpakRemote(
      FlatpakRemoteAction action, FlatpakRemote remote, CommandCompletion onComplete,
      ProgressCallback onProgress = {}
  );

  [[nodiscard]] std::size_t pendingUpdateCount() noexcept;
  [[nodiscard]] std::size_t pendingUpdateCountFor(Kind kind) noexcept;
  void notePendingUpdates(Kind kind, std::size_t count) noexcept;
  [[nodiscard]] bool refreshPendingUpdateCount(std::function<void()> onComplete = {});

} // namespace helix::packages
