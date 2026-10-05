#include "shell/control_center/settings_pane.h"
#include "core/deferred_call.h"
#include "shell/panel/panel_manager.h"
#include "shell/panel/panel_button_style.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/button.h"
#include "ui/style.h"
#include "render/core/renderer.h"

SettingsPane::SettingsPane(const ControlCenterServices&, SettingsPaneOptions options)
    : m_selectedSection(std::move(options.section)) {}
SettingsPane::~SettingsPane() = default;

void SettingsPane::openNative() const {
  const auto section = m_selectedSection;
  DeferredCall::callLater([section] { PanelManager::instance().openSettings(section); });
}
std::unique_ptr<Flex> SettingsPane::create(float, float) {
  openNative();
  auto empty = ui::column({.out = &m_root});
  return empty;
}
std::unique_ptr<Flex> SettingsPane::createInline(float scale, float) { return createHeaderActions(scale); }
std::unique_ptr<Flex> SettingsPane::createHeaderActions(float scale) {
  auto actions = ui::row({.align = FlexAlign::Center});
  actions->addChild(ui::button({
      .text = "Open Settings", .glyph = "settings", .onClick = [this] { openNative(); },
      .configure = [scale](Button& button) { panel_button_style::configureHeaderTextButton(button, scale); },
  }));
  return actions;
}
std::unique_ptr<Flex> SettingsPane::wrapLive(std::unique_ptr<Flex> live, float, float) {
  m_root = live.get();
  return live;
}
std::unique_ptr<Flex> SettingsPane::embedHeader(std::unique_ptr<Flex> liveActions, float scale) {
  auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  if (liveActions) actions->addChild(std::move(liveActions));
  actions->addChild(createHeaderActions(scale));
  return actions;
}
void SettingsPane::onClose() { m_root = nullptr; }
void SettingsPane::layout(Renderer& renderer, float width, float height) {
  if (m_root) { m_root->setSize(width, height); m_root->layout(renderer); }
}
bool SettingsPane::layoutEmbedded(Renderer&, float, float) { return false; }
void SettingsPane::setConfigureMode(bool show) { if (show) openNative(); }
void SettingsPane::setSection(std::string section) { m_selectedSection = std::move(section); }
void SettingsPane::setSearchQuery(std::string) {}
void SettingsPane::setIncludedGroups(std::unordered_set<std::string>) {}
void SettingsPane::onExternalChanged() {}
void SettingsPane::setSelectedBarName(std::string) {}
void SettingsPane::openCalendarAccount(std::optional<std::string>) { openNative(); }
void SettingsPane::openSearchPicker(settings::SearchPickerOpenRequest) { openNative(); }
void SettingsPane::openUserTemplateEditor(std::string) { openNative(); }
void SettingsPane::openPluginSettings(std::string) { openNative(); }
void SettingsPane::openWidgetInspector(std::string, std::string) { openNative(); }
