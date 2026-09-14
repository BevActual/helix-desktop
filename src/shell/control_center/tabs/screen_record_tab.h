#pragma once

#include "capture/screen_recorder_service.h"
#include "core/timer_manager.h"
#include "shell/control_center/recent_captures.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
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
class Segmented;
class ThumbnailService;
class Toggle;
struct ControlCenterServices;

class ScreenRecordTab : public Tab {
public:
  explicit ScreenRecordTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void buildRecordCard(Flex& root, float scale);
  void buildReplayCard(Flex& root, float scale);
  void buildRecentCard(Flex& root, float scale);
  void syncUi();
  void syncReplayUi();
  void startElapsedTimer();
  void requestStart();
  void tickCountdown();
  void cancelCountdown();
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

  ScreenRecorderService* m_recorder = nullptr;
  ConfigService* m_config = nullptr;
  ThumbnailService* m_thumbnails = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
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
};
