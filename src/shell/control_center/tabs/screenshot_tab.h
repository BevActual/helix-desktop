#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Button;
class ConfigService;
class IpcService;
class Label;
class Segmented;
struct ControlCenterServices;

class ScreenshotTab : public Tab {
public:
  explicit ScreenshotTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void requestCapture(const char* command);
  void runCapture(const char* command);
  void tickCountdown();
  void cancelCountdown();
  void syncCountdownUi();
  void openScreenshotFolder();
  void openRecentFile(const std::string& path);
  void rebuildRecentFiles();
  [[nodiscard]] int delaySeconds() const noexcept;

  struct CaptureButton {
    Button* button = nullptr;
    bool available = true;
  };

  IpcService* m_ipc = nullptr;
  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Flex* m_rootLayout = nullptr;
  Segmented* m_delay = nullptr;
  Label* m_status = nullptr;
  Flex* m_recentList = nullptr;
  Label* m_recentEmpty = nullptr;
  std::vector<CaptureButton> m_captureButtons;
  Timer m_delayTimer;
  const char* m_pendingCommand = nullptr;
  int m_countdownLeft = 0;
  std::size_t m_delayIndex = 0;
  std::string m_lastRecentKey;
};
