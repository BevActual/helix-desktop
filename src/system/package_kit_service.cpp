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
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace noctalia::packages {

  namespace {

    constexpr Logger kLog("packages");
    constexpr std::string_view kPkcon = "pkcon";
    constexpr int kPkconNothingUseful = 5;
    constexpr std::string_view kFlatpak = "flatpak";
    constexpr std::string_view kFwupdmgr = "fwupdmgr";
    std::atomic<bool> g_busy{false};
    std::atomic<std::size_t> g_pendingRpmCount{0};
    std::atomic<std::size_t> g_pendingFlatpakCount{0};
    std::atomic<std::size_t> g_pendingFirmwareCount{0};
    std::shared_ptr<std::atomic<bool>> g_cancelFlag;

    enum class ParseMode : std::uint8_t {
      Pkcon,
      RpmInstalled,
      FlatpakInstalled,
      FlatpakUpdates,
      FlatpakSearch,
      FwupdInstalled,
      FwupdUpdates,
      None,
    };

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

    void attachSummaries(std::vector<PackageHit>& hits) {
      if (hits.empty()) {
        return;
      }
      process::RunOptions rpmOptions;
      rpmOptions.timeout = std::chrono::seconds(8);
      rpmOptions.maxOutputBytes = 4U << 20;
      const auto rpm = process::runSync(
          {std::string("rpm"), "-qa", "--qf", "%{NAME}\t%{SUMMARY}\n"}, rpmOptions
      );
      if (rpm && !rpm.out.empty()) {
        applySummaryMap(hits, parseNameSummaryOutput(rpm.out, '\t'));
      }

      std::vector<std::string> missing;
      std::unordered_set<std::string> seen;
      missing.reserve(hits.size());
      for (const auto& hit : hits) {
        if (!hit.summary.empty() || !seen.insert(hit.name).second) {
          continue;
        }
        missing.push_back(hit.name);
        if (missing.size() >= 400) {
          break;
        }
      }
      if (missing.empty() || !process::commandExists("dnf")) {
        return;
      }
      std::vector<std::string> args = {
          std::string("dnf"), "-C", "-q", "repoquery", "--queryformat", "%{name}|%{summary}\n"
      };
      args.insert(args.end(), missing.begin(), missing.end());
      process::RunOptions dnfOptions;
      dnfOptions.timeout = std::chrono::seconds(12);
      dnfOptions.maxOutputBytes = 1U << 20;
      const auto dnf = process::runSync(args, dnfOptions);
      if (dnf && !dnf.out.empty()) {
        applySummaryMap(hits, parseNameSummaryOutput(dnf.out, '|'));
      }
    }

    std::vector<PackageHit> parseHits(ParseMode parse, std::string_view text) {
      switch (parse) {
      case ParseMode::Pkcon:
        return parsePkconOutput(text);
      case ParseMode::RpmInstalled:
        return parseRpmInstalledOutput(text);
      case ParseMode::FlatpakInstalled:
        return parseFlatpakListOutput(text, FlatpakListKind::Installed);
      case ParseMode::FlatpakUpdates:
        return parseFlatpakListOutput(text, FlatpakListKind::Updates);
      case ParseMode::FlatpakSearch:
        return parseFlatpakSearchJson(text);
      case ParseMode::FwupdInstalled:
        return parseFwupdDevicesJson(text, FwupdListKind::Installed);
      case ParseMode::FwupdUpdates:
        return parseFwupdDevicesJson(text, FwupdListKind::Updates);
      case ParseMode::None:
        return {};
      }
      return {};
    }

    bool launchTracked(
        std::vector<std::string> args, process::RunOptions options, CommandCompletion onComplete,
        ProgressCallback onProgress, ParseMode parse, bool enrichSummaries, const char* commandLabel
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
                  [completion, cancel, enrichSummaries, parse](process::RunResult result) {
                    const bool cancelled = cancel->load(std::memory_order_acquire);
                    CommandResult out;
                    out.cancelled = cancelled;
                    const bool pkcon = parse == ParseMode::Pkcon;
                    out.success = !cancelled && !result.timedOut
                        && (result.exitCode == 0 || (pkcon && result.exitCode == kPkconNothingUseful));
                    out.hits = parseHits(parse, result.out);
                    if (enrichSummaries && out.success && !cancelled && !out.hits.empty()) {
                      attachSummaries(out.hits);
                    }
                    g_busy.store(false, std::memory_order_release);
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
        kLog.warn("failed to launch {}", commandLabel);
      }
      return launched;
    }

    bool launchPkcon(
        std::vector<std::string> args, process::RunOptions options, CommandCompletion onComplete,
        ProgressCallback onProgress, bool enrichSummaries = false
    ) {
      return launchTracked(
          std::move(args), std::move(options), std::move(onComplete), std::move(onProgress), ParseMode::Pkcon,
          enrichSummaries, "pkcon"
      );
    }

    bool launchRpmInstalled(CommandCompletion onComplete) {
      process::RunOptions options;
      options.timeout = std::chrono::seconds(15);
      options.maxOutputBytes = 8U << 20;
      return launchTracked(
          {std::string("rpm"), "-qa", "--qf", "%{NAME}\t%{EPOCHNUM}\t%{VERSION}\t%{RELEASE}\t%{ARCH}\t%{SUMMARY}\n"},
          std::move(options), std::move(onComplete), {}, ParseMode::RpmInstalled, false, "rpm"
      );
    }

    bool launchFlatpak(
        std::vector<std::string> args, process::RunOptions options, CommandCompletion onComplete,
        ProgressCallback onProgress, ParseMode parse
    ) {
      return launchTracked(
          std::move(args), std::move(options), std::move(onComplete), std::move(onProgress), parse, false, "flatpak"
      );
    }

    bool launchFwupd(
        std::vector<std::string> args, process::RunOptions options, CommandCompletion onComplete,
        ProgressCallback onProgress, ParseMode parse
    ) {
      return launchTracked(
          std::move(args), std::move(options), std::move(onComplete), std::move(onProgress), parse, false, "fwupdmgr"
      );
    }

    void appendFlatpakScope(std::vector<std::string>& args, std::string_view installation) {
      if (installation == "system") {
        args.emplace_back("--system");
      } else if (installation == "user" || !installation.empty()) {
        args.emplace_back("--user");
      }
    }

    void finishFirmwareRefresh(std::function<void()> onComplete) {
      if (!process::commandExists(kFwupdmgr.data())) {
        g_pendingFirmwareCount.store(0, std::memory_order_release);
        if (onComplete) {
          onComplete();
        }
        return;
      }
      process::RunOptions options;
      options.timeout = std::chrono::seconds(45);
      options.maxOutputBytes = 2U << 20;
      if (!launchFwupd(
              {std::string(kFwupdmgr), "get-updates", "--json"}, std::move(options),
              [onComplete = std::move(onComplete)](CommandResult result) {
                if (result.success && !result.cancelled) {
                  g_pendingFirmwareCount.store(dedupeByName(std::move(result.hits)).size(), std::memory_order_release);
                }
                if (onComplete) {
                  onComplete();
                }
              },
              {}, ParseMode::FwupdUpdates
          )) {
        if (onComplete) {
          onComplete();
        }
      }
    }

    void finishPendingRefresh(std::function<void()> onComplete, bool fedoraOk) {
      if (!process::commandExists(kFlatpak.data())) {
        if (fedoraOk) {
          g_pendingFlatpakCount.store(0, std::memory_order_release);
        }
        finishFirmwareRefresh(std::move(onComplete));
        return;
      }
      process::RunOptions options;
      options.timeout = std::chrono::seconds(45);
      options.maxOutputBytes = 1U << 20;
      if (!launchFlatpak(
              {std::string(kFlatpak), "remote-ls", "--updates", "--app", "--columns",
               "application:f,name:f,description:f,version:f,origin:f"},
              std::move(options),
              [onComplete = std::move(onComplete)](CommandResult result) {
                if (result.success && !result.cancelled) {
                  g_pendingFlatpakCount.store(dedupeByName(std::move(result.hits)).size(), std::memory_order_release);
                }
                finishFirmwareRefresh(std::move(onComplete));
              },
              {}, ParseMode::FlatpakUpdates
          )) {
        finishFirmwareRefresh(std::move(onComplete));
      }
    }

  } // namespace

  bool backendAvailable() { return process::commandExists(kPkcon.data()); }

  bool flatpakAvailable() { return process::commandExists(kFlatpak.data()); }

  bool firmwareAvailable() { return process::commandExists(kFwupdmgr.data()); }

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

  std::size_t pendingUpdateCount() noexcept {
    return g_pendingRpmCount.load(std::memory_order_acquire)
        + g_pendingFlatpakCount.load(std::memory_order_acquire)
        + g_pendingFirmwareCount.load(std::memory_order_acquire);
  }

  void notePendingUpdates(Kind kind, std::size_t count) noexcept {
    if (kind == Kind::Flatpak) {
      g_pendingFlatpakCount.store(count, std::memory_order_release);
    } else if (kind == Kind::Firmware) {
      g_pendingFirmwareCount.store(count, std::memory_order_release);
    } else {
      g_pendingRpmCount.store(count, std::memory_order_release);
    }
  }

  bool refreshPendingUpdateCount(std::function<void()> onComplete) {
    if (!tabAvailable()) {
      g_pendingRpmCount.store(0, std::memory_order_release);
      g_pendingFlatpakCount.store(0, std::memory_order_release);
      g_pendingFirmwareCount.store(0, std::memory_order_release);
      if (onComplete) {
        onComplete();
      }
      return false;
    }
    if (busy()) {
      return false;
    }
    process::RunOptions options;
    options.timeout = std::chrono::seconds(60);
    options.maxOutputBytes = 1U << 20;
    return launchPkcon(
        {std::string(kPkcon), "--plain", "--noninteractive", "get-updates"}, std::move(options),
        [onComplete = std::move(onComplete)](CommandResult result) {
          const bool fedoraOk = result.success && !result.cancelled;
          if (fedoraOk) {
            g_pendingRpmCount.store(dedupeByName(std::move(result.hits)).size(), std::memory_order_release);
          }
          finishPendingRefresh(std::move(onComplete), fedoraOk);
        },
        {}, false
    );
  }

  void cancelCurrent() {
    if (g_cancelFlag) {
      g_cancelFlag->store(true, std::memory_order_release);
    }
  }

  bool search(std::string query, CommandCompletion onComplete, Kind kind) {
    if (!isSafeSearchQuery(query)) {
      return false;
    }
    if (kind == Kind::Firmware) {
      return false;
    }
    if (kind == Kind::Flatpak) {
      if (!flatpakAvailable()) {
        return false;
      }
      process::RunOptions options;
      options.timeout = std::chrono::seconds(90);
      options.maxOutputBytes = 2U << 20;
      return launchFlatpak(
          {std::string(kFlatpak), "search", "--json", std::move(query)}, std::move(options), std::move(onComplete),
          {}, ParseMode::FlatpakSearch
      );
    }
    if (!backendAvailable()) {
      return false;
    }
    process::RunOptions options;
    options.timeout = std::chrono::seconds(45);
    options.maxOutputBytes = 1U << 20;
    return launchPkcon(
        {std::string(kPkcon), "--plain", "--noninteractive", "search", "details", std::move(query)}, std::move(options),
        std::move(onComplete), {}, true
    );
  }

  bool listUpdates(CommandCompletion onComplete, Kind kind) {
    if (kind == Kind::Firmware) {
      if (!firmwareAvailable()) {
        return false;
      }
      process::RunOptions options;
      options.timeout = std::chrono::seconds(45);
      options.maxOutputBytes = 2U << 20;
      return launchFwupd(
          {std::string(kFwupdmgr), "get-updates", "--json"}, std::move(options), std::move(onComplete), {},
          ParseMode::FwupdUpdates
      );
    }
    if (kind == Kind::Flatpak) {
      if (!flatpakAvailable()) {
        return false;
      }
      process::RunOptions options;
      options.timeout = std::chrono::seconds(45);
      options.maxOutputBytes = 1U << 20;
      return launchFlatpak(
          {std::string(kFlatpak), "remote-ls", "--updates", "--app", "--columns",
           "application:f,name:f,description:f,version:f,origin:f"},
          std::move(options), std::move(onComplete), {}, ParseMode::FlatpakUpdates
      );
    }
    if (!backendAvailable()) {
      return false;
    }
    process::RunOptions options;
    options.timeout = std::chrono::seconds(60);
    options.maxOutputBytes = 1U << 20;
    return launchPkcon(
        {std::string(kPkcon), "--plain", "--noninteractive", "get-updates"}, std::move(options), std::move(onComplete),
        {}, true
    );
  }

  bool listInstalled(CommandCompletion onComplete, Kind kind) {
    if (kind == Kind::Firmware) {
      if (!firmwareAvailable()) {
        return false;
      }
      process::RunOptions options;
      options.timeout = std::chrono::seconds(30);
      options.maxOutputBytes = 4U << 20;
      return launchFwupd(
          {std::string(kFwupdmgr), "get-devices", "--json"}, std::move(options), std::move(onComplete), {},
          ParseMode::FwupdInstalled
      );
    }
    if (kind == Kind::Flatpak) {
      if (!flatpakAvailable()) {
        return false;
      }
      process::RunOptions options;
      options.timeout = std::chrono::seconds(20);
      options.maxOutputBytes = 4U << 20;
      return launchFlatpak(
          {std::string(kFlatpak), "list", "--app", "--columns",
           "application:f,name:f,version:f,branch:f,origin:f,installation:f"},
          std::move(options), std::move(onComplete), {}, ParseMode::FlatpakInstalled
      );
    }
    if (process::commandExists("rpm")) {
      return launchRpmInstalled(std::move(onComplete));
    }
    if (!backendAvailable()) {
      return false;
    }
    process::RunOptions options;
    options.timeout = std::chrono::seconds(120);
    options.maxOutputBytes = 8U << 20;
    return launchPkcon(
        {std::string(kPkcon), "--plain", "--noninteractive", "get-packages", "--filter", "installed"},
        std::move(options), std::move(onComplete), {}, true
    );
  }

  bool updateAll(CommandCompletion onComplete, ProgressCallback onProgress, Kind kind) {
    return updatePackages({}, std::move(onComplete), std::move(onProgress), kind);
  }

  bool updatePackages(
      std::vector<std::string> names, CommandCompletion onComplete, ProgressCallback onProgress, Kind kind
  ) {
    if (kind == Kind::Firmware) {
      if (!firmwareAvailable()) {
        return false;
      }
      std::vector<std::string> args = {
          std::string(kFwupdmgr), "update", "--assume-yes", "--no-reboot-check"
      };
      for (auto& name : names) {
        if (!isValidFwupdDeviceId(name)) {
          return false;
        }
        args.push_back(std::move(name));
      }
      process::RunOptions options;
      options.timeout = std::chrono::minutes(20);
      options.maxOutputBytes = 1U << 20;
      return launchFwupd(
          std::move(args), std::move(options), std::move(onComplete), std::move(onProgress), ParseMode::None
      );
    }
    if (kind == Kind::Flatpak) {
      if (!flatpakAvailable()) {
        return false;
      }
      std::vector<std::string> args = {
          std::string(kFlatpak), "update", "-y", "--noninteractive", "--app"
      };
      for (auto& name : names) {
        if (!isValidFlatpakAppId(name)) {
          return false;
        }
        args.push_back(std::move(name));
      }
      process::RunOptions options;
      options.timeout = std::chrono::minutes(15);
      options.maxOutputBytes = 1U << 20;
      return launchFlatpak(std::move(args), std::move(options), std::move(onComplete), std::move(onProgress), ParseMode::None);
    }
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

  bool mutate(
      Action action, std::string name, CommandCompletion onComplete, ProgressCallback onProgress, Kind kind,
      std::string installation, std::string remotes
  ) {
    if (kind == Kind::Firmware) {
      if (action != Action::Update || !isValidFwupdDeviceId(name) || !firmwareAvailable()) {
        return false;
      }
      process::RunOptions options;
      options.timeout = std::chrono::minutes(20);
      options.maxOutputBytes = 1U << 20;
      return launchFwupd(
          {std::string(kFwupdmgr), "update", "--assume-yes", "--no-reboot-check", std::move(name)}, std::move(options),
          std::move(onComplete), std::move(onProgress), ParseMode::None
      );
    }
    if (kind == Kind::Flatpak) {
      if (!isValidFlatpakAppId(name) || !flatpakAvailable()) {
        return false;
      }
      std::vector<std::string> args = {std::string(kFlatpak)};
      if (action == Action::Install) {
        args.insert(args.end(), {"install", "-y", "--noninteractive", "--user", "--app"});
        if (const auto remote = preferredFlatpakRemote(remotes); !remote.empty()) {
          args.push_back(remote);
        }
        args.push_back(std::move(name));
      } else if (action == Action::Remove) {
        args.insert(args.end(), {"uninstall", "-y", "--noninteractive"});
        appendFlatpakScope(args, installation);
        args.emplace_back("--app");
        args.push_back(std::move(name));
      } else {
        args.insert(args.end(), {"update", "-y", "--noninteractive"});
        appendFlatpakScope(args, installation);
        args.emplace_back("--app");
        args.push_back(std::move(name));
      }
      process::RunOptions options;
      options.timeout = std::chrono::minutes(15);
      options.maxOutputBytes = 1U << 20;
      return launchFlatpak(
          std::move(args), std::move(options), std::move(onComplete), std::move(onProgress), ParseMode::None
      );
    }
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
