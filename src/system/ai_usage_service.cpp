#include "system/ai_usage_service.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "net/http_client.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"
#include "system/terminal_launch.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
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

  [[nodiscard]] bool pathExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
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

} // namespace

AiUsageService::AiUsageService(ConfigService& config, HttpClient& http) : m_config(&config), m_http(&http) {
  m_snapshot.providers.resize(noctalia::ai_usage::kProviderCount);
  for (std::size_t i = 0; i < noctalia::ai_usage::kProviderCount; ++i) {
    m_snapshot.providers[i].id = static_cast<noctalia::ai_usage::ProviderId>(i);
  }
}

AiUsageService::~AiUsageService() { m_timer.stop(); }

void AiUsageService::start() {
  if (m_started || m_config == nullptr) {
    return;
  }
  m_started = true;
  m_config->addReloadCallback([this]() {
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

noctalia::ai_usage::ProviderSnapshot& AiUsageService::slot(noctalia::ai_usage::ProviderId id) {
  return m_snapshot.providers[static_cast<std::size_t>(id)];
}

void AiUsageService::notifyChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void AiUsageService::publish(noctalia::ai_usage::ProviderSnapshot snap) {
  snap.launchable = canLaunch(snap.id);
  snap.fetchedAt = std::chrono::system_clock::now();
  slot(snap.id) = std::move(snap);
  m_snapshot.refreshing = false;
  notifyChanged();
}

void AiUsageService::publishOllama(noctalia::ai_usage::OllamaSnapshot snap) {
  snap.busy = m_snapshot.ollama.busy;
  m_snapshot.ollama = std::move(snap);
  m_snapshot.refreshing = false;
  notifyChanged();
}

void AiUsageService::publishError(noctalia::ai_usage::ProviderId id, noctalia::ai_usage::Status status) {
  auto& current = slot(id);
  if (current.status == noctalia::ai_usage::Status::Ok) {
    current.stale = true;
    current.fetchedAt = std::chrono::system_clock::now();
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
    if (mode == Mode::Auto && !detect() && !canLaunch(id)) {
      noctalia::ai_usage::ProviderSnapshot snap;
      snap.id = id;
      snap.status = noctalia::ai_usage::Status::Absent;
      publish(std::move(snap));
      return;
    }
    fetch();
  };

  const auto claudeCreds = FileUtils::expandUserPath("~/.claude/.credentials.json");
  const auto codexDir = FileUtils::expandUserPath("~/.codex/sessions");
  const auto geminiDir = FileUtils::expandUserPath("~/.gemini");
  const auto qwenDir = FileUtils::expandUserPath("~/.qwen");
  const std::filesystem::path copilotDir{FileUtils::expandXdgBaseDir("$XDG_CONFIG_HOME/github-copilot")};
  const std::filesystem::path cursorDb{FileUtils::expandXdgBaseDir("$XDG_CONFIG_HOME/Cursor/User/globalStorage/state.vscdb")};

  run(
      noctalia::ai_usage::ProviderId::Claude, [&]() { return pathExists(claudeCreds); },
      [&]() { fetchClaude(generation, force); }
  );
  run(
      noctalia::ai_usage::ProviderId::Codex, [&]() { return pathExists(codexDir); },
      [&]() { fetchCodex(generation); }
  );
  run(
      noctalia::ai_usage::ProviderId::Gemini, [&]() { return pathExists(geminiDir); },
      [&]() { fetchGemini(generation); }
  );
  run(
      noctalia::ai_usage::ProviderId::Qwen, [&]() { return pathExists(qwenDir); },
      [&]() { fetchQwen(generation); }
  );
  run(
      noctalia::ai_usage::ProviderId::Copilot,
      [&]() { return pathExists(copilotDir) && process::commandExists("gh"); },
      [&]() { fetchCopilot(generation); }
  );
  run(
      noctalia::ai_usage::ProviderId::Cursor,
      [&]() { return pathExists(cursorDb) && process::commandExists("sqlite3"); },
      [&]() { fetchCursor(generation); }
  );
  run(
      noctalia::ai_usage::ProviderId::DeepSeek, [&]() { return !deepseekApiKey().empty(); },
      [&]() { fetchDeepseek(generation); }
  );
  fetchOllama(generation);
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
    const auto scanned = scanCodexSessions(dir);
    DeferredCall::callLater([this, generation, scanned]() {
      if (generation != m_generation) {
        return;
      }
      if (!scanned.ok) {
        publishError(noctalia::ai_usage::ProviderId::Codex, noctalia::ai_usage::Status::NoData);
        return;
      }
      const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
      auto parsed = noctalia::ai_usage::parseCodexRateLimitsJson(scanned.line, static_cast<std::int64_t>(now));
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

bool AiUsageService::canLaunch(noctalia::ai_usage::ProviderId id) const {
  const auto entries = desktopEntries();
  for (const auto desktopId : noctalia::ai_usage::providerDesktopIds(id)) {
    if (app_identity::findDesktopEntry(desktopId, entries).has_value()) {
      return true;
    }
  }
  const auto cli = noctalia::ai_usage::providerCli(id);
  return !cli.empty() && process::commandExists(std::string(cli).c_str());
}

void AiUsageService::launchProvider(noctalia::ai_usage::ProviderId id) {
  const auto entries = desktopEntries();
  for (const auto desktopId : noctalia::ai_usage::providerDesktopIds(id)) {
    if (auto entry = app_identity::findDesktopEntry(desktopId, entries); entry.has_value()) {
      desktop_entry_launch::LaunchOptions options;
      options.dbusActivatable = entry->dbusActivatable;
      options.dbusAppId = entry->id;
      if (desktop_entry_launch::launchEntry(*entry, options)) {
        return;
      }
    }
  }
  const auto cli = noctalia::ai_usage::providerCli(id);
  if (cli.empty()) {
    return;
  }
  const std::string command(cli);
  if (noctalia::ai_usage::providerOpensInTerminal(id)) {
    (void)terminal_launch::launch(command);
    return;
  }
  if (process::commandExists(command.c_str())) {
    (void)process::launchDetachedTracked(std::vector<std::string>{command});
  }
}

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
