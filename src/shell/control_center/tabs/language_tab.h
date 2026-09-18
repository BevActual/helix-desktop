#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Button;
class ConfigService;
class Flex;
class GridView;
class Input;
class Label;
class Renderer;
struct ControlCenterServices;

class LanguageTab : public Tab {
public:
  explicit LanguageTab(const ControlCenterServices& services);

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
  void rebuildLanguageGrid();
  void applyLanguage(std::string code);

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Label* m_currentName = nullptr;
  Label* m_caption = nullptr;
  Input* m_filter = nullptr;
  GridView* m_grid = nullptr;
  Label* m_empty = nullptr;
  std::vector<Button*> m_tiles;
  std::vector<std::string> m_tileCodes;
  std::string m_query;
  std::string m_lastGridKey;

  bool m_syncing = false;
  bool m_liveDirty = false;
};
