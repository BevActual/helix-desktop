#include "system/ai_usage_service.h"

#include "cli/schema_msg.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "net/http_client.h"
#include "notification/notifications.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"
#include "system/terminal_launch.h"
#include "time/time_format.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("ai-usage");
  constexpr auto kClaudeMinInterval = std::chrono::seconds(60);
  constexpr auto kCliTimeout = std::chrono::seconds(10);
  constexpr auto kScanTimeout = std::chrono::seconds(20);

  [[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      return {};
    }
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
  }

  [[nodiscard]] std::string todayIso() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[11]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
  }

  [[nodiscard]] std::chrono::system_clock::time_point tomorrowLocal() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_mday += 1;
    tm.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
  }

  [[nodiscard]] std::string deepseekApiKey() {
    if (const char* env = std::getenv("DEEPSEEK_API_KEY"); env != nullptr && env[0] != '\0') {
      return StringUtils::trim(env);
    }
    return {};
  }

  [[nodiscard]] std::string stripTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') {
      value.pop_back();
    }
    return value;
  }

  [[nodiscard]] std::string normalizeOllamaBase(std::string host) {
    host = StringUtils::trim(host);
    if (host.empty()) {
      if (const char* env = std::getenv("OLLAMA_HOST"); env != nullptr && env[0] != '\0') {
        host = StringUtils::trim(env);
      }
    }
    if (host.empty()) {
      host = "127.0.0.1:11434";
    }
    if (!host.starts_with("http://") && !host.starts_with("https://")) {
      host = "http://" + host;
    }
    return stripTrailingSlash(std::move(host));
  }

  [[nodiscard]] bool hostLooksLocal(std::string_view base) {
    std::string_view rest = base;
    if (rest.starts_with("http://")) {
      rest.remove_prefix(7);
    } else if (rest.starts_with("https://")) {
      rest.remove_prefix(8);
    }
    const auto slash = rest.find('/');
    if (slash != std::string_view::npos) {
      rest = rest.substr(0, slash);
    }
    const auto colon = rest.rfind(':');
    if (colon != std::string_view::npos && rest.find(']') == std::string_view::npos) {
      rest = rest.substr(0, colon);
    }
    if (rest.size() >= 2 && rest.front() == '[' && rest.back() == ']') {
      rest = rest.substr(1, rest.size() - 2);
    }
    return rest == "127.0.0.1" || rest == "localhost" || rest == "::1" || rest == "0.0.0.0";
  }

  struct PromptScanResult {
    int count = 0;
    bool ok = false;
  };

  [[nodiscard]] PromptScanResult scanPromptDir(const std::filesystem::path& dir, std::string_view today) {
    PromptScanResult result;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
      return result;
    }
    const std::string sessionPrefix = "session-" + std::string(today);
    int files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (files >= 40) {
        break;
      }
      if (!entry.is_regular_file(ec)) {
        continue;
      }
      const auto name = entry.path().filename().string();
      const bool jsonl = name.starts_with(sessionPrefix) && name.ends_with(".jsonl");
      const bool logs = name == "logs.json";
      if (!jsonl && !logs) {
        continue;
      }
      ++files;
      result.count += noctalia::ai_usage::countPromptEvents(readFile(entry.path()), jsonl, today);
      result.ok = true;
    }
    return result;
  }

  struct CodexScanResult {
    std::string line;
    bool ok = false;
  };

  [[nodiscard]] CodexScanResult scanCodexSessions(const std::filesystem::path& dir) {
    CodexScanResult result;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
      return result;
    }
    struct FileTime {
      std::filesystem::path path;
      std::filesystem::file_time_type mtime;
    };
    std::vector<FileTime> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
      if (!entry.is_regular_file(ec)) {
        continue;
      }
      if (entry.path().extension() != ".jsonl") {
        continue;
      }
      files.push_back({entry.path(), entry.last_write_time(ec)});
    }
    std::ranges::sort(files, [](const FileTime& a, const FileTime& b) { return a.mtime > b.mtime; });
    if (files.size() > 12) {
      files.resize(12);
    }
    for (const auto& file : files) {
      const std::string raw = readFile(file.path);
      std::string last;
      std::size_t start = 0;
      while (start < raw.size()) {
        auto end = raw.find('\n', start);
        if (end == std::string::npos) {
          end = raw.size();
        }
        const auto line = std::string_view(raw).substr(start, end - start);
        start = end + 1;
        if (line.find("\"rate_limits\"") != std::string_view::npos) {
          last = std::string(line);
        }
      }
      if (!last.empty()) {
        result.line = std::move(last);
        result.ok = true;
        return result;
      }
    }
    return result;
  }

  void closeFd(int& fd) {
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
  }

  [[nodiscard]] std::optional<std::string> queryCodexAppServer() {
    if (!process::commandExists("codex")) {
      return std::nullopt;
    }
    int toChild[2] = {-1, -1};
    int fromChild[2] = {-1, -1};
    if (pipe(toChild) != 0 || pipe(fromChild) != 0) {
      closeFd(toChild[0]);
      closeFd(toChild[1]);
      closeFd(fromChild[0]);
      closeFd(fromChild[1]);
      return std::nullopt;
    }
    const pid_t pid = fork();
    if (pid == 0) {
      dup2(toChild[0], STDIN_FILENO);
      dup2(fromChild[1], STDOUT_FILENO);
      const int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
      }
      close(toChild[0]);
      close(toChild[1]);
      close(fromChild[0]);
      close(fromChild[1]);
      execlp("codex", "codex", "-s", "read-only", "-a", "untrusted", "app-server", static_cast<char*>(nullptr));
      _exit(127);
    }
    closeFd(toChild[0]);
    closeFd(fromChild[1]);
    if (pid < 0) {
      closeFd(toChild[1]);
      closeFd(fromChild[0]);
      return std::nullopt;
    }

    constexpr char kRequest[] =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"clientInfo\":{\"name\":\"helix\",\"version\":\"1\"}}}\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"account/rateLimits/read\",\"params\":{}}\n";
    (void)write(toChild[1], kRequest, sizeof(kRequest) - 1);
    closeFd(toChild[1]);

    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    char buf[4096];
    while (std::chrono::steady_clock::now() < deadline) {
      const auto remain =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      pollfd pfd{.fd = fromChild[0], .events = POLLIN, .revents = 0};
      const int ready = poll(&pfd, 1, static_cast<int>(std::max<std::int64_t>(remain.count(), 0)));
      if (ready <= 0) {
        break;
      }
      if ((pfd.revents & POLLIN) != 0) {
        const ssize_t n = read(fromChild[0], buf, sizeof(buf));
        if (n <= 0) {
          break;
        }
        out.append(buf, static_cast<std::size_t>(n));
      }
      if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
        break;
      }
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    closeFd(fromChild[0]);
    if (out.empty()) {
      return std::nullopt;
    }
    return out;
  }

  [[nodiscard]] std::optional<noctalia::ai_usage::ProviderSnapshot>
  parseCodexStdout(std::string_view text, std::int64_t nowEpoch) {
    std::optional<noctalia::ai_usage::ProviderSnapshot> parsed;
    std::size_t start = 0;
    while (start < text.size()) {
      auto end = text.find('\n', start);
      if (end == std::string_view::npos) {
        end = text.size();
      }
      const auto line = text.substr(start, end - start);
      start = end + 1;
      if (line.empty()) {
        continue;
      }
      if (auto next = noctalia::ai_usage::parseCodexRateLimitsJson(line, nowEpoch)) {
        parsed = std::move(next);
      }
    }
    if (!parsed.has_value()) {
      parsed = noctalia::ai_usage::parseCodexRateLimitsJson(text, nowEpoch);
    }
    return parsed;
  }

} // namespace

