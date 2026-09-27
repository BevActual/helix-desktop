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

  // LocalAI (localai.io): one OpenAI-compatible endpoint that fronts chat, vision, speech,
  // transcription, image, video, and embedding backends. Helix treats it as the desktop's
  // AI service layer: the tab manages the daemon, its models and gallery, and plugins read
  // `models` (with capabilities) to pick an endpoint for a given job.
  enum class LocalAiStatus : std::uint8_t {
    Absent = 0,
    Disabled,
    Stopped,
    Running,
    Error,
  };

  enum class LocalAiCapability : std::uint8_t {
    Chat = 0,
    Vision,
    Tools,
    Thinking,
    Transcript,
    Tts,
    Embeddings,
    Image,
    Video,
    Sound,
    Vad,
    Rerank,
    Detection,
    Other,
  };

  inline constexpr std::size_t kLocalAiCapabilityCount = 14;

  struct LocalAiModel {
    std::string id;
    std::string backend;
    std::vector<LocalAiCapability> capabilities;
    std::vector<std::string> inputModalities;
    std::vector<std::string> outputModalities;
    std::int64_t contextSize = 0;
    bool loaded = false;
  };

  // One in-flight or finished gallery operation (`GET /models/jobs/{uuid}`).
  struct LocalAiJob {
    std::string uuid;
    std::string galleryId; // what was sent to /models/apply; retry re-sends this
    std::string model;
    std::string message;
    std::string phase;
    std::string fileName;
    float progress = 0.0F; // 0..100
    std::uint64_t currentBytes = 0;
    std::uint64_t totalBytes = 0;
    bool processed = false;
    bool deletion = false;
    bool cancelled = false;
    bool error = false;
  };

  struct LocalAiSnapshot {
    LocalAiStatus status = LocalAiStatus::Absent;
    bool hasCli = false;
    bool local = true;
    bool busy = false;
    std::string version;
    std::string baseUrl;
    std::string selected;
    std::string deleting;
    std::vector<std::string> backends;
    std::vector<LocalAiModel> models;
    std::optional<LocalAiJob> job;
  };

  struct LocalAiGalleryHit {
    std::string id; // "<gallery>@<name>", what `POST /models/apply` wants
    std::string name;
    std::string gallery;
    std::string description;
    std::string backend;
    std::string size;
    std::string license;
    std::vector<std::string> tags;
    bool installed = false;
  };

  struct LocalAiGallery {
    std::string query;
    std::vector<LocalAiGalleryHit> hits; // the full catalog; filter with localAiGalleryMatchesQuery
    bool loading = false;
    bool loaded = false;
    std::string error;
  };

  struct Snapshot {
    std::vector<ProviderSnapshot> providers;
    OllamaSnapshot ollama;
    LocalAiSnapshot localAi;
    LocalAiGallery localAiGallery;
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
  // LocalAI
  [[nodiscard]] std::string_view localAiStatusKey(LocalAiStatus status) noexcept;
  [[nodiscard]] std::string localAiStatusLabel(LocalAiStatus status);
  [[nodiscard]] std::string localAiGlanceLabel(const LocalAiSnapshot& localAi);
  [[nodiscard]] bool localAiVisible(LocalAiStatus status) noexcept;
  [[nodiscard]] bool glanceableLocalAi(const Snapshot& snapshot) noexcept;
  [[nodiscard]] const LocalAiJob* activeLocalAiJob(const Snapshot& snapshot) noexcept;
  [[nodiscard]] const LocalAiJob* failedLocalAiJob(const Snapshot& snapshot) noexcept;
  [[nodiscard]] std::string_view localAiCapabilityKey(LocalAiCapability capability) noexcept;
  [[nodiscard]] std::string localAiCapabilityLabel(LocalAiCapability capability);
  [[nodiscard]] std::string_view localAiCapabilityGlyph(LocalAiCapability capability) noexcept;
  [[nodiscard]] std::optional<LocalAiCapability> localAiCapabilityFromKey(std::string_view key) noexcept;
  [[nodiscard]] bool localAiModelHas(const LocalAiModel& model, LocalAiCapability capability) noexcept;
  [[nodiscard]] std::string_view effectiveLocalAiModel(const LocalAiSnapshot& localAi) noexcept;
  [[nodiscard]] const LocalAiModel* localAiModelFor(const LocalAiSnapshot& localAi, LocalAiCapability capability) noexcept;
  [[nodiscard]] bool localAiModelInstalled(const LocalAiSnapshot& localAi, std::string_view name) noexcept;
  [[nodiscard]] bool localAiModelMatchesQuery(const LocalAiModel& model, std::string_view query) noexcept;
  [[nodiscard]] bool localAiGalleryMatchesQuery(const LocalAiGalleryHit& hit, std::string_view query) noexcept;
  [[nodiscard]] std::vector<const LocalAiGalleryHit*>
  filterLocalAiGallery(const LocalAiGallery& gallery, std::string_view query, std::size_t limit);
  [[nodiscard]] bool localAiModelNameValid(std::string_view name) noexcept;
  void sortLocalAiModels(std::vector<LocalAiModel>& models, std::string_view selected);
  [[nodiscard]] std::string localAiModelSummary(const LocalAiModel& model);
  [[nodiscard]] std::string formatLocalAiJobProgress(const LocalAiJob& job);
  [[nodiscard]] float localAiJobFraction(const LocalAiJob& job) noexcept;
  [[nodiscard]] std::span<const std::string_view> localAiDesktopIds() noexcept;
  [[nodiscard]] std::string parseLocalAiWellKnownVersion(std::string_view json);
  [[nodiscard]] std::optional<std::vector<LocalAiModel>> parseLocalAiModelsJson(std::string_view json);
  void applyLocalAiSystemJson(LocalAiSnapshot& localAi, std::string_view json);
  [[nodiscard]] std::optional<std::vector<LocalAiGalleryHit>>
  parseLocalAiGalleryJson(std::string_view json, const LocalAiSnapshot& installed);
  [[nodiscard]] std::optional<LocalAiJob> parseLocalAiJobJson(std::string_view json, std::string_view uuid);
  [[nodiscard]] std::string parseLocalAiApplyUuid(std::string_view json);
  [[nodiscard]] std::string localAiApplyJson(std::string_view galleryId);

  [[nodiscard]] std::optional<std::string> jwtSubject(std::string_view token);
  [[nodiscard]] std::string cursorUserId(std::string_view subject);
  [[nodiscard]] int countPromptEvents(std::string_view raw, bool jsonl, std::string_view todayIso);

} // namespace noctalia::ai_usage
