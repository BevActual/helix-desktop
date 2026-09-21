#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace noctalia::ai_usage {

  enum class ProviderId : std::uint8_t {
    Claude = 0,
    Codex,
    Gemini,
    Qwen,
    Copilot,
    Cursor,
    DeepSeek,
  };

  inline constexpr std::size_t kProviderCount = 7;

  enum class Status : std::uint8_t {
    Ok = 0,
    Absent,
    Disabled,
    NoCredentials,
    NoAuth,
    Expired,
    NoData,
    HttpError,
    Unsupported,
  };

  enum class WindowKey : std::uint8_t {
    Session = 0,
    Weekly,
    Day,
    Premium,
    Chat,
    Completions,
    Opus,
    Sonnet,
    Extra,
  };

  struct Window {
    WindowKey key = WindowKey::Session;
    float percent = 0.0F;
    std::optional<std::chrono::system_clock::time_point> reset;
    std::optional<double> used;
    std::optional<double> max;
  };

  struct ProviderSnapshot {
    ProviderId id = ProviderId::Claude;
    Status status = Status::Absent;
    bool stale = false;
    bool launchable = false;
    std::string plan;
    std::vector<Window> windows;
    std::optional<std::chrono::system_clock::time_point> dataAt;
    std::chrono::system_clock::time_point fetchedAt{};
  };

  enum class OllamaStatus : std::uint8_t {
    Absent = 0,
    Disabled,
    Stopped,
    Running,
    Error,
  };

  struct OllamaModel {
    std::string name;
    std::uint64_t size = 0;
    bool loaded = false;
    std::uint64_t vram = 0;
    std::string parameterSize;
    std::string quantization;
  };

  struct OllamaSnapshot {
    OllamaStatus status = OllamaStatus::Absent;
    bool hasCli = false;
    bool local = true;
    bool busy = false;
    std::string version;
    std::string selected;
    std::string loading;
    std::string deleting;
    bool wantLoaded = false;
    std::vector<OllamaModel> models;
  };

  struct LibraryHit {
    std::string name;
    std::string description;
    std::string pulls;
    std::string sizes;
  };

  struct PullProgress {
    std::string name;
    std::string status;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
    bool error = false;
    bool done = false;
  };

  struct LibrarySnapshot {
    std::string query;
    std::vector<LibraryHit> hits;
    bool searching = false;
    std::string error;
    std::optional<PullProgress> pull;
  };

  enum class LocalRuntimeKind : std::uint8_t {
    LmStudio = 0,
    LlamaCpp,
    Vllm,
    Jan,
  };

  inline constexpr std::size_t kLocalRuntimeCount = 4;

  struct LocalRuntime {
    LocalRuntimeKind kind = LocalRuntimeKind::LmStudio;
    bool launchable = false;
    std::uint16_t port = 0;
    std::vector<std::string> models;
  };

  struct Snapshot {
    std::vector<ProviderSnapshot> providers;
    OllamaSnapshot ollama;
    std::vector<LocalRuntime> localRuntimes;
    LibrarySnapshot library;
    bool refreshing = false;
    std::chrono::system_clock::time_point fetchedAt{};
  };

  struct ClaudeCredentials {
    std::string accessToken;
    std::int64_t expiresAtMs = 0;
    std::string plan;
  };

  struct QuotaHit {
    ProviderId provider = ProviderId::Claude;
    Window window;
  };

  enum class QuotaWarningKind : std::uint8_t { High, ResetSoon };

  struct QuotaWarning {
    ProviderId provider = ProviderId::Claude;
    Window window;
    QuotaWarningKind kind = QuotaWarningKind::High;
  };

  struct QuotaWarningState {
    bool high = false;
    bool reset = false;
  };

  [[nodiscard]] std::string_view providerKey(ProviderId id) noexcept;
  [[nodiscard]] std::string_view windowKeyId(WindowKey key) noexcept;
  [[nodiscard]] std::string_view providerGlyph(ProviderId id) noexcept;
  [[nodiscard]] std::string providerLabel(ProviderId id);
  [[nodiscard]] std::string windowLabel(WindowKey key);
  [[nodiscard]] std::string ollamaStatusLabel(OllamaStatus status);
  [[nodiscard]] std::string ollamaGlanceLabel(const OllamaSnapshot& ollama);
  [[nodiscard]] const PullProgress* activeOllamaPull(const Snapshot& snapshot) noexcept;
  [[nodiscard]] const PullProgress* failedOllamaPull(const Snapshot& snapshot) noexcept;
  [[nodiscard]] std::string snapshotGlanceLabel(const Snapshot& snapshot);
  [[nodiscard]] std::string_view effectiveOllamaModel(const OllamaSnapshot& ollama) noexcept;
  [[nodiscard]] bool ollamaModelIsLoaded(const OllamaSnapshot& ollama, std::string_view name) noexcept;
  [[nodiscard]] std::uint64_t ollamaDiskBytes(const OllamaSnapshot& ollama) noexcept;
  [[nodiscard]] std::uint64_t ollamaVramBytes(const OllamaSnapshot& ollama) noexcept;
  [[nodiscard]] bool ollamaModelMatchesQuery(const OllamaModel& model, std::string_view query) noexcept;
  void sortOllamaModels(std::vector<OllamaModel>& models, std::string_view selected);
  [[nodiscard]] std::string_view ollamaStatusKey(OllamaStatus status) noexcept;
  [[nodiscard]] std::string_view localRuntimeKey(LocalRuntimeKind kind) noexcept;
  [[nodiscard]] std::string localRuntimeLabel(LocalRuntimeKind kind);
  [[nodiscard]] std::string_view localRuntimeGlyph(LocalRuntimeKind kind) noexcept;
  [[nodiscard]] std::span<const std::string_view> localRuntimeDesktopIds(LocalRuntimeKind kind) noexcept;
  [[nodiscard]] std::uint16_t localRuntimePort(LocalRuntimeKind kind) noexcept;
  [[nodiscard]] std::string_view statusKey(Status status) noexcept;
  [[nodiscard]] std::string formatSnapshotDump(const Snapshot& snapshot);
  [[nodiscard]] bool providerVisible(Status status) noexcept;
  [[nodiscard]] bool ollamaVisible(OllamaStatus status) noexcept;
  [[nodiscard]] bool localInferenceVisible(const Snapshot& snapshot) noexcept;
  [[nodiscard]] bool glanceableOllama(const Snapshot& snapshot) noexcept;
  [[nodiscard]] const LocalRuntime* glanceableLocalRuntime(const Snapshot& snapshot) noexcept;
  [[nodiscard]] const Window* hottestWindow(const ProviderSnapshot& provider) noexcept;
  [[nodiscard]] std::optional<QuotaHit> hottestQuota(const Snapshot& snapshot);
  [[nodiscard]] std::string quotaWarningKey(ProviderId id, WindowKey key);
  [[nodiscard]] std::vector<QuotaWarning> takeQuotaWarnings(
      const Snapshot& snapshot, int warnPercent, int warnResetMinutes,
      std::unordered_map<std::string, QuotaWarningState>& state, std::chrono::system_clock::time_point now
  );
  [[nodiscard]] std::span<const std::string_view> providerDesktopIds(ProviderId id) noexcept;
  [[nodiscard]] std::span<const std::string_view> ollamaDesktopIds() noexcept;
  [[nodiscard]] std::string_view providerCli(ProviderId id) noexcept;
  [[nodiscard]] bool providerIsCli(ProviderId id) noexcept;
  [[nodiscard]] bool localRuntimeIsCli(LocalRuntimeKind kind) noexcept;
  [[nodiscard]] bool providerOpensInTerminal(ProviderId id) noexcept;
  [[nodiscard]] std::optional<std::chrono::system_clock::time_point> parseIsoUtc(std::string_view iso);
  [[nodiscard]] float clampPercent(double value) noexcept;
  [[nodiscard]] std::string formatByteSize(std::uint64_t bytes);
  [[nodiscard]] std::optional<ClaudeCredentials> parseClaudeCredentialsJson(std::string_view json);
  [[nodiscard]] std::optional<ProviderSnapshot> parseClaudeUsageJson(std::string_view json, std::string plan);
  [[nodiscard]] std::optional<ProviderSnapshot> parseCodexRateLimitsJson(std::string_view json, std::int64_t nowEpoch);
  [[nodiscard]] std::optional<ProviderSnapshot> parseCopilotUsageJson(std::string_view json);
  [[nodiscard]] std::optional<ProviderSnapshot> parseCursorUsageJson(std::string_view json);
  [[nodiscard]] std::optional<ProviderSnapshot> parseDeepseekBalanceJson(std::string_view json);
  [[nodiscard]] std::optional<OllamaSnapshot>
  parseOllamaSnapshot(std::string_view tagsJson, std::string_view psJson, std::string_view versionJson);
  [[nodiscard]] std::optional<std::vector<std::string>> parseOpenAiModelsJson(std::string_view json);
  [[nodiscard]] std::string ollamaKeepAliveJson(std::string_view model, int keepAlive);
  [[nodiscard]] bool ollamaModelNameValid(std::string_view name) noexcept;
  [[nodiscard]] bool ollamaModelInstalled(const OllamaSnapshot& ollama, std::string_view name) noexcept;
  [[nodiscard]] std::string_view ollamaInstalledName(const OllamaSnapshot& ollama, std::string_view name) noexcept;
  [[nodiscard]] std::string ollamaLibrarySearchUrl(std::string_view query);
  [[nodiscard]] std::vector<LibraryHit> parseOllamaLibraryHtml(std::string_view html);
  [[nodiscard]] std::optional<PullProgress> parseOllamaPullLine(std::string_view json);
  [[nodiscard]] std::string ollamaPullJson(std::string_view model);
  [[nodiscard]] std::string ollamaDeleteJson(std::string_view model);
  [[nodiscard]] std::string formatOllamaPullProgress(const PullProgress& pull);
  [[nodiscard]] float ollamaPullFraction(const PullProgress& pull) noexcept;
  [[nodiscard]] std::optional<std::string> jwtSubject(std::string_view token);
  [[nodiscard]] std::string cursorUserId(std::string_view subject);
  [[nodiscard]] int countPromptEvents(std::string_view raw, bool jsonl, std::string_view todayIso);

} // namespace noctalia::ai_usage
