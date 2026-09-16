#pragma once

#include "compositors/niri/niri_config_binds.h"
#include "config/config_types.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Button;
class CompositorPlatform;
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

class ThemeTab : public Tab {
public:
  explicit ThemeTab(const ControlCenterServices& services);

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
  void updateAutoStatus();
  void rebuildPaletteSelect();
  void applyThemeMode(std::size_t index);
  void applyShellMode(std::size_t index);
  void applyPaletteSource(std::size_t index);
  void applyPaletteSelection(std::size_t index);
  void applyPureBlack(bool enabled);
  void applyNiriFocusRing(bool enabled);
  void applyNiriBorder(bool enabled);
  void applyNiriDrawBorder(bool enabled);
  void applyNiriBlur(bool enabled);
  void commitNiriBlurPasses();
  void commitNiriBlurOffset();
  void commitNiriBlurNoise();
  compositors::niri::InstallResult syncNiriLayoutTheme();
  compositors::niri::InstallResult syncNiriBlur();
  [[nodiscard]] bool niriBlurSupported() const;
  void openPalettePicker();
  void openCustomPalettesFolder();
  void exportWallpaperPalette();
  [[nodiscard]] PaletteSource selectedPaletteSource() const;
  [[nodiscard]] std::string paletteValueForSource(PaletteSource source) const;

  ConfigService* m_config = nullptr;
  noctalia::theme::ThemeService* m_theme = nullptr;
  CompositorPlatform* m_platform = nullptr;
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
  Toggle* m_niriFocusRing = nullptr;
  Toggle* m_niriBorder = nullptr;
  Toggle* m_niriDrawBorder = nullptr;
  Toggle* m_niriBlur = nullptr;
  Slider* m_niriBlurPasses = nullptr;
  Label* m_niriBlurPassesValue = nullptr;
  Slider* m_niriBlurOffset = nullptr;
  Label* m_niriBlurOffsetValue = nullptr;
  Slider* m_niriBlurNoise = nullptr;
  Label* m_niriBlurNoiseValue = nullptr;

  std::vector<std::string> m_paletteValues;
  bool m_syncing = false;
  bool m_liveDirty = false;
};
