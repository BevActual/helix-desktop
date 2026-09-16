#include "shell/control_center/tabs/wallpaper_tab.h"

#include "compositors/compositor_platform.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "ui/controls/flex.h"

WallpaperTab::WallpaperTab(const ControlCenterServices& services)
    : m_picker(
          services.platform != nullptr ? &services.platform->wayland() : nullptr, services.config, services.thumbnails,
          services.wallpaperScanner, services.theme, services.httpClient,
          WallpaperPicker::Options{.showTitle = false, .showCloseButton = false}, services.videoWallpaper
      ),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "wallpaper"})) {}

std::unique_ptr<Flex> WallpaperTab::create() {
  const float scale = contentScale();
  m_picker.setContentScale(scale);
  m_picker.setPanelCardOpacity(panelCardOpacity());

  auto picker = m_picker.create();
  picker->setFlexGrow(1.0F);
  return m_settings->wrapLive(std::move(picker), scale, panelCardOpacity());
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
  if (!query.empty()) {
    m_settings->setConfigureMode(true);
  }
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* WallpaperTab::settingsPane() { return m_settings.get(); }

void WallpaperTab::applyOpenContext(std::string_view context) {
  if (context.empty()) {
    return;
  }
  m_picker.onOpen(context);
}

WallpaperPicker::BrowseSource WallpaperTab::browseSource() const noexcept { return m_picker.browseSource(); }

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
