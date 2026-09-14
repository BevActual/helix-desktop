#include "system/package_kit_service.h"

#include "core/log.h"
#include "core/process/process.h"
#include "system/fedora_upgrade.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <utility>

namespace noctalia::packages {

  namespace {

    constexpr Logger kLog("packages");
    constexpr std::string_view kPkcon = "pkcon";
    constexpr int kPkconNothingUseful = 5;
    std::atomic<bool> g_busy{false};
    std::shared_ptr<std::atomic<bool>> g_cancelFlag;

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

    std::string compactError(std::string text) {
      std::string last;
      std::size_t start = 0;
      while (start <= text.size()) {
        const auto end = text.find('\n', start);
        auto line = std::string_view(text).substr(
            start, end == std::string::npos ? text.size() - start : end - start
        );
        while (!line.empty() && (line.back() == '\r' || std::isspace(static_cast<unsigned char>(line.back())) != 0)) {
          line.remove_suffix(1);
        }
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())) != 0) {
          line.remove_prefix(1);
        }
        if (!line.empty() && !line.starts_with("Transaction:") && !line.starts_with("Status:")
            && !line.starts_with("Percentage:") && line != "Results:") {
          last = std::string(line);
        }
        if (end == std::string::npos) {
          break;
        }
        start = end + 1;
      }
      if (last.empty()) {
        last = std::move(text);
      }
      while (!last.empty() && (last.back() == '\n' || last.back() == '\r')) {
        last.pop_back();
      }
      constexpr std::size_t kMax = 280;
      if (last.size() <= kMax) {
        return last;
      }
      return last.substr(last.size() - kMax);
    }

    void feedProgress(std::string& pending, std::string_view chunk, PkconProgress& progress, const ProgressCallback& onProgress) {
      pending.append(chunk);
      std::size_t start = 0;
      while (start < pending.size()) {
        const auto end = pending.find('\n', start);
        if (end == std::string::npos) {
          break;
        }
        const bool changed = applyPkconProgressLine(std::string_view(pending).substr(start, end - start), progress);
        start = end + 1;
        if (changed && onProgress) {
          onProgress(progress);
        }
      }
      if (start > 0) {
        pending.erase(0, start);
      }
    }

    bool launchPkcon(
        std::vector<std::string> args, process::RunOptions options, CommandCompletion onComplete,
        ProgressCallback onProgress
    ) {
      bool expected = false;
      if (!g_busy.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
        return false;
      }
      auto completion = std::make_shared<CommandCompletion>(std::move(onComplete));
      auto progressCb = std::make_shared<ProgressCallback>(std::move(onProgress));
      auto pendingOut = std::make_shared<std::string>();
      auto pendingErr = std::make_shared<std::string>();
      auto progress = std::make_shared<PkconProgress>();
      auto cancel = std::make_shared<std::atomic<bool>>(false);
      g_cancelFlag = cancel;
      options.cancel = cancel;
      const bool launched = process::runAsync(
          std::move(args),
          process::RunCallbacks{
              .stdOut =
                  [pendingOut, progress, progressCb](std::string_view chunk) {
                    feedProgress(*pendingOut, chunk, *progress, *progressCb);
                  },
              .stdErr =
                  [pendingErr, progress, progressCb](std::string_view chunk) {
                    feedProgress(*pendingErr, chunk, *progress, *progressCb);
                  },
              .onExit =
                  [completion, cancel](process::RunResult result) {
                    g_busy.store(false, std::memory_order_release);
                    const bool cancelled = cancel->load(std::memory_order_acquire);
                    CommandResult out;
                    out.cancelled = cancelled;
                    out.success = !cancelled && !result.timedOut
                        && (result.exitCode == 0 || result.exitCode == kPkconNothingUseful);
                    out.hits = parsePkconOutput(result.out);
                    if (!out.success && !cancelled) {
                      out.error = compactError(result.err.empty() ? result.out : result.err);
                      if (result.timedOut) {
                        out.error = "timed out";
                      }
                    }
                    if (completion && *completion) {
                      (*completion)(std::move(out));
                    }
                  },
          },
          std::move(options)
      );
      if (!launched) {
        g_busy.store(false, std::memory_order_release);
        g_cancelFlag.reset();
        kLog.warn("failed to launch pkcon");
      }
      return launched;
    }

  } // namespace

  bool backendAvailable() { return process::commandExists(kPkcon.data()); }

  bool tabAvailable() {
    if (!backendAvailable()) {
      return false;
    }
    const auto host = fedora_upgrade::classifyHost(
        osReleaseText(), std::filesystem::exists("/run/ostree-booted")
    );
    return host == fedora_upgrade::HostKind::Fedora;
  }

  bool busy() noexcept { return g_busy.load(std::memory_order_acquire); }

  void cancelCurrent() {
    if (g_cancelFlag) {
      g_cancelFlag->store(true, std::memory_order_release);
    }
  }

  bool search(std::string query, CommandCompletion onComplete) {
    if (!isSafeSearchQuery(query) || !backendAvailable()) {
      return false;
    }
    process::RunOptions options;
    options.timeout = std::chrono::seconds(45);
    options.maxOutputBytes = 1U << 20;
    return launchPkcon(
        {std::string(kPkcon), "--plain", "--noninteractive", "search", "name", std::move(query)}, std::move(options),
        std::move(onComplete), {}
    );
  }

  bool listUpdates(CommandCompletion onComplete) {
    if (!backendAvailable()) {
      return false;
    }
    process::RunOptions options;
    options.timeout = std::chrono::seconds(60);
    options.maxOutputBytes = 1U << 20;
    return launchPkcon(
        {std::string(kPkcon), "--plain", "--noninteractive", "get-updates"}, std::move(options), std::move(onComplete),
        {}
    );
  }

  bool listInstalled(CommandCompletion onComplete) {
    if (!backendAvailable()) {
      return false;
    }
    process::RunOptions options;
    options.timeout = std::chrono::seconds(120);
    options.maxOutputBytes = 8U << 20;
    return launchPkcon(
        {std::string(kPkcon), "--plain", "--noninteractive", "get-packages", "--filter", "installed"},
        std::move(options), std::move(onComplete), {}
    );
  }

  bool updateAll(CommandCompletion onComplete, ProgressCallback onProgress) {
    return updatePackages({}, std::move(onComplete), std::move(onProgress));
  }

  bool updatePackages(
      std::vector<std::string> names, CommandCompletion onComplete, ProgressCallback onProgress
  ) {
    if (!backendAvailable()) {
      return false;
    }
    std::vector<std::string> args = {
        std::string(kPkcon), "--plain", "--noninteractive", "-y", "update"
    };
    for (auto& name : names) {
      if (!isValidRpmName(name)) {
        return false;
      }
      args.push_back(std::move(name));
    }
    process::RunOptions options;
    options.timeout = std::chrono::minutes(15);
    options.maxOutputBytes = 1U << 20;
    return launchPkcon(std::move(args), std::move(options), std::move(onComplete), std::move(onProgress));
  }

  bool mutate(Action action, std::string name, CommandCompletion onComplete, ProgressCallback onProgress) {
    if (!isValidRpmName(name) || !backendAvailable()) {
      return false;
    }
    if (action == Action::Remove && isProtectedPackage(name)) {
      return false;
    }
    const char* verb = "install";
    if (action == Action::Remove) {
      verb = "remove";
    } else if (action == Action::Update) {
      verb = "update";
    }
    process::RunOptions options;
    options.timeout = std::chrono::minutes(15);
    options.maxOutputBytes = 1U << 20;
    return launchPkcon(
        {std::string(kPkcon), "--plain", "--noninteractive", "-y", verb, std::move(name)}, std::move(options),
        std::move(onComplete), std::move(onProgress)
    );
  }

} // namespace noctalia::packages
