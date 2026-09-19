#pragma once

#include "compositors/niri/niri_config_binds.h"
#include "config/config_types.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "shell/control_center/tabs/effects_tab.h"
#include "shell/control_center/tabs/glass_tab.h"
#include "shell/control_center/tabs/icons_tab.h"
#include "shell/control_center/tabs/language_tab.h"
#include "shell/control_center/tabs/roundness_tab.h"
#include "shell/control_center/tabs/scale_tab.h"
#include "shell/control_center/tabs/type_tab.h"
#include "ui/palette.h"
#include "ui/signal.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Button;
class ColorSwatchPreviewStrip;
class ConfigService;
class Flex;
class Input;
class Label;
class Renderer;
class Segmented;
class VirtualGridAdapter;
class VirtualGridView;
struct ControlCenterServices;

namespace noctalia::theme {
  class ThemeService;
}

class ThemeTab : public Tab {
public:
  enum class Section : std::uint8_t {
    Palettes,
    Chrome,
    Icons,
    Type,
    Effects,
    Language,
  };

  enum class ChromeCatalog : std::uint8_t {
    Glass,
    Scale,
    Roundness,
  };

  explicit ThemeTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;
  void applyOpenSection(Section section, bool persist = true);
  void applyOpenContext(std::string_view context, bool persist = true);
  [[nodiscard]] bool matchesOpenContext(std::string_view context) const noexcept;
  [[nodiscard]] Section currentSection() const noexcept { return m_section; }
  [[nodiscard]] ChromeCatalog currentChromeCatalog() const noexcept { return m_chromeCatalog; }
  [[nodiscard]] static Section sectionFromId(std::string_view id) noexcept;
  [[nodiscard]] static std::string_view idFromSection(Section section) noexcept;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void syncLiveControls();
  void updateAutoStatus();
  void rebuildPaletteGrid();
  void applyThemeMode(std::size_t index);
  void applyPaletteSource(std::size_t index);
  void applyPaletteSelection(std::size_t index);
  void applySection(std::size_t index);
  void applyChromeCatalog(std::size_t index);
  void persistSection();
  void persistChrome();
  void syncHosts();
  void syncSettingsSection();
  void syncChildChrome();
  compositors::niri::InstallResult syncNiriLayoutTheme();
  void openCustomPalettesFolder();
  void saveCurrentPalette();
  void pickRoleColor(ColorRole role, std::string roleLabel);
  [[nodiscard]] PaletteSource selectedPaletteSource() const;
  [[nodiscard]] std::string paletteValueForSource(PaletteSource source) const;
  [[nodiscard]] Tab* hostedTab() const noexcept;
  [[nodiscard]] Flex* hostNode() const noexcept;
  [[nodiscard]] static std::size_t indexFromSection(Section section) noexcept;
  [[nodiscard]] static Section sectionFromIndex(std::size_t index) noexcept;
  [[nodiscard]] static std::size_t indexFromChromeCatalog(ChromeCatalog catalog) noexcept;
  [[nodiscard]] static ChromeCatalog chromeCatalogFromIndex(std::size_t index) noexcept;
  [[nodiscard]] static ChromeCatalog chromeCatalogFromId(std::string_view id) noexcept;
  [[nodiscard]] static std::string_view idFromChromeCatalog(ChromeCatalog catalog) noexcept;

  ConfigService* m_config = nullptr;
  noctalia::theme::ThemeService* m_theme = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;
  std::unique_ptr<GlassTab> m_glass;
  std::unique_ptr<IconsTab> m_icons;
  std::unique_ptr<TypeTab> m_type;
  std::unique_ptr<ScaleTab> m_scale;
  std::unique_ptr<RoundnessTab> m_roundness;
  std::unique_ptr<EffectsTab> m_effects;
  std::unique_ptr<LanguageTab> m_language;
  std::unique_ptr<VirtualGridAdapter> m_paletteAdapter;

  Flex* m_rootLayout = nullptr;
  Segmented* m_sectionSwitch = nullptr;
  Flex* m_palettesToolbar = nullptr;
  Flex* m_chromeToolbar = nullptr;
  Segmented* m_chromeSwitch = nullptr;
  Flex* m_palettesHost = nullptr;
  Flex* m_palettesLeftover = nullptr;
  ColorSwatchPreviewStrip* m_currentSwatches = nullptr;
  Flex* m_glassHost = nullptr;
  Flex* m_iconsHost = nullptr;
  Flex* m_typeHost = nullptr;
  Flex* m_scaleHost = nullptr;
  Flex* m_roundnessHost = nullptr;
  Flex* m_effectsHost = nullptr;
  Flex* m_languageHost = nullptr;
  Segmented* m_modeSwitch = nullptr;
  Button* m_autoStatus = nullptr;
  Label* m_paletteName = nullptr;
  Segmented* m_sourceSwitch = nullptr;
  Input* m_paletteFilter = nullptr;
  Flex* m_catalogToolbar = nullptr;
  VirtualGridView* m_paletteGrid = nullptr;
  Label* m_paletteEmpty = nullptr;
  Button* m_openWallpaperButton = nullptr;
  Button* m_openPalettesButton = nullptr;
  Button* m_saveCustomButton = nullptr;
  Label* m_exportStatus = nullptr;

  std::vector<std::string> m_paletteValues;
  std::string m_paletteQuery;
  Section m_section = Section::Palettes;
  ChromeCatalog m_chromeCatalog = ChromeCatalog::Glass;
  bool m_syncing = false;
  bool m_liveDirty = false;
  bool m_sectionSyncing = false;
  bool m_chromeSyncing = false;
};
