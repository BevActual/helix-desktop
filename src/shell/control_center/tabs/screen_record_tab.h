#pragma once

#include "capture/screen_recorder_service.h"
#include "core/timer_manager.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

class Button;
class ConfigService;
class Label;
class Segmented;
class Toggle;
struct ControlCenterServices;

class ScreenRecordTab : public Tab {
public:
  explicit ScreenRecordTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void syncUi();
  void startElapsedTimer();
  void requestStart();
  void tickCountdown();
  void cancelCountdown();
  void openRecordingsFolder();
  [[nodiscard]] ScreenRecorderService::Source selectedSource() const noexcept;
  [[nodiscard]] int delaySeconds() const noexcept;

  ScreenRecorderService* m_recorder = nullptr;
  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Flex* m_rootLayout = nullptr;
  Segmented* m_source = nullptr;
  Segmented* m_delay = nullptr;
  Toggle* m_audio = nullptr;
  Button* m_recordButton = nullptr;
  Label* m_status = nullptr;
  Label* m_elapsed = nullptr;
  Label* m_backend = nullptr;
  Timer m_elapsedTimer;
  Timer m_delayTimer;
  std::vector<ScreenRecorderService::Source> m_sources;
  ScreenRecorderService::Source m_selectedSource = ScreenRecorderService::Source::Focused;
  std::size_t m_delayIndex = 0;
  bool m_pendingStart = false;
  int m_countdownLeft = 0;
};