AiUsageService::AiUsageService(ConfigService& config, HttpClient& http) : m_config(&config), m_http(&http) {
  m_snapshot.providers.resize(noctalia::ai_usage::kProviderCount);
  for (std::size_t i = 0; i < noctalia::ai_usage::kProviderCount; ++i) {
    m_snapshot.providers[i].id = static_cast<noctalia::ai_usage::ProviderId>(i);
  }
}

AiUsageService::~AiUsageService() {
  m_timer.stop();
  if (m_http != nullptr && m_pullStream != 0) {
    m_http->cancelStream(m_pullStream);
    m_pullStream = 0;
  }
}

void AiUsageService::start() {
  if (m_started || m_config == nullptr) {
    return;
  }
  m_started = true;
  m_ollamaWantLoad = !m_config->config().controlCenter.aiUsage.ollamaModel.empty();
  m_config->addReloadCallback([this]() {
    m_ollamaWantLoad = !m_config->config().controlCenter.aiUsage.ollamaModel.empty();
    scheduleTimer();
    refresh(false);
  });
  refresh(true);
  scheduleTimer();
}

void AiUsageService::scheduleTimer() {
  if (m_config == nullptr) {
    return;
  }
  int seconds = m_config->config().controlCenter.aiUsage.refreshSeconds;
  seconds = std::clamp(seconds, 30, 900);
  m_timer.startRepeating(std::chrono::seconds(seconds), [this]() { refresh(false); });
}

void AiUsageService::refresh(bool force) { fetchAll(force); }

void AiUsageService::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::aiUsageStatus, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: ai-usage-status takes no arguments\n";
    }
    return noctalia::ai_usage::formatSnapshotDump(m_snapshot);
  });
  ipc.bind(noctalia::cli::msg::aiUsageRefresh, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: ai-usage-refresh takes no arguments\n";
    }
    refresh(true);
    return "ok\n";
  });
  ipc.bind(noctalia::cli::msg::aiUsageOllamaStart, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: ai-usage-ollama-start takes no arguments\n";
    }
    startOllama();
    return "ok\n";
  });
  ipc.bind(noctalia::cli::msg::aiUsageOllamaStop, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: ai-usage-ollama-stop takes no arguments\n";
    }
    stopOllama();
    return "ok\n";
  });
  ipc.bind(noctalia::cli::msg::aiUsageOllamaModel, [this](const std::string& args) -> std::string {
    const auto words = noctalia::ipc::splitWords(args);
    if (words.size() != 1) {
      return "error: ai-usage-ollama-model requires a model name\n";
    }
    selectOllamaModel(words.front());
    return "ok\n";
  });
  ipc.bind(noctalia::cli::msg::aiUsageOllamaPull, [this](const std::string& args) -> std::string {
    const auto words = noctalia::ipc::splitWords(args);
    if (words.size() != 1) {
      return "error: ai-usage-ollama-pull requires a model name\n";
    }
    pullOllamaModel(words.front());
    return "ok\n";
  });
  ipc.bind(noctalia::cli::msg::aiUsageOllamaCancel, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: ai-usage-ollama-cancel takes no arguments\n";
    }
    cancelOllamaPull();
    return "ok\n";
  });
  ipc.bind(noctalia::cli::msg::aiUsageOllamaDelete, [this](const std::string& args) -> std::string {
    const auto words = noctalia::ipc::splitWords(args);
    if (words.size() != 1) {
      return "error: ai-usage-ollama-delete requires a model name\n";
    }
    deleteOllamaModel(words.front());
    return "ok\n";
  });
}

noctalia::ai_usage::ProviderSnapshot& AiUsageService::slot(noctalia::ai_usage::ProviderId id) {
  return m_snapshot.providers[static_cast<std::size_t>(id)];
}

void AiUsageService::addChangeCallback(ChangeCallback callback) {
  if (callback) {
    m_changeCallbacks.push_back(std::move(callback));
  }
}

void AiUsageService::notifyChanged() {
  emitQuotaWarnings();
  for (const auto& callback : m_changeCallbacks) {
    callback();
  }
}

int AiUsageService::attentionPercent() const {
  if (m_config == nullptr) {
    return 90;
  }
  const int warn = m_config->config().controlCenter.aiUsage.warnPercent;
  return warn > 0 ? warn : 90;
}

void AiUsageService::emitQuotaWarnings() {
  if (m_config == nullptr) {
    return;
  }
  const auto& cfg = m_config->config().controlCenter.aiUsage;
  const auto now = std::chrono::system_clock::now();
  const auto warnings = noctalia::ai_usage::takeQuotaWarnings(
      m_snapshot, cfg.warnPercent, cfg.warnResetMinutes, m_warningState, now
  );
  for (const auto& warning : warnings) {
    const std::string provider = noctalia::ai_usage::providerLabel(warning.provider);
    const std::string window = noctalia::ai_usage::windowLabel(warning.window.key);
    const int percent = static_cast<int>(std::lround(warning.window.percent));
    std::string resetIn;
    std::string resetDuration;
    if (warning.window.reset.has_value()) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::seconds>(*warning.window.reset - now);
      if (remaining > std::chrono::seconds{0}) {
        resetDuration = formatDuration(remaining);
        resetIn = i18n::tr("control-center.ai-usage.resets-in", "time", resetDuration);
      }
    }
    if (warning.kind == noctalia::ai_usage::QuotaWarningKind::High) {
      std::string body = i18n::tr(
          "notifications.internal.ai-usage-high-body", "provider", provider, "window", window, "percent", percent
      );
      if (!resetIn.empty()) {
        body += " ";
        body += resetIn;
      }
      notify::info(i18n::tr("notifications.internal.ai-usage-high-title", "provider", provider), std::move(body));
    } else {
      notify::info(
          i18n::tr("notifications.internal.ai-usage-reset-title", "provider", provider),
          i18n::tr(
              "notifications.internal.ai-usage-reset-body", "provider", provider, "window", window, "percent",
              percent, "reset", resetDuration
          )
      );
    }
  }
}

void AiUsageService::publish(noctalia::ai_usage::ProviderSnapshot snap) {
  snap.launchable = canLaunch(snap.id);
  const auto now = std::chrono::system_clock::now();
  snap.fetchedAt = now;
  slot(snap.id) = std::move(snap);
  m_snapshot.fetchedAt = now;
  m_snapshot.refreshing = false;
  notifyChanged();
}

