#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "shell/control_center/tabs/screen_record_tab.h"
#include "shell/control_center/tabs/screenshot_tab.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class ConfigService;
class Flex;
class Segmented;
struct ControlCenterServices;

class CaptureTab : public Tab {
public:
  enum class View : std::uint8_t { Screenshot, Record };

  explicit CaptureTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;
  void applyOpenView(View view, bool persist = true);
  [[nodiscard]] View currentView() const noexcept { return m_view; }

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void applyView(std::size_t index);
  void persistView();
  void syncHosts();
  void syncSettingsSection();
  [[nodiscard]] static std::size_t indexFromView(View view) noexcept;
  [[nodiscard]] static View viewFromIndex(std::size_t index) noexcept;

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  std::unique_ptr<ScreenshotTab> m_screenshot;
  std::unique_ptr<ScreenRecordTab> m_record;
  Flex* m_rootLayout = nullptr;
  Segmented* m_viewSwitch = nullptr;
  Flex* m_screenshotCompactHost = nullptr;
  Flex* m_recordCompactHost = nullptr;
  Flex* m_screenshotLeftoverHost = nullptr;
  Flex* m_recordLeftoverHost = nullptr;
  View m_view = View::Screenshot;
  bool m_viewSyncing = false;
};
