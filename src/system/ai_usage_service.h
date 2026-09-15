#pragma once

#include "core/timer_manager.h"
#include "system/ai_usage.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

class ConfigService;
class HttpClient;

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
  void startOllama();
  void stopOllama();

  [[nodiscard]] const noctalia::ai_usage::Snapshot& snapshot() const noexcept { return m_snapshot; }

  void setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

private:
  void scheduleTimer();
  void fetchAll(bool force);
  void publish(noctalia::ai_usage::ProviderSnapshot snap);
  void publishError(noctalia::ai_usage::ProviderId id, noctalia::ai_usage::Status status);
  void publishOllama(noctalia::ai_usage::OllamaSnapshot snap);
  void notifyChanged();
  void fetchClaude(std::uint64_t generation, bool force);
  void fetchCodex(std::uint64_t generation);
  void fetchGemini(std::uint64_t generation);
  void fetchQwen(std::uint64_t generation);
  void fetchPromptCounter(noctalia::ai_usage::ProviderId id, std::uint64_t generation, std::string dir, int limit);
  void fetchCopilot(std::uint64_t generation);
  void fetchCursor(std::uint64_t generation);
  void fetchDeepseek(std::uint64_t generation);
  void fetchOllama(std::uint64_t generation);
  void spawnOllamaServe();
  [[nodiscard]] noctalia::ai_usage::ProviderSnapshot& slot(noctalia::ai_usage::ProviderId id);
  [[nodiscard]] bool canLaunch(noctalia::ai_usage::ProviderId id) const;
  [[nodiscard]] std::string ollamaBaseUrl() const;
  [[nodiscard]] bool ollamaHostIsLocal() const;

  ConfigService* m_config = nullptr;
  HttpClient* m_http = nullptr;
  ChangeCallback m_changeCallback;
  Timer m_timer;
  noctalia::ai_usage::Snapshot m_snapshot;
  std::uint64_t m_generation = 0;
  std::chrono::steady_clock::time_point m_claudeLastProbe{};
  std::optional<int> m_ollamaServePid;
  bool m_started = false;
};
