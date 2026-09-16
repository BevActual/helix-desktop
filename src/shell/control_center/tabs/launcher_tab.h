#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class ConfigService;
class Flex;
class ListEditor;
class Renderer;
class Toggle;
struct ControlCenterServices;

class LauncherTab : public Tab {
public:
  explicit LauncherTab(const ControlCenterServices& services);

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
  void applyBool(std::vector<std::string> path, bool value);
  void commitPinned(std::vector<std::string> items);

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Toggle* m_categories = nullptr;
  Toggle* m_showIcons = nullptr;
  Toggle* m_origin = nullptr;
  Toggle* m_compact = nullptr;
  Toggle* m_appGrid = nullptr;
  Toggle* m_appActions = nullptr;
  Toggle* m_sortByUsage = nullptr;
  Toggle* m_typeToLaunch = nullptr;
  std::array<Toggle*, 5> m_providerGlobal{};
  ListEditor* m_pinnedList = nullptr;

  bool m_syncing = false;
  bool m_liveDirty = false;
};
