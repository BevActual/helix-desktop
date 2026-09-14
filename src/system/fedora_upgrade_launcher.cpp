#include "system/fedora_upgrade_launcher.h"

#include "core/log.h"
#include "core/process/process.h"
#include "system/fedora_upgrade.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <unistd.h>
#include <vector>

namespace noctalia::fedora_upgrade {

  namespace {

    constexpr Logger kLog("fedora-upgrade");
    std::atomic<bool> g_inProgress{false};

    std::string readTextFile(const std::filesystem::path& path) {
      std::ifstream file(path);
      if (!file) {
        return {};
      }
      return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    std::string osReleaseText() {
      for (const char* path : {"/etc/os-release", "/usr/lib/os-release"}) {
        auto text = readTextFile(path);
        if (!text.empty()) {
          return text;
        }
      }
      return {};
    }

    bool isExecutableFile(const std::filesystem::path& path) {
      std::error_code ec;
      return ::access(path.c_str(), X_OK) == 0 && std::filesystem::is_regular_file(path, ec);
    }

    std::filesystem::path helperPath() {
#ifdef NOCTALIA_SYSTEM_UPGRADE_PATH
      const std::filesystem::path configured(NOCTALIA_SYSTEM_UPGRADE_PATH);
      if (isExecutableFile(configured)) {
        return configured;
      }
#endif
      for (const char* candidate : {"/usr/libexec/noctalia-system-upgrade", "/usr/local/libexec/noctalia-system-upgrade"}) {
        if (isExecutableFile(candidate)) {
          return candidate;
        }
      }
      return {};
    }

  } // namespace

  bool available() {
    const auto host = classifyHost(
        osReleaseText(), std::filesystem::exists("/run/ostree-booted")
    );
    return host == HostKind::Fedora && !helperPath().empty() && process::commandExists(kPkexecName.data());
  }

  bool busy() noexcept { return g_inProgress.load(std::memory_order_acquire); }

  Launch start(Completion onComplete) {
    bool expected = false;
    if (!g_inProgress.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
      kLog.info("shell upgrade is already in progress");
      return Launch::Busy;
    }

    const auto helper = helperPath();
    if (helper.empty() || !process::commandExists(kPkexecName.data())) {
      g_inProgress.store(false, std::memory_order_release);
      kLog.warn("shell upgrade helper or pkexec is missing");
      return Launch::Failed;
    }

    const auto host = classifyHost(osReleaseText(), std::filesystem::exists("/run/ostree-booted"));
    if (host != HostKind::Fedora) {
      g_inProgress.store(false, std::memory_order_release);
      kLog.warn("shell upgrade is only available on Fedora");
      return Launch::Failed;
    }

    auto completion = std::make_shared<Completion>(std::move(onComplete));
    const bool launched = process::runAsync(
        std::vector<std::string>{std::string(kPkexecName), helper.string()},
        process::RunCallbacks{
            .onExit =
                [completion](process::RunResult result) {
                  g_inProgress.store(false, std::memory_order_release);
                  if (completion && *completion) {
                    std::string error = result.err.empty() ? result.out : result.err;
                    if (result.timedOut) {
                      error = "upgrade timed out";
                    }
                    (*completion)(static_cast<bool>(result), std::move(error));
                  }
                },
        }
    );
    if (!launched) {
      g_inProgress.store(false, std::memory_order_release);
      kLog.warn("failed to launch pkexec for shell upgrade");
      return Launch::Failed;
    }
    return Launch::Launched;
  }

} // namespace noctalia::fedora_upgrade
