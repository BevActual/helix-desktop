#include "shell/control_center/tabs/settings_tab.h"

#include "render/core/renderer.h"
#include "ui/controls/flex.h"

SettingsTab::SettingsTab(
    const ControlCenterServices& services, std::string section, std::unordered_set<std::string> includedGroups
)
    : m_pane(
          services,
          SettingsPaneOptions{.section = std::move(section), .includedGroups = std::move(includedGroups)}
      ) {}

std::unique_ptr<Flex> SettingsTab::create() { return m_pane.create(contentScale(), panelCardOpacity()); }

std::unique_ptr<Flex> SettingsTab::createHeaderActions() { return m_pane.createHeaderActions(contentScale()); }

void SettingsTab::onClose() { m_pane.onClose(); }

void SettingsTab::setSettingsSearchQuery(std::string_view query) { m_pane.setSearchQuery(std::string(query)); }

void SettingsTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  m_pane.layout(renderer, contentWidth, bodyHeight);
}
