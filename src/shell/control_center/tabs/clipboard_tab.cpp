#include "shell/control_center/tabs/clipboard_tab.h"

#include "config/config_service.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "ui/controls/flex.h"
#include "wayland/clipboard_service.h"

ClipboardTab::ClipboardTab(const ControlCenterServices& services)
    : m_config(services.config), m_scheduleAutoPaste(services.scheduleClipboardAutoPaste),
      m_picker(
          services.clipboard, services.config, services.asyncTextures,
          ClipboardPicker::Options{.showTitle = false, .showCloseButton = false, .closeHostWhenEmpty = false}
      ),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "clipboard", .includedGroups = {"clipboard"}}
      )) {
  m_picker.setCloseCallback([]() { PanelManager::instance().close(); });
  m_picker.setActivateCallback([this](const ClipboardEntry& entry) {
    if (m_config == nullptr) {
      PanelManager::instance().close();
      return;
    }
    const ClipboardAutoPasteMode mode = m_config->config().shell.clipboardAutoPaste;
    if (mode == ClipboardAutoPasteMode::Off || !m_scheduleAutoPaste) {
      PanelManager::instance().close();
      return;
    }
    // Auto-paste injects a keystroke into whatever holds keyboard focus. The animated close keeps
    // the panel surface (and its keyboard focus) alive for the duration of the reveal animation, so
    // the keys would land on the closing panel instead of the target window. Close without animation
    // so focus returns to the toplevel before we paste, mirroring the launcher's app-launch close.
    PanelManager::instance().closePanel(false);
    m_scheduleAutoPaste(entry.isImage());
  });
}

std::unique_ptr<Flex> ClipboardTab::create() {
  m_picker.setContentScale(contentScale());
  m_picker.setPanelCardOpacity(panelCardOpacity());
  return m_settings->wrapLive(m_picker.create(), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> ClipboardTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void ClipboardTab::setActive(bool active) {
  if (active) {
    m_picker.onOpen({});
  }
}

void ClipboardTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_picker.onClose();
}

void ClipboardTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool ClipboardTab::configureMode() const { return m_settings->configureMode(); }

void ClipboardTab::setSettingsSearchQuery(std::string_view query) {
  m_settings->setSearchQuery(std::string(query));
}

void ClipboardTab::onPanelCardOpacityChanged(float opacity) { m_picker.setPanelCardOpacity(opacity); }

void ClipboardTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  m_picker.setContentScale(contentScale());
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  m_picker.layout(renderer, contentWidth, bodyHeight);
}

void ClipboardTab::doUpdate(Renderer& renderer) {
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  m_picker.update(renderer);
}
