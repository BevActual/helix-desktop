#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <cstddef>
#include <memory>
#include <string_view>

class ConfigService;
class Flex;
class Label;
class Renderer;
class Segmented;
class Slider;
class Toggle;
struct ControlCenterServices;

class PanelsTab : public Tab {
public:
  explicit PanelsTab(const ControlCenterServices& services);

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
  void applyFloatingLayer(std::size_t index);
  void applyBorders(bool enabled);
  void applyShadow(bool enabled);
  void applyListItemBackground(bool enabled);
  void commitFloatingOffset();

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Segmented* m_floatingLayer = nullptr;
  Toggle* m_borders = nullptr;
  Toggle* m_shadow = nullptr;
  Toggle* m_listItemBackground = nullptr;
  Slider* m_floatingOffset = nullptr;
  Label* m_floatingOffsetValue = nullptr;

  bool m_syncing = false;
  bool m_liveDirty = false;
};
