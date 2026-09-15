#pragma once

#include "capture/screen_recorder_service.h"
#include "core/timer_manager.h"
#include "shell/control_center/recent_captures.h"
#include "ui/signal.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Button;
class ConfigService;
class Flex;
class Label;
class Renderer;
class Segmented;
class ThumbnailService;
class Toggle;
struct ControlCenterServices;

class ScreenRecordTab {
public:
  explicit ScreenRecordTab(const ControlCenterServices& services);

  [[nodiscard]] std::unique_ptr<Flex> createLiveBody();
  void setActive(bool active);
  void onClose();
  void updateContent(Renderer& renderer);
  void cancelCountdown();
  void setContentScale(float scale) noexcept { m_contentScale = scale; }
  void setPanelCardOpacity(float opacity) noexcept;

private:
  void buildRecordCard(Flex& root, float scale);
  void buildReplayCard(Flex& root, float scale);
  void buildRecentCard(Flex& root, float scale);
  void syncUi();
  void syncReplayUi();
  void startElapsedTimer();
  void requestStart();
  void tickCountdown();
  void openRecordingsFolder();
  void openRecentFile(const std::string& path);
  void rebuildRecentFiles();
  void bindRecentGallery();
  void requestReplayStart();
  void requestReplayStop();
  void requestReplaySave();
  [[nodiscard]] ScreenRecorderService::Source selectedSource() const noexcept;
  [[nodiscard]] ScreenRecorderService::Source replaySource() const noexcept;
  [[nodiscard]] int delaySeconds() const noexcept;
  [[nodiscard]] float contentScale() const noexcept { return m_contentScale; }
  [[nodiscard]] float panelCardOpacity() const noexcept { return m_panelCardOpacity; }

  ScreenRecorderService* m_recorder = nullptr;
  ConfigService* m_config = nullptr;
  ThumbnailService* m_thumbnails = nullptr;
  control_center::RecentCaptureGallery m_recents;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;
  Flex* m_rootLayout = nullptr;
  Segmented* m_source = nullptr;
  Segmented* m_delay = nullptr;
  Toggle* m_audio = nullptr;
  Button* m_recordButton = nullptr;
  Label* m_status = nullptr;
  Label* m_elapsed = nullptr;
  Label* m_backend = nullptr;
  Label* m_replayHint = nullptr;
  Button* m_replayStartButton = nullptr;
  Button* m_replaySaveButton = nullptr;
  Label* m_replayStatus = nullptr;
  Flex* m_recentList = nullptr;
  Label* m_recentEmpty = nullptr;
  Timer m_elapsedTimer;
  Timer m_delayTimer;
  std::vector<ScreenRecorderService::Source> m_sources;
  ScreenRecorderService::Source m_selectedSource = ScreenRecorderService::Source::Focused;
  std::size_t m_delayIndex = 0;
  bool m_pendingStart = false;
  int m_countdownLeft = 0;
  float m_contentScale = 1.0F;
  float m_panelCardOpacity = 1.0F;
};
