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
class Renderer;
class Select;
struct ControlCenterServices;

class LanguageTab : public Tab {
public:
  explicit LanguageTab(const ControlCenterServices& services);

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
  void applyLanguage(std::size_t index);

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Select* m_language = nullptr;
  std::vector<std::string> m_languageCodes;

  bool m_syncing = false;
  bool m_liveDirty = false;
};