void AiUsageService::publishOllama(noctalia::ai_usage::OllamaSnapshot snap) {
  snap.busy = m_snapshot.ollama.busy;
  snap.loading = m_snapshot.ollama.loading;
  snap.deleting = m_snapshot.ollama.deleting;
  snap.wantLoaded = m_ollamaWantLoad;
  if (m_config != nullptr) {
    snap.selected = m_config->config().controlCenter.aiUsage.ollamaModel;
  }
  if (snap.models.empty()
      && (snap.status == noctalia::ai_usage::OllamaStatus::Stopped
          || snap.status == noctalia::ai_usage::OllamaStatus::Error)
      && !m_snapshot.ollama.models.empty()) {
    snap.models = m_snapshot.ollama.models;
    for (auto& model : snap.models) {
      model.loaded = false;
    }
  }
  if (snap.selected.empty() && snap.local && snap.status == noctalia::ai_usage::OllamaStatus::Running) {
    std::string only;
    int loaded = 0;
    for (const auto& model : snap.models) {
      if (model.loaded) {
        ++loaded;
        only = model.name;
      }
    }
    if (loaded == 1 && m_config != nullptr) {
      snap.selected = only;
      (void)m_config->setOverride({"control_center", "ai_usage", "ollama_model"}, only);
    }
  }
  noctalia::ai_usage::sortOllamaModels(snap.models, snap.selected);
  m_snapshot.ollama = std::move(snap);
  m_snapshot.fetchedAt = std::chrono::system_clock::now();
  m_snapshot.refreshing = false;
  notifyChanged();
  if (m_ollamaWantLoad && !m_ollamaLoadPending) {
    ensureSelectedOllamaModel();
  }
  if (!m_pendingPull.empty() && m_snapshot.ollama.status == noctalia::ai_usage::OllamaStatus::Running
      && !m_snapshot.ollama.busy) {
    std::string name = std::move(m_pendingPull);
    m_pendingPull.clear();
    pullOllamaModel(std::move(name));
  } else if (!m_pendingPull.empty() && !m_snapshot.ollama.busy
             && m_snapshot.ollama.status != noctalia::ai_usage::OllamaStatus::Running) {
    m_pendingPull.clear();
    finishOllamaPull(false, "Could not start Ollama");
  }
  if (!m_pendingDelete.empty() && m_snapshot.ollama.status == noctalia::ai_usage::OllamaStatus::Running
      && !m_snapshot.ollama.busy && m_snapshot.ollama.deleting.empty()) {
    std::string name = std::move(m_pendingDelete);
    m_pendingDelete.clear();
    deleteOllamaModel(std::move(name));
  } else if (!m_pendingDelete.empty() && !m_snapshot.ollama.busy
             && m_snapshot.ollama.status != noctalia::ai_usage::OllamaStatus::Running) {
    m_pendingDelete.clear();
    m_snapshot.ollama.deleting.clear();
    notifyChanged();
  }
}

void AiUsageService::publishLocalRuntimes(std::vector<noctalia::ai_usage::LocalRuntime> runtimes) {
  m_snapshot.localRuntimes = std::move(runtimes);
  m_snapshot.fetchedAt = std::chrono::system_clock::now();
  m_snapshot.refreshing = false;
  notifyChanged();
}

void AiUsageService::publishError(noctalia::ai_usage::ProviderId id, noctalia::ai_usage::Status status) {
  auto& current = slot(id);
  if (current.status == noctalia::ai_usage::Status::Ok) {
    current.stale = true;
    current.fetchedAt = std::chrono::system_clock::now();
    m_snapshot.fetchedAt = current.fetchedAt;
    m_snapshot.refreshing = false;
    notifyChanged();
    return;
  }
  noctalia::ai_usage::ProviderSnapshot snap;
  snap.id = id;
  snap.status = status;
  publish(std::move(snap));
}

void AiUsageService::fetchAll(bool force) {
  if (m_config == nullptr) {
    return;
  }
  const auto& cfg = m_config->config().controlCenter.aiUsage;
  const auto generation = ++m_generation;
  m_snapshot.refreshing = true;
  notifyChanged();

  const auto run = [&](noctalia::ai_usage::ProviderId id, auto detect, auto fetch) {
    using Mode = AiUsageProviderMode;
    Mode mode = Mode::Auto;
    switch (id) {
    case noctalia::ai_usage::ProviderId::Claude:
      mode = cfg.claude;
      break;
    case noctalia::ai_usage::ProviderId::Codex:
      mode = cfg.codex;
      break;
    case noctalia::ai_usage::ProviderId::Gemini:
      mode = cfg.gemini;
      break;
    case noctalia::ai_usage::ProviderId::Qwen:
      mode = cfg.qwen;
      break;
    case noctalia::ai_usage::ProviderId::Copilot:
      mode = cfg.copilot;
      break;
    case noctalia::ai_usage::ProviderId::Cursor:
      mode = cfg.cursor;
      break;
    case noctalia::ai_usage::ProviderId::DeepSeek:
      mode = cfg.deepseek;
      break;
    }
    if (mode == Mode::Off) {
      noctalia::ai_usage::ProviderSnapshot snap;
      snap.id = id;
      snap.status = noctalia::ai_usage::Status::Disabled;
      publish(std::move(snap));
      return;
    }
    if (mode == Mode::Auto && !detect()) {
      noctalia::ai_usage::ProviderSnapshot snap;
      snap.id = id;
      snap.status = noctalia::ai_usage::Status::Absent;
      publish(std::move(snap));
      return;
    }
    fetch();
  };

  const auto cliOnPath = [](noctalia::ai_usage::ProviderId id) {
    if (!noctalia::ai_usage::providerIsCli(id)) {
      return false;
    }
    const auto cli = noctalia::ai_usage::providerCli(id);
    return !cli.empty() && process::commandExists(std::string(cli).c_str());
  };

  run(
      noctalia::ai_usage::ProviderId::Claude, [&]() { return cliOnPath(noctalia::ai_usage::ProviderId::Claude); },
      [&]() { fetchClaude(generation, force); }
  );
  run(
      noctalia::ai_usage::ProviderId::Codex, [&]() { return cliOnPath(noctalia::ai_usage::ProviderId::Codex); },
      [&]() { fetchCodex(generation); }
  );
  run(
      noctalia::ai_usage::ProviderId::Gemini, [&]() { return cliOnPath(noctalia::ai_usage::ProviderId::Gemini); },
      [&]() { fetchGemini(generation); }
  );
  run(
      noctalia::ai_usage::ProviderId::Qwen, [&]() { return cliOnPath(noctalia::ai_usage::ProviderId::Qwen); },
      [&]() { fetchQwen(generation); }
  );
  run(
      noctalia::ai_usage::ProviderId::Copilot, []() { return false; },
      [&]() { fetchCopilot(generation); }
  );
  run(
      noctalia::ai_usage::ProviderId::Cursor, []() { return false; },
      [&]() { fetchCursor(generation); }
  );
  run(
      noctalia::ai_usage::ProviderId::DeepSeek, []() { return false; },
      [&]() { fetchDeepseek(generation); }
  );
  fetchOllama(generation);
  fetchLocalRuntimes(generation, force);
}

void AiUsageService::fetchClaude(std::uint64_t generation, bool force) {
  const auto now = std::chrono::steady_clock::now();
  if (!force && m_claudeLastProbe.time_since_epoch().count() != 0 && now - m_claudeLastProbe < kClaudeMinInterval) {
    m_snapshot.refreshing = false;
    notifyChanged();
    return;
  }
  m_claudeLastProbe = now;
  const auto path = FileUtils::expandUserPath("~/.claude/.credentials.json");
  const auto creds = noctalia::ai_usage::parseClaudeCredentialsJson(readFile(path));
  if (!creds.has_value()) {
    publishError(noctalia::ai_usage::ProviderId::Claude, noctalia::ai_usage::Status::NoCredentials);
    return;
  }
  if (creds->expiresAtMs > 0
      && creds->expiresAtMs / 1000 <= std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())) {
    publishError(noctalia::ai_usage::ProviderId::Claude, noctalia::ai_usage::Status::Expired);
    return;
  }
  if (m_http == nullptr) {
    publishError(noctalia::ai_usage::ProviderId::Claude, noctalia::ai_usage::Status::HttpError);
    return;
  }
  HttpRequest req;
  req.url = "https://api.anthropic.com/api/oauth/usage";
  req.headers = {
      "Authorization: Bearer " + creds->accessToken, "anthropic-beta: oauth-2025-04-20", "Accept: application/json"
  };
  const std::string plan = creds->plan;
  m_http->request(std::move(req), [this, generation, plan](HttpResponse res) {
    if (generation != m_generation) {
      return;
    }
    if (!res.transportOk || res.status < 200 || res.status >= 300) {
      if (res.status == 401 || res.status == 403) {
        publishError(noctalia::ai_usage::ProviderId::Claude, noctalia::ai_usage::Status::Expired);
      } else {
        publishError(noctalia::ai_usage::ProviderId::Claude, noctalia::ai_usage::Status::HttpError);
      }
      return;
    }
    auto parsed = noctalia::ai_usage::parseClaudeUsageJson(res.body, plan);
    if (!parsed.has_value()) {
      publishError(noctalia::ai_usage::ProviderId::Claude, noctalia::ai_usage::Status::HttpError);
      return;
    }
    publish(std::move(*parsed));
  });
}

