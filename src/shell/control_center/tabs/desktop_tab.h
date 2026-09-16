#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <memory>
#include <string_view>

class Button;
class ConfigService;
class DesktopWidgetsController;
class Flex;
class Label;
class Renderer;
class Toggle;
struct ControlCenterServices;

class DesktopTab : public Tab {
public:
  explicit DesktopTab(const ControlCenterServices& services);

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
  void applyWidgets(bool enabled);
  void applyScreenCorners(bool enabled);
  void applyHotCorners(bool enabled);
  void startEdit();

  ConfigService* m_config = nullptr;
  DesktopWidgetsController* m_widgets = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Flex* m_widgetDetails = nullptr;
  Toggle* m_widgetsEnabled = nullptr;
  Toggle* m_screenCorners = nullptr;
  Toggle* m_hotCorners = nullptr;
  Label* m_widgetCount = nullptr;
  Button* m_edit = nullptr;

  bool m_syncing = false;
  bool m_liveDirty = false;
};
