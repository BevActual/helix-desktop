#pragma once

#include "shell/control_center/control_center_services.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"

#include <string>
#include <string_view>
#include <utility>

class Flex;
class Renderer;

class SettingsTab : public Tab {
public:
  SettingsTab(const ControlCenterServices& services, std::string section);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  [[nodiscard]] bool configureMode() const override { return true; }
  void setSettingsSearchQuery(std::string_view query) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;

  SettingsPane m_pane;
};
