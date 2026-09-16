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
class Select;
class Slider;
class Toggle;
struct ControlCenterServices;

class OsdTab : public Tab {
public:
  explicit OsdTab(const ControlCenterServices& services);

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
  void applyOrientation(std::size_t index);
  void applyPosition(std::size_t index);
  void applyBorder(bool enabled);
  void commitScale();
  void commitOpacity();

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Toggle* m_enabled = nullptr;
  Segmented* m_orientation = nullptr;
  Select* m_position = nullptr;
  Slider* m_scale = nullptr;
  Label* m_scaleValue = nullptr;
  Slider* m_opacity = nullptr;
  Label* m_opacityValue = nullptr;
  Toggle* m_border = nullptr;
  Flex* m_details = nullptr;

  bool m_syncing = false;
  bool m_liveDirty = false;
};
