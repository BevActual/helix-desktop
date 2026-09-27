#pragma once

#include "core/timer_manager.h"
#include "system/ai_usage.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class ConfigService;
class HttpClient;
class IpcService;

class AiUsageService {
public:
  using ChangeCallback = std::function<void()>;

  AiUsageService(ConfigService& config, HttpClient& http);
  ~AiUsageService();

  AiUsageService(const AiUsageService&) = delete;
  AiUsageService& operator=(const AiUsageService&) = delete;

  void start();
  void refresh(bool force = false);
  void launchProvider(noctalia::ai_usage::ProviderId id);
  void launchOllama();
  void launchLocalRuntime(noctalia::ai_usage::LocalRuntimeKind kind);
  void startOllama();
  void stopOllama();
  void selectOllamaModel(std::string name);
  void setOllamaModelLoaded(bool loaded);
  void searchOllamaLibrary(std::string query);
  void pullOllamaModel(std::string name);
  void cancelOllamaPull();
  void deleteOllamaModel(std::string name);
  [[nodiscard]] bool canLaunchOllama() const;
  [[nodiscard]] bool canLaunchLocalRuntime(noctalia::ai_usage::LocalRuntimeKind kind) const;

  // LocalAI: the desktop's AI service layer.
  void startLocalAi();
  void stopLocalAi();
  void launchLocalAi(); // WebUI in the browser (or the desktop app when installed)
  void selectLocalAiModel(std::string name);
  void loadLocalAiGallery(bool force);
  void setLocalAiGalleryQuery(std::string query);
  void installLocalAiModel(std::string galleryId);
  void deleteLocalAiModel(std::string name);
  void dismissLocalAiJob();
  [[nodiscard]] bool canLaunchLocalAi() const;
  [[nodiscard]] std::string localAiBaseUrl() const;

  [[nodiscard]] const noctalia::ai_usage::Snapshot& snapshot() const noexcept { return m_snapshot; }
  [[nodiscard]] int attentionPercent() const;

  void addChangeCallback(ChangeCallback callback);
  void registerIpc(IpcService& ipc);

private:
  void scheduleTimer();
  void fetchAll(bool force);
  void publish(noctalia::ai_usage::ProviderSnapshot snap);
  void publishError(noctalia::ai_usage::ProviderId id, noctalia::ai_usage::Status status);
  void publishOllama(noctalia::ai_usage::OllamaSnapshot snap);
  void publishLocalRuntimes(std::vector<noctalia::ai_usage::LocalRuntime> runtimes);
  void notifyChanged();
  void emitQuotaWarnings();
  void fetchClaude(std::uint64_t generation, bool force);
  void fetchCodex(std::uint64_t generation);
  void fetchGemini(std::uint64_t generation);
  void fetchQwen(std::uint64_t generation);
  void fetchPromptCounter(noctalia::ai_usage::ProviderId id, std::uint64_t generation, std::string dir, int limit);
  void fetchCopilot(std::uint64_t generation);
  void fetchCursor(std::uint64_t generation);
  void fetchDeepseek(std::uint64_t generation);
  void fetchOllama(std::uint64_t generation);
  void fetchLocalRuntimes(std::uint64_t generation, bool force);
  void spawnOllamaServe();
  void ensureSelectedOllamaModel();
  void unloadLoadedOllamaModels();
  void postOllamaKeepAlive(std::string model, int keepAlive, std::function<void(bool)> done);
  void finishOllamaLoad(bool ok);
  void finishOllamaPull(bool ok, std::string error);
  void ingestOllamaPullChunk(std::string_view chunk);
  void rememberPulledOllamaModel(std::string name);
  void finishOllamaDelete(bool ok);
  void postOllamaDelete(std::string name);
  [[nodiscard]] noctalia::ai_usage::ProviderSnapshot& slot(noctalia::ai_usage::ProviderId id);
  [[nodiscard]] bool canLaunch(noctalia::ai_usage::ProviderId id) const;
  [[nodiscard]] bool canLaunchDesktop(std::span<const std::string_view> ids) const;
  [[nodiscard]] bool launchDesktopIds(std::span<const std::string_view> ids) const;
  [[nodiscard]] std::string ollamaBaseUrl() const;
  [[nodiscard]] bool ollamaHostIsLocal() const;
  void fetchLocalAi(std::uint64_t generation, bool force);
  void publishLocalAi(noctalia::ai_usage::LocalAiSnapshot snap);
  void dropShadowedLocalRuntimes();
  [[nodiscard]] bool localAiHostIsLocal() const;
  void spawnLocalAiRun();
  void pollLocalAiJob();
  void finishLocalAiJob(bool ok, std::string error);
  void postLocalAiApply(std::string galleryId);
  void postLocalAiDelete(std::string name);

  ConfigService* m_config = nullptr;
  HttpClient* m_http = nullptr;
  std::vector<ChangeCallback> m_changeCallbacks;
  Timer m_timer;
  noctalia::ai_usage::Snapshot m_snapshot;
  std::unordered_map<std::string, noctalia::ai_usage::QuotaWarningState> m_warningState;
  std::uint64_t m_generation = 0;
  std::chrono::steady_clock::time_point m_claudeLastProbe{};
  struct LocalRuntimeProbe {
    std::chrono::steady_clock::time_point nextProbeAt{};
    std::chrono::seconds backoff{0};
  };
  std::array<LocalRuntimeProbe, noctalia::ai_usage::kLocalRuntimeCount> m_localRuntimeProbes{};
  std::vector<noctalia::ai_usage::LocalRuntime> m_localRuntimesKnown;
  std::optional<int> m_ollamaServePid;
  LocalRuntimeProbe m_localAiProbe;
  std::optional<int> m_localAiRunPid;
  Timer m_localAiJobTimer;
  Timer m_localAiStartTimer;
  std::uint64_t m_localAiGalleryGeneration = 0;
  std::uint64_t m_localAiJobGeneration = 0;
  std::string m_pendingLocalAiInstall;
  std::string m_pendingLocalAiDelete;
  bool m_ollamaWantLoad = false;
  bool m_ollamaLoadPending = false;
  bool m_started = false;
  std::uint64_t m_libraryGeneration = 0;
  std::uint64_t m_pullStream = 0;
  std::uint64_t m_pullGeneration = 0;
  std::string m_pullBuf;
  std::string m_pendingPull;
  std::string m_pendingDelete;
};
