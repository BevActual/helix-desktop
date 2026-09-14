#pragma once

#include "config/config_types.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Button;
class ConfigService;
class Flex;
class Label;
class Renderer;
class Segmented;
class Select;
class Slider;
class Toggle;
struct ControlCenterServices;

namespace noctalia::theme {
  class ThemeService;
}

class AppearanceTab : public Tab {
public:
  explicit AppearanceTab(const ControlCenterServices& services);

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
  void syncFontButton();
  void updateAutoStatus();
  void rebuildPaletteSelect();
  void applyThemeMode(std::size_t index);
  void applyShellMode(std::size_t index);
  void applyPaletteSource(std::size_t index);
  void applyPaletteSelection(std::size_t index);
  void applyPureBlack(bool enabled);
  void applyGreeterAutoSync(bool enabled);
  void commitUiScale();
  void commitRoundness();
  void syncGreeterCard();
  void openFontPicker();
  void openPalettePicker();
  void openCustomPalettesFolder();
  void exportWallpaperPalette();
  [[nodiscard]] PaletteSource selectedPaletteSource() const;
  [[nodiscard]] std::string paletteValueForSource(PaletteSource source) const;

  ConfigService* m_config = nullptr;
  noctalia::theme::ThemeService* m_theme = nullptr;
  std::function<void()> m_syncGreeter;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Segmented* m_themeMode = nullptr;
  Segmented* m_shellMode = nullptr;
  Label* m_autoStatus = nullptr;
  Segmented* m_paletteSource = nullptr;
  Select* m_paletteSelect = nullptr;
  Button* m_palettePickerButton = nullptr;
  Label* m_paletteEmpty = nullptr;
  Button* m_openPalettesButton = nullptr;
  Button* m_exportButton = nullptr;
  Label* m_exportStatus = nullptr;
  Toggle* m_pureBlack = nullptr;
  Button* m_fontButton = nullptr;
  Slider* m_uiScale = nullptr;
  Label* m_uiScaleValue = nullptr;
  Slider* m_roundness = nullptr;
  Label* m_roundnessValue = nullptr;
  Flex* m_greeterCard = nullptr;
  Toggle* m_greeterAutoSync = nullptr;
  Button* m_greeterSyncButton = nullptr;

  std::vector<std::string> m_paletteValues;
  bool m_syncing = false;
  bool m_liveDirty = false;
};
