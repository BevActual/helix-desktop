#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/palette.h"
#include "ui/signal.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Box;
class Button;
class ConfigService;
class Flex;
class Label;
class Renderer;
class Segmented;
struct ControlCenterServices;

class IconsTab : public Tab {
public:
  explicit IconsTab(const ControlCenterServices& services);

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
  void syncLiveControls();
  void applyIconStyle(bool tinted);
  void applyTintRole(ColorRole role);
  void pickCustomTint();

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Segmented* m_styleSwitch = nullptr;
  Label* m_caption = nullptr;
  Flex* m_tintRow = nullptr;
  Button* m_customTint = nullptr;
  std::vector<Button*> m_tintButtons;
  std::array<Box*, 3> m_tintedPreview{};
  std::array<Box*, 3> m_specimenPreview{};

  bool m_syncing = false;
  bool m_liveDirty = false;
};
