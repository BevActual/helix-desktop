#pragma once

#include "capture/screencopy_capture.h"
#include "core/timer_manager.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class CompositorPlatform;
class ConfigService;
class IpcService;
class NotificationManager;
class ScreenshotService;

class ScreenRecorderService {
public:
  enum class Source : std::uint8_t { Focused, Portal, Region };

  using ChangeCallback = std::function<void()>;

  ScreenRecorderService(
      CompositorPlatform& platform, ConfigService& config, NotificationManager& notifications,
      ScreenshotService& screenshots
  );
  ~ScreenRecorderService();

  ScreenRecorderService(const ScreenRecorderService&) = delete;
  ScreenRecorderService& operator=(const ScreenRecorderService&) = delete;

  [[nodiscard]] bool available() const;
  [[nodiscard]] bool hasGpuScreenRecorder() const;
  [[nodiscard]] bool hasWfRecorder() const;
  [[nodiscard]] bool supportsSource(Source source) const;
  [[nodiscard]] std::vector<Source> availableSources() const;
  [[nodiscard]] bool isRecording() const noexcept;
  [[nodiscard]] bool isStopping() const noexcept;
  [[nodiscard]] bool isReplaying() const noexcept;
  [[nodiscard]] bool isReplayStopping() const noexcept;
  [[nodiscard]] std::chrono::seconds elapsed() const;
  [[nodiscard]] std::string elapsedLabel() const;
  [[nodiscard]] std::filesystem::path outputPath() const;
  [[nodiscard]] std::string backendDescription() const;
  [[nodiscard]] int replayDurationSeconds() const;

  void setChangeCallback(ChangeCallback callback);

  [[nodiscard]] std::string start(Source source);
  [[nodiscard]] std::string stop();
  [[nodiscard]] std::string toggle(Source source);
  [[nodiscard]] std::string startReplay(Source source);
  [[nodiscard]] std::string stopReplay();
  [[nodiscard]] std::string saveReplay();
  [[nodiscard]] std::string toggleReplay(Source source);

  void registerIpc(IpcService& ipc);

  [[nodiscard]] static std::optional<Source> parseSource(std::string_view token);
  [[nodiscard]] static std::optional<Source> parseReplaySource(std::string_view token);

private:
  enum class State : std::uint8_t { Idle, Recording, Stopping, Replaying, StoppingReplay };

  void notifyChanged();
  void notifySaved();
  void notifyReplaySaved();
  void notifyFailed(const std::string& message);
  [[nodiscard]] std::filesystem::path outputDirectory() const;
  [[nodiscard]] std::filesystem::path makeOutputPath() const;
  [[nodiscard]] std::string focusedOutputName() const;
  [[nodiscard]] bool audioEnabled() const;
  [[nodiscard]] bool showCursor() const;
  [[nodiscard]] bool processAlive() const;
  [[nodiscard]] std::vector<std::string> gsrCommand() const;
  bool launch(const std::vector<std::string>& args, const std::filesystem::path& output, State nextState);
  bool launchGsr(const std::string& window, const std::filesystem::path& output);
  bool launchGsrReplay(const std::string& window, const std::filesystem::path& directory);
  bool launchWf(
      const std::optional<std::string>& outputName, const std::optional<LogicalRect>& region,
      const std::filesystem::path& output
  );
  void startPolling();
  void onProcessCheck();
  void requestStop();
  void requestStopReplay();
  void finalize(bool expectFile);
  void finalizeReplay();
  void sendSignal(int sig);

  CompositorPlatform& m_platform;
  ConfigService& m_config;
  NotificationManager& m_notifications;
  ScreenshotService& m_screenshots;
  ChangeCallback m_onChange;
  State m_state = State::Idle;
  int m_pid = -1;
  std::filesystem::path m_outputPath;
  std::chrono::steady_clock::time_point m_startedAt{};
  Timer m_pollTimer;
  Timer m_stopTimer;
  Timer m_saveTimer;
  int m_stopWaitTicks = 0;
};