void AiUsageService::fetchCodex(std::uint64_t generation) {
  const auto dir = FileUtils::expandUserPath("~/.codex/sessions");
  std::thread([this, generation, dir]() {
    const auto live = queryCodexAppServer();
    const auto now = static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    auto parsed = live.has_value() ? parseCodexStdout(*live, now) : std::nullopt;
    CodexScanResult scanned;
    if (!parsed.has_value()) {
      scanned = scanCodexSessions(dir);
      if (scanned.ok) {
        parsed = noctalia::ai_usage::parseCodexRateLimitsJson(scanned.line, now);
      }
    }
    DeferredCall::callLater([this, generation, parsed = std::move(parsed)]() mutable {
      if (generation != m_generation) {
        return;
      }
      if (!parsed.has_value()) {
        publishError(noctalia::ai_usage::ProviderId::Codex, noctalia::ai_usage::Status::NoData);
        return;
      }
      publish(std::move(*parsed));
    });
  }).detach();
  (void)kScanTimeout;
}

void AiUsageService::fetchGemini(std::uint64_t generation) {
  if (m_config == nullptr) {
    return;
  }
  fetchPromptCounter(
      noctalia::ai_usage::ProviderId::Gemini, generation, FileUtils::expandUserPath("~/.gemini/tmp").string(),
      m_config->config().controlCenter.aiUsage.geminiDailyLimit
  );
}

void AiUsageService::fetchQwen(std::uint64_t generation) {
  if (m_config == nullptr) {
    return;
  }
  fetchPromptCounter(
      noctalia::ai_usage::ProviderId::Qwen, generation, FileUtils::expandUserPath("~/.qwen/tmp").string(),
      m_config->config().controlCenter.aiUsage.qwenDailyLimit
  );
}

void AiUsageService::fetchPromptCounter(
    noctalia::ai_usage::ProviderId id, std::uint64_t generation, std::string dir, int limit
) {
  const int quota = limit > 0 ? limit : 1000;
  std::thread([this, id, generation, dir = std::move(dir), quota]() {
    const auto scanned = scanPromptDir(dir, todayIso());
    DeferredCall::callLater([this, id, generation, scanned, quota]() {
      if (generation != m_generation) {
        return;
      }
      if (!scanned.ok && scanned.count == 0) {
        publishError(id, noctalia::ai_usage::Status::NoData);
        return;
      }
      noctalia::ai_usage::ProviderSnapshot snap;
      snap.id = id;
      snap.status = noctalia::ai_usage::Status::Ok;
      snap.plan = std::to_string(scanned.count) + "/" + std::to_string(quota);
      noctalia::ai_usage::Window window;
      window.key = noctalia::ai_usage::WindowKey::Day;
      window.percent = noctalia::ai_usage::clampPercent((static_cast<double>(scanned.count) / quota) * 100.0);
      window.reset = tomorrowLocal();
      window.used = scanned.count;
      window.max = quota;
      snap.windows.push_back(window);
      publish(std::move(snap));
    });
  }).detach();
}

void AiUsageService::fetchCopilot(std::uint64_t generation) {
  if (!process::runAsync(
      std::vector<std::string>{"gh", "auth", "token"},
      process::RunCallbacks{
          .onExit =
              [this, generation](process::RunResult result) {
                DeferredCall::callLater([this, generation, result = std::move(result)]() mutable {
                  if (generation != m_generation) {
                    return;
                  }
                  const std::string token = StringUtils::trim(result.out);
                  if (!result || token.empty()) {
                    publishError(noctalia::ai_usage::ProviderId::Copilot, noctalia::ai_usage::Status::NoAuth);
                    return;
                  }
                  if (m_http == nullptr) {
                    publishError(noctalia::ai_usage::ProviderId::Copilot, noctalia::ai_usage::Status::HttpError);
                    return;
                  }
                  HttpRequest req;
                  req.url = "https://api.github.com/copilot_internal/user";
                  req.headers = {"Authorization: token " + token, "Accept: application/json"};
                  m_http->request(std::move(req), [this, generation](HttpResponse res) {
                    if (generation != m_generation) {
                      return;
                    }
                    if (!res.transportOk || res.status < 200 || res.status >= 300) {
                      publishError(
                          noctalia::ai_usage::ProviderId::Copilot,
                          res.status == 401 || res.status == 403 ? noctalia::ai_usage::Status::NoAuth
                                                                 : noctalia::ai_usage::Status::HttpError
                      );
                      return;
                    }
                    auto parsed = noctalia::ai_usage::parseCopilotUsageJson(res.body);
                    if (!parsed.has_value()) {
                      publishError(noctalia::ai_usage::ProviderId::Copilot, noctalia::ai_usage::Status::HttpError);
                      return;
                    }
                    publish(std::move(*parsed));
                  });
                });
              },
      },
      process::RunOptions{.timeout = kCliTimeout}
  )) {
    publishError(noctalia::ai_usage::ProviderId::Copilot, noctalia::ai_usage::Status::NoAuth);
  }
}

void AiUsageService::fetchCursor(std::uint64_t generation) {
  const std::filesystem::path db{FileUtils::expandXdgBaseDir("$XDG_CONFIG_HOME/Cursor/User/globalStorage/state.vscdb")};
  if (!process::runAsync(
      std::vector<std::string>{
          "sqlite3", "-readonly", db.string(), "SELECT value FROM ItemTable WHERE key = 'cursorAuth/accessToken'"
      },
      process::RunCallbacks{
          .onExit =
              [this, generation](process::RunResult result) {
                DeferredCall::callLater([this, generation, result = std::move(result)]() mutable {
                  if (generation != m_generation) {
                    return;
                  }
                  const std::string token = StringUtils::trim(result.out);
                  if (!result || token.empty()) {
                    publishError(noctalia::ai_usage::ProviderId::Cursor, noctalia::ai_usage::Status::NoAuth);
                    return;
                  }
                  const auto subject = noctalia::ai_usage::jwtSubject(token);
                  if (!subject.has_value()) {
                    publishError(noctalia::ai_usage::ProviderId::Cursor, noctalia::ai_usage::Status::NoAuth);
                    return;
                  }
                  const std::string userId = noctalia::ai_usage::cursorUserId(*subject);
                  if (userId.empty() || m_http == nullptr) {
                    publishError(noctalia::ai_usage::ProviderId::Cursor, noctalia::ai_usage::Status::NoAuth);
                    return;
                  }
                  HttpRequest req;
                  req.url = "https://cursor.com/api/usage?user=" + StringUtils::urlEncode(userId);
                  req.headers = {
                      "Cookie: WorkosCursorSessionToken=" + StringUtils::urlEncode(userId + "::" + token),
                      "Accept: application/json"
                  };
                  m_http->request(std::move(req), [this, generation](HttpResponse res) {
                    if (generation != m_generation) {
                      return;
                    }
                    if (!res.transportOk || res.status < 200 || res.status >= 300) {
                      publishError(
                          noctalia::ai_usage::ProviderId::Cursor,
                          res.status == 401 || res.status == 403 ? noctalia::ai_usage::Status::NoAuth
                                                                 : noctalia::ai_usage::Status::HttpError
                      );
                      return;
                    }
                    auto parsed = noctalia::ai_usage::parseCursorUsageJson(res.body);
                    if (!parsed.has_value()) {
                      publishError(noctalia::ai_usage::ProviderId::Cursor, noctalia::ai_usage::Status::HttpError);
                      return;
                    }
                    publish(std::move(*parsed));
                  });
                });
              },
      },
      process::RunOptions{.timeout = kCliTimeout}
  )) {
    publishError(noctalia::ai_usage::ProviderId::Cursor, noctalia::ai_usage::Status::NoAuth);
  }
}

