#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "shell/control_center/tabs/bar_tab.h"
#include "shell/control_center/tabs/dock_tab.h"
#include "shell/control_center/tabs/osd_tab.h"
#include "shell/control_center/tabs/panels_tab.h"
#include "shell/control_center/tabs/windows_tab.h"
#include "ui/signal.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Button;
class ConfigService;
class DesktopWidgetsController;
class Flex;
class GridView;
class Input;
class Label;
class Renderer;
class Segmented;
class Select;
class Slider;
class Toggle;
struct ControlCenterServices;

class DesktopTab : public Tab {
public:
  enum class Section : std::uint8_t { Widgets, Corners, Bar, Dock, Windows, Panels, Osd };
  enum class WidgetCatalog : std::uint8_t { Glanceable, Sound, Tools };
  enum class CornersCatalog : std::uint8_t { Screen, Hot };

  explicit DesktopTab(const ControlCenterServices& services);

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
  [[nodiscard]] static Section sectionFromId(std::string_view id) noexcept;
  [[nodiscard]] static std::string_view idFromSection(Section section) noexcept;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void syncLiveControls();
  void syncHosts();
  void syncSettingsSection();
  void syncChildChrome();
  void persistSection();
  void persistWidgetCatalog();
  void persistCornersCatalog();
  void rebuildWidgetGrid();
  void applySection(std::size_t index);
  void applyWidgetCatalog(std::size_t index);
  void applyCornersCatalog(std::size_t index);
  void applyWidgets(bool enabled);
  void applyScreenCorners(bool enabled);
  void applyHotCorners(bool enabled);
  void commitScreenCornerSize();
  void commitHotCornerDelay();
  void applyHotCornerAction(std::string_view corner, std::size_t index);
  void addWidgetType(std::string type);
  void startEdit();
  [[nodiscard]] Tab* hostedTab() const noexcept;
  [[nodiscard]] Flex* hostNode() const noexcept;
  [[nodiscard]] std::vector<Section> visibleSections() const;
  [[nodiscard]] std::size_t indexFromSection(Section section) const noexcept;
  [[nodiscard]] Section sectionFromIndex(std::size_t index) const noexcept;
  [[nodiscard]] static std::size_t indexFromWidgetCatalog(WidgetCatalog catalog) noexcept;
  [[nodiscard]] static WidgetCatalog widgetCatalogFromIndex(std::size_t index) noexcept;
  [[nodiscard]] static WidgetCatalog widgetCatalogFromId(std::string_view id) noexcept;
  [[nodiscard]] static std::string_view idFromWidgetCatalog(WidgetCatalog catalog) noexcept;
  [[nodiscard]] static std::size_t indexFromCornersCatalog(CornersCatalog catalog) noexcept;
  [[nodiscard]] static CornersCatalog cornersCatalogFromIndex(std::size_t index) noexcept;
  [[nodiscard]] static CornersCatalog cornersCatalogFromId(std::string_view id) noexcept;
  [[nodiscard]] static std::string_view idFromCornersCatalog(CornersCatalog catalog) noexcept;
  [[nodiscard]] static bool widgetTypeInCatalog(std::string_view type, WidgetCatalog catalog) noexcept;

  ConfigService* m_config = nullptr;
  DesktopWidgetsController* m_widgets = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;
  std::unique_ptr<BarTab> m_bar;
  std::unique_ptr<DockTab> m_dock;
  std::unique_ptr<WindowsTab> m_windows;
  std::unique_ptr<PanelsTab> m_panels;
  std::unique_ptr<OsdTab> m_osd;

  Flex* m_rootLayout = nullptr;
  Segmented* m_sectionSwitch = nullptr;
  Flex* m_widgetsToolbar = nullptr;
  Flex* m_cornersToolbar = nullptr;
  Flex* m_barToolbar = nullptr;
  Flex* m_dockToolbar = nullptr;
  Flex* m_windowsToolbar = nullptr;
  Flex* m_panelsToolbar = nullptr;
  Flex* m_osdToolbar = nullptr;
  Flex* m_widgetsHost = nullptr;
  Flex* m_screenHost = nullptr;
  Flex* m_hotHost = nullptr;
  Flex* m_barHost = nullptr;
  Flex* m_dockHost = nullptr;
  Flex* m_windowsHost = nullptr;
  Flex* m_panelsHost = nullptr;
  Flex* m_osdHost = nullptr;
  Flex* m_screenDetails = nullptr;
  Flex* m_hotDetails = nullptr;
  Toggle* m_widgetsEnabled = nullptr;
  Toggle* m_screenCorners = nullptr;
  Toggle* m_hotCorners = nullptr;
  Slider* m_cornerSize = nullptr;
  Label* m_cornerSizeValue = nullptr;
  Slider* m_hotDelay = nullptr;
  Label* m_hotDelayValue = nullptr;
  Select* m_topLeft = nullptr;
  Select* m_topRight = nullptr;
  Select* m_bottomLeft = nullptr;
  Select* m_bottomRight = nullptr;
  Label* m_widgetCount = nullptr;
  Input* m_widgetFilter = nullptr;
  GridView* m_widgetGrid = nullptr;
  Label* m_widgetEmpty = nullptr;
  Button* m_edit = nullptr;
  Segmented* m_widgetCatalogSwitch = nullptr;
  Segmented* m_cornersCatalogSwitch = nullptr;
  Segmented* m_barCatalogSwitch = nullptr;
  Segmented* m_dockCatalogSwitch = nullptr;
  Segmented* m_windowsCatalogSwitch = nullptr;
  Segmented* m_panelsCatalogSwitch = nullptr;
  Segmented* m_osdCatalogSwitch = nullptr;
  std::vector<Button*> m_widgetTiles;
  std::vector<std::string> m_tileTypes;
  std::string m_widgetQuery;
  std::string m_lastGridKey;

  Section m_section = Section::Widgets;
  WidgetCatalog m_widgetCatalog = WidgetCatalog::Glanceable;
  CornersCatalog m_cornersCatalog = CornersCatalog::Screen;
  bool m_syncing = false;
  bool m_sectionSyncing = false;
  bool m_widgetCatalogSyncing = false;
  bool m_cornersCatalogSyncing = false;
  bool m_liveDirty = false;
};
