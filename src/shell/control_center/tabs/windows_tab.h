#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <memory>
#include <string_view>

class ConfigService;
class Flex;
class Label;
class Renderer;
class Slider;
struct ControlCenterServices;

class WindowsTab : public Tab {
public:
  explicit WindowsTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncLiveControls();
  void commitGaps();
  void commitStruts();
  void commitColumnWidth();
  void persistLayout();

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Slider* m_gaps = nullptr;
  Label* m_gapsValue = nullptr;
  Slider* m_struts = nullptr;
  Label* m_strutsValue = nullptr;
  Slider* m_columnWidth = nullptr;
  Label* m_columnWidthValue = nullptr;

  bool m_syncing = false;
  bool m_liveDirty = false;
};
