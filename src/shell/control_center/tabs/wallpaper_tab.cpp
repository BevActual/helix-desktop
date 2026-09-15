#include "shell/control_center/tabs/wallpaper_tab.h"

#include "compositors/compositor_platform.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "ui/controls/flex.h"

#include <string>
#include <utility>

WallpaperTab::WallpaperTab(const ControlCenterServices& services)
    : m_picker(
          services.platform != nullptr ? &services.platform->wayland() : nullptr, services.config, services.thumbnails,
          services.wallpaperScanner, services.theme, services.httpClient,
          WallpaperPicker::Options{.showTitle = false, .showCloseButton = false}, services.videoWallpaper
      ),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "wallpaper"})) {}

std::unique_ptr<Flex> WallpaperTab::create() {
  m_picker.setContentScale(contentScale());
  m_picker.setPanelCardOpacity(panelCardOpacity());
  return m_settings->wrapLive(m_picker.create(), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> WallpaperTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void WallpaperTab::setActive(bool active) {
  if (active) {
    m_picker.onOpen({});
  }
}

void WallpaperTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_picker.onClose();
}

void WallpaperTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool WallpaperTab::configureMode() const { return m_settings->configureMode(); }

void WallpaperTab::setSettingsSearchQuery(std::string_view query) {
  m_settings->setSearchQuery(std::string(query));
}

void WallpaperTab::onPanelCardOpacityChanged(float opacity) { m_picker.setPanelCardOpacity(opacity); }

void WallpaperTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  m_picker.setContentScale(contentScale());
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  m_picker.layout(renderer, contentWidth, bodyHeight);
}

void WallpaperTab::doUpdate(Renderer& renderer) {
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  m_picker.update(renderer);
}