void AiUsageService::fetchDeepseek(std::uint64_t generation) {
  const std::string key = deepseekApiKey();
  if (key.empty()) {
    publishError(noctalia::ai_usage::ProviderId::DeepSeek, noctalia::ai_usage::Status::NoAuth);
    return;
  }
  if (m_http == nullptr) {
    publishError(noctalia::ai_usage::ProviderId::DeepSeek, noctalia::ai_usage::Status::HttpError);
    return;
  }
  HttpRequest req;
  req.url = "https://api.deepseek.com/user/balance";
  req.headers = {"Authorization: Bearer " + key, "Accept: application/json"};
  m_http->request(std::move(req), [this, generation](HttpResponse res) {
    if (generation != m_generation) {
      return;
    }
    if (!res.transportOk || res.status < 200 || res.status >= 300) {
      publishError(
          noctalia::ai_usage::ProviderId::DeepSeek,
          res.status == 401 || res.status == 403 ? noctalia::ai_usage::Status::NoAuth
                                                 : noctalia::ai_usage::Status::HttpError
      );
      return;
    }
    auto parsed = noctalia::ai_usage::parseDeepseekBalanceJson(res.body);
    if (!parsed.has_value()) {
      publishError(noctalia::ai_usage::ProviderId::DeepSeek, noctalia::ai_usage::Status::HttpError);
      return;
    }
    publish(std::move(*parsed));
  });
}

bool AiUsageService::canLaunchDesktop(std::span<const std::string_view> ids) const {
  const auto entries = desktopEntries();
  for (const auto desktopId : ids) {
    if (app_identity::findDesktopEntry(desktopId, entries).has_value()) {
      return true;
    }
  }
  return false;
}

bool AiUsageService::launchDesktopIds(std::span<const std::string_view> ids) const {
  const auto entries = desktopEntries();
  for (const auto desktopId : ids) {
    if (auto entry = app_identity::findDesktopEntry(desktopId, entries); entry.has_value()) {
      desktop_entry_launch::LaunchOptions options;
      options.dbusActivatable = entry->dbusActivatable;
      options.dbusAppId = entry->id;
      if (desktop_entry_launch::launchEntry(*entry, options)) {
        return true;
      }
    }
  }
  return false;
}

bool AiUsageService::canLaunch(noctalia::ai_usage::ProviderId id) const {
  if (noctalia::ai_usage::providerIsCli(id)) {
    const auto cli = noctalia::ai_usage::providerCli(id);
    return !cli.empty() && process::commandExists(std::string(cli).c_str());
  }
  return canLaunchDesktop(noctalia::ai_usage::providerDesktopIds(id));
}

bool AiUsageService::canLaunchOllama() const {
  return canLaunchDesktop(noctalia::ai_usage::ollamaDesktopIds());
}

void AiUsageService::launchProvider(noctalia::ai_usage::ProviderId id) {
  if (noctalia::ai_usage::providerIsCli(id)) {
    const auto cli = noctalia::ai_usage::providerCli(id);
    if (!cli.empty() && process::commandExists(std::string(cli).c_str())) {
      (void)terminal_launch::launch(std::string(cli));
    }
    return;
  }
  (void)launchDesktopIds(noctalia::ai_usage::providerDesktopIds(id));
}

void AiUsageService::launchOllama() {
  (void)launchDesktopIds(noctalia::ai_usage::ollamaDesktopIds());
}

void AiUsageService::launchLocalRuntime(noctalia::ai_usage::LocalRuntimeKind /*kind*/) {}

bool AiUsageService::canLaunchLocalRuntime(noctalia::ai_usage::LocalRuntimeKind /*kind*/) const { return false; }

std::string AiUsageService::ollamaBaseUrl() const {
  if (m_config == nullptr) {
    return normalizeOllamaBase({});
  }
  return normalizeOllamaBase(m_config->config().controlCenter.aiUsage.ollamaHost);
}

bool AiUsageService::ollamaHostIsLocal() const { return hostLooksLocal(ollamaBaseUrl()); }

void AiUsageService::fetchOllama(std::uint64_t generation) {
  using noctalia::ai_usage::OllamaSnapshot;
  using noctalia::ai_usage::OllamaStatus;
  if (m_config == nullptr) {
    return;
  }
  const auto mode = m_config->config().controlCenter.aiUsage.ollama;
  const bool hasCli = process::commandExists("ollama");
  const bool local = ollamaHostIsLocal();
  const bool hostConfigured = !m_config->config().controlCenter.aiUsage.ollamaHost.empty();
  if (mode == AiUsageProviderMode::Off) {
    OllamaSnapshot snap;
    snap.status = OllamaStatus::Disabled;
    snap.hasCli = hasCli;
    snap.local = local;
    publishOllama(std::move(snap));
    return;
  }
  if (mode == AiUsageProviderMode::Auto && !hasCli && local && !hostConfigured) {
    OllamaSnapshot snap;
    snap.status = OllamaStatus::Absent;
    snap.hasCli = false;
    snap.local = local;
    publishOllama(std::move(snap));
    return;
  }
  if (m_http == nullptr) {
    OllamaSnapshot snap;
    snap.status = hasCli ? OllamaStatus::Stopped : OllamaStatus::Error;
    snap.hasCli = hasCli;
    snap.local = local;
    publishOllama(std::move(snap));
    return;
  }
  const std::string base = ollamaBaseUrl();
  HttpRequest tags;
  tags.url = base + "/api/tags";
  tags.allowWhenOffline = local;
  tags.freshConnection = true;
  m_http->request(std::move(tags), [this, generation, hasCli, local, base](HttpResponse tagsRes) {
    if (generation != m_generation) {
      return;
    }
    if (!tagsRes.transportOk || tagsRes.status < 200 || tagsRes.status >= 300) {
      OllamaSnapshot snap;
      snap.status = hasCli ? OllamaStatus::Stopped : (local ? OllamaStatus::Absent : OllamaStatus::Error);
      snap.hasCli = hasCli;
      snap.local = local;
      publishOllama(std::move(snap));
      return;
    }
    HttpRequest ps;
    ps.url = base + "/api/ps";
    ps.allowWhenOffline = local;
    ps.freshConnection = true;
    m_http->request(std::move(ps), [this, generation, hasCli, local, base, tagsBody = tagsRes.body](HttpResponse psRes) {
      if (generation != m_generation) {
        return;
      }
      HttpRequest version;
      version.url = base + "/api/version";
      version.allowWhenOffline = local;
      m_http->request(
          std::move(version),
          [this, generation, hasCli, local, tagsBody, psBody = psRes.transportOk ? psRes.body : std::string{}](
              HttpResponse versionRes
          ) {
            if (generation != m_generation) {
              return;
            }
            auto parsed = noctalia::ai_usage::parseOllamaSnapshot(
                tagsBody, psBody, versionRes.transportOk ? versionRes.body : std::string{}
            );
            if (!parsed.has_value()) {
              OllamaSnapshot snap;
              snap.status = OllamaStatus::Error;
              snap.hasCli = hasCli;
              snap.local = local;
              publishOllama(std::move(snap));
              return;
            }
            parsed->hasCli = hasCli;
            parsed->local = local;
            parsed->status = OllamaStatus::Running;
            publishOllama(std::move(*parsed));
          }
      );
    });
  });
}

