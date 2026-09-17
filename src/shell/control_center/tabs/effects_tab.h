#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

class ConfigService;
class Flex;
class Label;
class Renderer;
class Select;
class Slider;
class Toggle;
struct ControlCenterServices;

class EffectsTab : public Tab {
public:
  explicit EffectsTab(const ControlCenterServices& services);

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
  void commitAnimationSpeed();
  void commitShadowAlpha();
  void applyShadowDirection(std::size_t index);
  void applyTransparencyMode(std::size_t index);

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Select* m_transparencyMode = nullptr;
  Toggle* m_highContrast = nullptr;
  Toggle* m_iconColorize = nullptr;
  Flex* m_iconColorSection = nullptr;
  Select* m_iconColor = nullptr;
  Toggle* m_animations = nullptr;
  Flex* m_animationSpeedSection = nullptr;
  Slider* m_animationSpeed = nullptr;
  Label* m_animationSpeedValue = nullptr;
  Toggle* m_buttonBorders = nullptr;
  Toggle* m_inputBorders = nullptr;
  Toggle* m_popupBorders = nullptr;
  Toggle* m_cardBorders = nullptr;
  Toggle* m_popupShadows = nullptr;
  Flex* m_shadowDetails = nullptr;
  Select* m_shadowDirection = nullptr;
  Slider* m_shadowAlpha = nullptr;
  Label* m_shadowAlphaValue = nullptr;
  std::vector<std::string> m_shadowDirectionKeys;

  bool m_syncing = false;
  bool m_liveDirty = false;
};
