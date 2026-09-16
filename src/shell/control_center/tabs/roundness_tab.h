#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <memory>
#include <string>
#include <string_view>

class ConfigService;
class Flex;
class Label;
class Renderer;
class Slider;
struct ControlCenterServices;

class RoundnessTab : public Tab {
public:
  explicit RoundnessTab(const ControlCenterServices& services);

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
  void commitRoundness();
  void commitBarRadius();
  void commitDockRadius();
  void commitWindowRadius();

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Slider* m_roundness = nullptr;
  Label* m_roundnessValue = nullptr;
  Slider* m_barRadius = nullptr;
  Label* m_barRadiusValue = nullptr;
  Slider* m_dockRadius = nullptr;
  Label* m_dockRadiusValue = nullptr;
  Slider* m_windowRadius = nullptr;
  Label* m_windowRadiusValue = nullptr;

  std::string m_barName;
  bool m_syncing = false;
  bool m_liveDirty = false;
};
