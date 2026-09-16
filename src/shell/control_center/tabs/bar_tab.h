#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

class ConfigService;
class Flex;
class Label;
class Renderer;
class Segmented;
class Slider;
class Toggle;
struct ControlCenterServices;

class BarTab : public Tab {
public:
  explicit BarTab(const ControlCenterServices& services);

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
  void applyEnabled(bool enabled);
  void applyPosition(std::size_t index);
  void applyAutoHide(std::size_t index);
  void applyReserveSpace(bool enabled);
  void applyShadow(bool enabled);
  void commitThickness();
  void commitOpacity();

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Flex* m_enableRow = nullptr;
  Flex* m_details = nullptr;
  Flex* m_shapeCard = nullptr;
  Toggle* m_enabled = nullptr;
  Segmented* m_position = nullptr;
  Segmented* m_autoHide = nullptr;
  Toggle* m_reserveSpace = nullptr;
  Toggle* m_shadow = nullptr;
  Slider* m_thickness = nullptr;
  Label* m_thicknessValue = nullptr;
  Slider* m_opacity = nullptr;
  Label* m_opacityValue = nullptr;
  Label* m_empty = nullptr;

  std::string m_barName;
  bool m_syncing = false;
  bool m_liveDirty = false;
};
