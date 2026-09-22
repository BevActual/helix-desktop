#pragma once

#include "config/config_types.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <memory>
#include <string_view>

class CompositorPlatform;
class ConfigService;
class Flex;
class Label;
class Renderer;
class Slider;
class Toggle;
struct ControlCenterServices;

class GlassTab : public Tab {
public:
  explicit GlassTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  [[nodiscard]] std::unique_ptr<Flex> createLiveBody(bool hosted = false);
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
  void previewGlass(double next);
  void commitGlass();
  void applyRefraction(bool enabled);
  void persistCompositorGlass();

  ConfigService* m_config = nullptr;
  CompositorPlatform* m_platform = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Slider* m_glass = nullptr;
  Label* m_glassValue = nullptr;
  Label* m_caption = nullptr;
  Toggle* m_refraction = nullptr;
  Label* m_refractionCaption = nullptr;

  bool m_hosted = false;
  bool m_syncing = false;
  bool m_liveDirty = false;
};
