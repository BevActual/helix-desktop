#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class ConfigService;
class Flex;
class ListEditor;
class Renderer;
class Segmented;
class Toggle;
struct ControlCenterServices;

class DockTab : public Tab {
public:
  explicit DockTab(const ControlCenterServices& services);

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
  void applyEnabled(bool enabled);
  void applyPosition(std::size_t index);
  void applyAutoHide(std::size_t index);
  void applyShowRunning(bool enabled);
  void commitPinned(std::vector<std::string> items);

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Toggle* m_enabled = nullptr;
  Flex* m_details = nullptr;
  Segmented* m_position = nullptr;
  Segmented* m_autoHide = nullptr;
  Toggle* m_showRunning = nullptr;
  Flex* m_pinnedCard = nullptr;
  ListEditor* m_pinnedList = nullptr;

  bool m_syncing = false;
  bool m_liveDirty = false;
};
