#pragma once

#include "shell/clipboard/clipboard_picker.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"

#include <functional>
#include <memory>
#include <string_view>

class ConfigService;
struct ControlCenterServices;

class ClipboardTab : public Tab {
public:
  explicit ClipboardTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;

private:
  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;

  ConfigService* m_config = nullptr;
  std::function<void(bool isImage)> m_scheduleAutoPaste;
  ClipboardPicker m_picker;
  std::unique_ptr<SettingsPane> m_settings;
};
