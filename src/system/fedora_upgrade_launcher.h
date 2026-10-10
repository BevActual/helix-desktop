#pragma once

#include <functional>
#include <string>

namespace helix::fedora_upgrade {

  using Completion = std::function<void(bool success, std::string error)>;

  enum class Launch {
    Failed,
    Busy,
    Launched,
  };

  [[nodiscard]] bool available();
  [[nodiscard]] bool busy() noexcept;

  // Runs pkexec on the installed helper. onComplete runs on the worker thread.
  [[nodiscard]] Launch start(Completion onComplete);

} // namespace helix::fedora_upgrade
