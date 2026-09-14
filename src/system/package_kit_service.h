#pragma once

#include "system/package_spec.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace noctalia::packages {

  enum class Action : std::uint8_t { Install, Remove, Update };

  struct CommandResult {
    bool success = false;
    bool cancelled = false;
    std::string error;
    std::vector<PackageHit> hits;
  };

  using CommandCompletion = std::function<void(CommandResult result)>;
  using ProgressCallback = std::function<void(PkconProgress progress)>;

  [[nodiscard]] bool backendAvailable();
  [[nodiscard]] bool tabAvailable();
  [[nodiscard]] bool busy() noexcept;
  void cancelCurrent();

  // Worker-thread completion. Query/name must already be validated.
  [[nodiscard]] bool search(std::string query, CommandCompletion onComplete);
  [[nodiscard]] bool listUpdates(CommandCompletion onComplete);
  [[nodiscard]] bool listInstalled(CommandCompletion onComplete);
  [[nodiscard]] bool updateAll(CommandCompletion onComplete, ProgressCallback onProgress = {});
  [[nodiscard]] bool updatePackages(
      std::vector<std::string> names, CommandCompletion onComplete, ProgressCallback onProgress = {}
  );
  [[nodiscard]] bool mutate(
      Action action, std::string name, CommandCompletion onComplete, ProgressCallback onProgress = {}
  );

} // namespace noctalia::packages