// llama.cpp and vLLM servers are usually not running. A refused connect is the normal
// answer, so each absent port is re-probed on a doubling schedule (up to ten minutes)
// instead of every refresh; a forced refresh (tab open) probes everything at once.
namespace {
  constexpr std::chrono::seconds kLocalRuntimeInitialBackoff{60};
  constexpr std::chrono::seconds kLocalRuntimeMaxBackoff{600};
} // namespace

void AiUsageService::fetchLocalRuntimes(std::uint64_t generation, bool force) {
  if (m_http == nullptr) {
    publishLocalRuntimes({});
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  std::vector<noctalia::ai_usage::LocalRuntimeKind> kinds;
  kinds.reserve(noctalia::ai_usage::kLocalRuntimeCount);
  for (std::size_t i = 0; i < noctalia::ai_usage::kLocalRuntimeCount; ++i) {
    const auto kind = static_cast<noctalia::ai_usage::LocalRuntimeKind>(i);
    if (!noctalia::ai_usage::localRuntimeIsCli(kind)) {
      continue;
    }
    if (!force && m_localRuntimeProbes[i].nextProbeAt > now) {
      continue;
    }
    kinds.push_back(kind);
  }
  if (kinds.empty()) {
    publishLocalRuntimes(m_localRuntimesKnown);
    return;
  }
  auto pending = std::make_shared<std::size_t>(kinds.size());
  auto found = std::make_shared<std::vector<noctalia::ai_usage::LocalRuntime>>();
  // Ports skipped this round keep their last known answer.
  for (const auto& runtime : m_localRuntimesKnown) {
    if (!std::ranges::contains(kinds, runtime.kind)) {
      found->push_back(runtime);
    }
  }
  for (const auto kind : kinds) {
    const auto port = noctalia::ai_usage::localRuntimePort(kind);
    HttpRequest req;
    req.url = std::format("http://127.0.0.1:{}/v1/models", port);
    req.allowWhenOffline = true;
    req.freshConnection = true;
    req.quietTransportErrors = true;
    req.timeoutSeconds = 2;
    m_http->request(
        std::move(req),
        [this, generation, pending, found, kind, port](HttpResponse res) {
          if (generation != m_generation) {
            return;
          }
          auto& probe = m_localRuntimeProbes[static_cast<std::size_t>(kind)];
          bool alive = false;
          if (res.transportOk && res.status >= 200 && res.status < 300) {
            if (auto models = noctalia::ai_usage::parseOpenAiModelsJson(res.body)) {
              noctalia::ai_usage::LocalRuntime runtime;
              runtime.kind = kind;
              runtime.port = port;
              runtime.models = std::move(*models);
              runtime.launchable = false;
              found->push_back(std::move(runtime));
              alive = true;
            }
          }
          if (alive) {
            probe.backoff = std::chrono::seconds(0);
            probe.nextProbeAt = {};
          } else {
            probe.backoff = probe.backoff.count() == 0 ? kLocalRuntimeInitialBackoff
                                                       : std::min(probe.backoff * 2, kLocalRuntimeMaxBackoff);
            probe.nextProbeAt = std::chrono::steady_clock::now() + probe.backoff;
          }
          if (--*pending == 0) {
            std::ranges::sort(*found, [](const auto& a, const auto& b) {
              return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
            });
            m_localRuntimesKnown = *found;
            publishLocalRuntimes(std::move(*found));
          }
        }
    );
  }
}

void AiUsageService::spawnOllamaServe() {
  m_ollamaServePid = process::launchDetachedTracked(std::vector<std::string>{"ollama", "serve"});
  DeferredCall::callLater([this]() {
    m_snapshot.ollama.busy = false;
    refresh(true);
  });
}

void AiUsageService::startOllama() {
  if (!process::commandExists("ollama") || !ollamaHostIsLocal()) {
    return;
  }
  if (m_pendingDelete.empty() && m_snapshot.ollama.deleting.empty() && m_config != nullptr
      && !m_config->config().controlCenter.aiUsage.ollamaModel.empty()) {
    m_ollamaWantLoad = true;
    m_snapshot.ollama.wantLoaded = true;
  }
  m_snapshot.ollama.busy = true;
  notifyChanged();
  if (process::runAsync(
          std::vector<std::string>{"systemctl", "--user", "start", "ollama"},
          process::RunCallbacks{
              .onExit =
                  [this](process::RunResult result) {
                    DeferredCall::callLater([this, ok = static_cast<bool>(result)]() {
                      if (ok) {
                        m_snapshot.ollama.busy = false;
                        refresh(true);
                        return;
                      }
                      spawnOllamaServe();
                    });
                  },
          },
          process::RunOptions{.timeout = kCliTimeout}
      )) {
    return;
  }
  spawnOllamaServe();
}

void AiUsageService::stopOllama() {
  if (!ollamaHostIsLocal()) {
    return;
  }
  m_pendingPull.clear();
  m_pendingDelete.clear();
  cancelOllamaPull();
  m_ollamaWantLoad = false;
  m_ollamaLoadPending = false;
  m_snapshot.ollama.wantLoaded = false;
  m_snapshot.ollama.loading.clear();
  m_snapshot.ollama.deleting.clear();
  m_snapshot.ollama.busy = true;
  notifyChanged();
  if (m_ollamaServePid.has_value()) {
    process::terminateTracked(*m_ollamaServePid);
    m_ollamaServePid.reset();
  }
  if (!process::runAsync(
          std::vector<std::string>{"systemctl", "--user", "stop", "ollama"},
          process::RunCallbacks{
              .onExit =
                  [this](process::RunResult) {
                    DeferredCall::callLater([this]() {
                      m_snapshot.ollama.busy = false;
                      refresh(true);
                    });
                  },
          },
          process::RunOptions{.timeout = kCliTimeout}
      )) {
    m_snapshot.ollama.busy = false;
    refresh(true);
  }
}

void AiUsageService::selectOllamaModel(std::string name) {
  if (m_config == nullptr || name.empty()) {
    return;
  }
  (void)m_config->setOverride({"control_center", "ai_usage", "ollama_model"}, name);
  m_snapshot.ollama.selected = name;
  m_snapshot.ollama.wantLoaded = m_ollamaWantLoad;
  if (!m_snapshot.ollama.local) {
    m_ollamaWantLoad = false;
    m_snapshot.ollama.wantLoaded = false;
    notifyChanged();
    return;
  }
  if (m_snapshot.ollama.status == noctalia::ai_usage::OllamaStatus::Running) {
    m_ollamaWantLoad = true;
    m_snapshot.ollama.wantLoaded = true;
    notifyChanged();
    ensureSelectedOllamaModel();
    return;
  }
  notifyChanged();
}

void AiUsageService::setOllamaModelLoaded(bool loaded) {
  if (!m_snapshot.ollama.local) {
    return;
  }
  if (loaded) {
    if (m_snapshot.ollama.selected.empty() && m_config != nullptr) {
      m_snapshot.ollama.selected = m_config->config().controlCenter.aiUsage.ollamaModel;
    }
    if (m_snapshot.ollama.selected.empty()) {
      return;
    }
    m_ollamaWantLoad = true;
    m_snapshot.ollama.wantLoaded = true;
    notifyChanged();
    if (m_snapshot.ollama.status == noctalia::ai_usage::OllamaStatus::Running) {
      ensureSelectedOllamaModel();
      return;
    }
    if (m_snapshot.ollama.hasCli && m_snapshot.ollama.status == noctalia::ai_usage::OllamaStatus::Stopped
        && !m_snapshot.ollama.busy) {
      startOllama();
    }
    return;
  }
  m_ollamaWantLoad = false;
  m_snapshot.ollama.wantLoaded = false;
  unloadLoadedOllamaModels();
}

void AiUsageService::unloadLoadedOllamaModels() {
  if (m_http == nullptr || m_ollamaLoadPending || m_snapshot.ollama.busy || !m_snapshot.ollama.local) {
    notifyChanged();
    return;
  }
  if (m_snapshot.ollama.status != noctalia::ai_usage::OllamaStatus::Running) {
    notifyChanged();
    return;
  }
  std::vector<std::string> loaded;
  for (const auto& model : m_snapshot.ollama.models) {
    if (model.loaded) {
      loaded.push_back(model.name);
    }
  }
  if (loaded.empty()) {
    notifyChanged();
    return;
  }
  m_ollamaLoadPending = true;
  m_snapshot.ollama.loading = loaded.front();
  notifyChanged();
  auto remaining = std::make_shared<std::size_t>(loaded.size());
  for (const auto& model : loaded) {
    postOllamaKeepAlive(model, 0, [this, remaining](bool) {
      if (--*remaining == 0) {
        finishOllamaLoad(true);
      }
    });
  }
}

void AiUsageService::ensureSelectedOllamaModel() {
  if (m_http == nullptr || m_ollamaLoadPending || m_snapshot.ollama.busy || !m_snapshot.ollama.local) {
    return;
  }
  if (m_snapshot.ollama.status != noctalia::ai_usage::OllamaStatus::Running) {
    return;
  }
  std::string wanted = m_snapshot.ollama.selected;
  if (wanted.empty() && m_config != nullptr) {
    wanted = m_config->config().controlCenter.aiUsage.ollamaModel;
    m_snapshot.ollama.selected = wanted;
  }
  if (wanted.empty()) {
    m_ollamaWantLoad = false;
    return;
  }
  bool installed = false;
  bool loaded = false;
  std::vector<std::string> extra;
  for (const auto& model : m_snapshot.ollama.models) {
    if (model.name == wanted) {
      installed = true;
      loaded = model.loaded;
    } else if (model.loaded) {
      extra.push_back(model.name);
    }
  }
  if (!installed) {
    m_ollamaWantLoad = false;
    return;
  }
  if (loaded && extra.empty()) {
    m_ollamaWantLoad = false;
    return;
  }
  m_ollamaLoadPending = true;
  m_snapshot.ollama.loading = wanted;
  notifyChanged();
  postOllamaKeepAlive(wanted, -1, [this, extra = std::move(extra)](bool ok) {
    if (!ok) {
      finishOllamaLoad(false);
      return;
    }
    if (extra.empty()) {
      finishOllamaLoad(true);
      return;
    }
    auto remaining = std::make_shared<std::size_t>(extra.size());
    for (const auto& model : extra) {
      postOllamaKeepAlive(model, 0, [this, remaining](bool) {
        if (--*remaining == 0) {
          finishOllamaLoad(true);
        }
      });
    }
  });
}

void AiUsageService::postOllamaKeepAlive(std::string model, int keepAlive, std::function<void(bool)> done) {
  if (m_http == nullptr) {
    if (done) {
      done(false);
    }
    return;
  }
  HttpRequest req;
  req.method = "POST";
  req.url = ollamaBaseUrl() + "/api/generate";
  req.headers = {"Content-Type: application/json"};
  req.body = noctalia::ai_usage::ollamaKeepAliveJson(model, keepAlive);
  req.allowWhenOffline = ollamaHostIsLocal();
  req.freshConnection = true;
  req.timeoutSeconds = keepAlive < 0 ? 120 : 15;
  m_http->request(std::move(req), [done = std::move(done)](HttpResponse res) {
    if (done) {
      done(res.transportOk && res.status >= 200 && res.status < 300);
    }
  });
}

void AiUsageService::finishOllamaLoad(bool ok) {
  const std::string finished = m_snapshot.ollama.loading;
  m_ollamaLoadPending = false;
  m_snapshot.ollama.loading.clear();
  if (!ok && m_snapshot.ollama.selected == finished) {
    m_ollamaWantLoad = false;
  }
  notifyChanged();
  refresh(true);
}

void AiUsageService::searchOllamaLibrary(std::string query) {
  query = StringUtils::trim(query);
  if (m_snapshot.library.searching && m_snapshot.library.query == query) {
    return;
  }
  m_snapshot.library.query = query;
  m_snapshot.library.searching = true;
  m_snapshot.library.error.clear();
  notifyChanged();
  if (m_http == nullptr) {
    m_snapshot.library.searching = false;
    m_snapshot.library.error = "offline";
    notifyChanged();
    return;
  }
  HttpRequest req;
  req.url = noctalia::ai_usage::ollamaLibrarySearchUrl(query);
  req.followRedirects = true;
  req.timeoutSeconds = 20;
  const auto generation = ++m_libraryGeneration;
  m_http->request(std::move(req), [this, generation, query](HttpResponse res) {
    if (generation != m_libraryGeneration) {
      return;
    }
    m_snapshot.library.searching = false;
    if (!res.transportOk || res.status < 200 || res.status >= 300) {
      m_snapshot.library.error = "http";
      notifyChanged();
      return;
    }
    auto hits = noctalia::ai_usage::parseOllamaLibraryHtml(res.body);
    if (noctalia::ai_usage::ollamaModelNameValid(query)) {
      bool found = false;
      for (const auto& hit : hits) {
        if (hit.name == query) {
          found = true;
          break;
        }
      }
      if (!found) {
        noctalia::ai_usage::LibraryHit typed;
        typed.name = query;
        hits.insert(hits.begin(), std::move(typed));
      }
    }
    if (hits.empty() && query.empty()) {
      m_snapshot.library.error = "parse";
    }
    m_snapshot.library.hits = std::move(hits);
    notifyChanged();
  });
}

void AiUsageService::pullOllamaModel(std::string name) {
  name = StringUtils::trim(name);
  if (!noctalia::ai_usage::ollamaModelNameValid(name) || !m_snapshot.ollama.local) {
    return;
  }
  if (const std::string_view installed = noctalia::ai_usage::ollamaInstalledName(m_snapshot.ollama, name);
      !installed.empty()) {
    rememberPulledOllamaModel(std::string(installed));
    return;
  }
  if (m_snapshot.library.pull.has_value() && m_snapshot.library.pull->name == name && m_pullStream != 0) {
    return;
  }
  if (m_http != nullptr && m_pullStream != 0) {
    m_http->cancelStream(m_pullStream);
    m_pullStream = 0;
  }
  ++m_pullGeneration;
  m_pullBuf.clear();
  noctalia::ai_usage::PullProgress progress;
  progress.name = name;
  progress.status = "pulling";
  m_snapshot.library.pull = std::move(progress);
  m_snapshot.library.error.clear();
  if (m_snapshot.ollama.status != noctalia::ai_usage::OllamaStatus::Running) {
    m_pendingPull = name;
    notifyChanged();
    if (!m_snapshot.ollama.busy && m_snapshot.ollama.hasCli) {
      startOllama();
    }
    return;
  }
  if (m_http == nullptr) {
    finishOllamaPull(false, "offline");
    return;
  }
  notifyChanged();
  HttpRequest req;
  req.method = "POST";
  req.url = ollamaBaseUrl() + "/api/pull";
  req.headers = {"Content-Type: application/json"};
  req.body = noctalia::ai_usage::ollamaPullJson(name);
  req.allowWhenOffline = true;
  req.freshConnection = true;
  const auto generation = m_pullGeneration;
  m_pullStream = m_http->startStream(
      std::move(req),
      [this, generation](std::string_view chunk) {
        if (generation != m_pullGeneration) {
          return;
        }
        ingestOllamaPullChunk(chunk);
      },
      [this, generation](HttpStreamResult result) {
        if (generation != m_pullGeneration) {
          return;
        }
        m_pullStream = 0;
        if (!m_pullBuf.empty()) {
          ingestOllamaPullChunk("\n");
        }
        const bool ok = result.transportOk && result.status >= 200 && result.status < 300
            && m_snapshot.library.pull.has_value() && m_snapshot.library.pull->done
            && !m_snapshot.library.pull->error;
        finishOllamaPull(ok, ok ? std::string{} : "pull failed");
      }
  );
}

void AiUsageService::ingestOllamaPullChunk(std::string_view chunk) {
  m_pullBuf.append(chunk);
  while (true) {
    const auto pos = m_pullBuf.find('\n');
    if (pos == std::string::npos) {
      break;
    }
    const std::string line = m_pullBuf.substr(0, pos);
    m_pullBuf.erase(0, pos + 1);
    if (line.empty()) {
      continue;
    }
    auto progress = noctalia::ai_usage::parseOllamaPullLine(line);
    if (!progress.has_value() || !m_snapshot.library.pull.has_value()) {
      continue;
    }
    progress->name = m_snapshot.library.pull->name;
    m_snapshot.library.pull = std::move(*progress);
    notifyChanged();
    if (m_snapshot.library.pull->error) {
      finishOllamaPull(false, m_snapshot.library.pull->status);
      return;
    }
  }
}

void AiUsageService::rememberPulledOllamaModel(std::string name) {
  if (m_config == nullptr || name.empty()) {
    return;
  }
  (void)m_config->setOverride({"control_center", "ai_usage", "ollama_model"}, name);
  m_snapshot.ollama.selected = std::move(name);
  notifyChanged();
}

void AiUsageService::finishOllamaPull(bool ok, std::string error) {
  ++m_pullGeneration;
  if (m_http != nullptr && m_pullStream != 0) {
    m_http->cancelStream(m_pullStream);
    m_pullStream = 0;
  }
  m_pullBuf.clear();
  const std::string name = m_snapshot.library.pull.has_value() ? m_snapshot.library.pull->name : std::string{};
  if (ok) {
    m_snapshot.library.pull.reset();
    m_snapshot.library.error.clear();
    if (!name.empty()) {
      rememberPulledOllamaModel(name);
    } else {
      notifyChanged();
    }
    refresh(true);
    return;
  }
  if (m_snapshot.library.pull.has_value()) {
    m_snapshot.library.pull->error = true;
    if (!error.empty()) {
      m_snapshot.library.pull->status = std::move(error);
    }
  } else if (!error.empty()) {
    m_snapshot.library.error = std::move(error);
  }
  notifyChanged();
}

void AiUsageService::cancelOllamaPull() {
  m_pendingPull.clear();
  if (!m_snapshot.library.pull.has_value() && m_pullStream == 0) {
    return;
  }
  finishOllamaPull(false, "cancelled");
  m_snapshot.library.pull.reset();
  m_snapshot.library.error.clear();
  notifyChanged();
}

void AiUsageService::deleteOllamaModel(std::string name) {
  name = StringUtils::trim(name);
  if (!noctalia::ai_usage::ollamaModelNameValid(name) || !m_snapshot.ollama.local) {
    return;
  }
  if (const std::string_view installed = noctalia::ai_usage::ollamaInstalledName(m_snapshot.ollama, name);
      !installed.empty()) {
    name = std::string(installed);
  }
  if (m_snapshot.library.pull.has_value()) {
    const auto& pullName = m_snapshot.library.pull->name;
    if (pullName == name || noctalia::ai_usage::ollamaInstalledName(m_snapshot.ollama, pullName) == name) {
      cancelOllamaPull();
    }
  }
  if (m_config != nullptr) {
    const std::string selected = m_config->config().controlCenter.aiUsage.ollamaModel;
    if (selected == name
        || noctalia::ai_usage::ollamaInstalledName(m_snapshot.ollama, selected) == name) {
      (void)m_config->setOverride({"control_center", "ai_usage", "ollama_model"}, std::string{});
      m_snapshot.ollama.selected.clear();
      m_ollamaWantLoad = false;
      m_snapshot.ollama.wantLoaded = false;
    }
  }
  if (!m_snapshot.ollama.deleting.empty() && m_snapshot.ollama.deleting == name) {
    return;
  }
  m_snapshot.ollama.deleting = name;
  if (m_snapshot.ollama.status != noctalia::ai_usage::OllamaStatus::Running) {
    m_pendingDelete = name;
    notifyChanged();
    if (!m_snapshot.ollama.busy && m_snapshot.ollama.hasCli) {
      startOllama();
    }
    return;
  }
  postOllamaDelete(std::move(name));
}

void AiUsageService::postOllamaDelete(std::string name) {
  if (m_http == nullptr) {
    finishOllamaDelete(false);
    return;
  }
  notifyChanged();
  HttpRequest req;
  req.method = "DELETE";
  req.url = ollamaBaseUrl() + "/api/delete";
  req.headers = {"Content-Type: application/json"};
  req.body = noctalia::ai_usage::ollamaDeleteJson(name);
  req.allowWhenOffline = true;
  req.freshConnection = true;
  req.timeoutSeconds = 60;
  m_http->request(std::move(req), [this, name = std::move(name)](HttpResponse res) {
    if (m_snapshot.ollama.deleting != name) {
      return;
    }
    finishOllamaDelete(res.transportOk && res.status >= 200 && res.status < 300);
  });
}

void AiUsageService::finishOllamaDelete(bool ok) {
  const std::string name = m_snapshot.ollama.deleting;
  m_snapshot.ollama.deleting.clear();
  m_pendingDelete.clear();
  if (ok && !name.empty()) {
    std::erase_if(m_snapshot.ollama.models, [&](const noctalia::ai_usage::OllamaModel& model) {
      return model.name == name;
    });
  }
  notifyChanged();
  refresh(true);
}
