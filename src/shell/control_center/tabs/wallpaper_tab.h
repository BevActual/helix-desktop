#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "shell/wallpaper/panel/wallpaper_picker.h"
#include "ui/signal.h"

#include <memory>
#include <string_view>

class ConfigService;
class Flex;
class Segmented;
struct ControlCenterServices;

class WallpaperTab : public Tab {
public:
  explicit WallpaperTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;
  void applyOpenContext(std::string_view context);
  [[nodiscard]] WallpaperPicker::BrowseSource browseSource() const noexcept;

private:
  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncLiveControls();
  void applyOverview(std::size_t index);
  void persistOverview();

  ConfigService* m_config = nullptr;
  WallpaperPicker m_picker;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Flex* m_pickerHost = nullptr;
  Segmented* m_overview = nullptr;
  bool m_syncing = false;
  bool m_liveDirty = false;
};
