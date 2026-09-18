#pragma once

#include "config/config_types.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <memory>
#include <string_view>

class ConfigService;
class Flex;
class Label;
class Renderer;
class Segmented;
struct ControlCenterServices;

class GlassTab : public Tab {
public:
  explicit GlassTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  [[nodiscard]] std::unique_ptr<Flex> createLiveBody();
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void syncLiveControls();
  void applyTransparencyMode(PanelTransparencyMode mode);

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Segmented* m_modeSwitch = nullptr;
  Label* m_caption = nullptr;

  bool m_syncing = false;
  bool m_liveDirty = false;
};
