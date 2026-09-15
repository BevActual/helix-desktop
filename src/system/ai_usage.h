#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
  };

  struct Window {
    WindowKey key = WindowKey::Session;
    float percent = 0.0F;
    std::optional<std::chrono::system_clock::time_point> reset;
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
  };

  struct OllamaSnapshot {
    OllamaStatus status = OllamaStatus::Absent;
    bool hasCli = false;
    bool local = true;
    bool busy = false;
    std::string version;
    std::vector<OllamaModel> models;
  };

  struct Snapshot {
    std::vector<ProviderSnapshot> providers;
    OllamaSnapshot ollama;
    bool refreshing = false;
  };

  struct ClaudeCredentials {
    std::string accessToken;
    std::int64_t expiresAtMs = 0;
    std::string plan;
  };

  [[nodiscard]] std::string_view providerKey(ProviderId id) noexcept;
  [[nodiscard]] std::string_view providerGlyph(ProviderId id) noexcept;
  [[nodiscard]] std::span<const std::string_view> providerDesktopIds(ProviderId id) noexcept;
  [[nodiscard]] std::string_view providerCli(ProviderId id) noexcept;
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
  [[nodiscard]] std::optional<std::string> jwtSubject(std::string_view token);
  [[nodiscard]] std::string cursorUserId(std::string_view subject);
  [[nodiscard]] int countPromptEvents(std::string_view raw, bool jsonl, std::string_view todayIso);

} // namespace noctalia::ai_usage
