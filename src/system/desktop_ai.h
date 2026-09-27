#pragma once

#include "system/ai_usage.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class AiUsageService;
class ClipboardService;
class HttpClient;
class IpcService;
class SoundPlayer;

// One-shot desktop actions against the LocalAI service layer: rewrite clipboard
// text, read or describe a screenshot, dictate from the mic, and read text aloud.
// Hermes stays the agent. This client only runs a single capability call.

enum class DesktopAiTask : std::uint8_t {
  None,
  Summarize,
  Translate,
  Rewrite,
  Speak,
  Describe,
  Read,
  Dictate,
};

enum class DesktopAiSurface : std::uint8_t { None, Clipboard, Capture };

struct DesktopAiState {
  bool busy = false;
  bool recording = false;
  bool failed = false;
  DesktopAiTask task = DesktopAiTask::None;
  DesktopAiSurface surface = DesktopAiSurface::None;
  std::string status;
  std::string result;
};

namespace noctalia::desktop_ai {

  [[nodiscard]] std::string chatRequestJson(std::string_view model, std::string_view system, std::string_view user);
  [[nodiscard]] std::string
  visionRequestJson(std::string_view model, std::string_view prompt, std::string_view dataUri);
  [[nodiscard]] std::string speechRequestJson(std::string_view model, std::string_view text);
  // Assistant text from a chat completion, or nullopt when the body is not one.
  [[nodiscard]] std::optional<std::string> parseChatContent(std::string_view json);
  [[nodiscard]] std::optional<std::string> parseTranscriptText(std::string_view json);
  [[nodiscard]] std::string parseErrorMessage(std::string_view json);

} // namespace noctalia::desktop_ai

class DesktopAiService {
public:
  using ChangeCallback = std::function<void()>;

  DesktopAiService(AiUsageService& usage, HttpClient& http, ClipboardService& clipboard);
  ~DesktopAiService();

  DesktopAiService(const DesktopAiService&) = delete;
  DesktopAiService& operator=(const DesktopAiService&) = delete;

  void setSoundPlayer(SoundPlayer* player) noexcept { m_soundPlayer = player; }
  void registerIpc(IpcService& ipc);

  void summarize(std::string text);
  void translate(std::string text);
  void rewrite(std::string text);
  void speak(std::string text);
  void describeImage(std::string path);
  void readImage(std::string path);
  void toggleDictation();

  // Current clipboard text. IPC commands use this; the clipboard panel passes the selected entry.
  void summarizeClipboard();
  void translateClipboard();
  void rewriteClipboard();
  void speakClipboard();

  [[nodiscard]] const DesktopAiState& state() const noexcept { return m_state; }
  // Empty when a model with this capability is installed and LocalAI is running.
  [[nodiscard]] std::string unavailableReason(noctalia::ai_usage::LocalAiCapability capability) const;

  void addChangeCallback(ChangeCallback callback);

private:
  struct Endpoint {
    std::string baseUrl;
    std::string model;
  };

  [[nodiscard]] std::optional<Endpoint> resolve(noctalia::ai_usage::LocalAiCapability capability) const;
  [[nodiscard]] std::string modelReason(noctalia::ai_usage::LocalAiCapability capability) const;
  [[nodiscard]] std::string clipboardText() const;
  [[nodiscard]] bool claim(DesktopAiSurface surface, DesktopAiTask task, std::string status);
  void fail(DesktopAiSurface surface, std::string status);
  void succeed(std::string result, bool copyToClipboard);
  void notify();
  void postChat(
      noctalia::ai_usage::LocalAiCapability capability, std::string system, std::string user, bool copyToClipboard
  );
  void postVision(std::string path, std::string prompt);
  void postSpeech(std::string text);
  void transcribeFile(std::string path);
  void finishDictation(std::uint64_t generation, std::string path);

  AiUsageService& m_usage;
  HttpClient& m_http;
  ClipboardService& m_clipboard;
  SoundPlayer* m_soundPlayer = nullptr;
  DesktopAiState m_state;
  std::uint64_t m_generation = 0;
  std::shared_ptr<std::atomic<bool>> m_dictateCancel;
  std::vector<ChangeCallback> m_callbacks;
};
