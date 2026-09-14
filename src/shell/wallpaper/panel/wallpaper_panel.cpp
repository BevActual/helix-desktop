#include "shell/wallpaper/panel/wallpaper_panel.h"

#include "config/config_service.h"
#include "ui/controls/flex.h"

#include <utility>

WallpaperPanel::WallpaperPanel(
    WaylandConnection* wayland, ConfigService* config, ThumbnailService* thumbnails, WallpaperScanner* scanner,
    noctalia::theme::ThemeService* themeService, HttpClient* httpClient, scripting::PluginManager* pluginManager
)
    : m_config(config),
      m_picker(wayland, config, thumbnails, scanner, themeService, httpClient, WallpaperPicker::Options(), pluginManager) {
}

PanelPlacement WallpaperPanel::panelPlacement() const noexcept {
  return m_config == nullptr ? PanelPlacement::Attached : m_config->config().shell.panel.wallpaperPlacement;
}

void WallpaperPanel::setAnimationManager(AnimationManager* mgr) noexcept {
  Panel::setAnimationManager(mgr);
  m_picker.setAnimationManager(mgr);
}

void WallpaperPanel::create() {
  m_picker.setContentScale(contentScale());
  m_picker.setPanelCardOpacity(panelCardOpacity());
  m_picker.setAnimationManager(m_animations);
  setRoot(m_picker.create());
}

void WallpaperPanel::onOpen(std::string_view context) { m_picker.onOpen(context); }

void WallpaperPanel::onClose() { m_picker.onClose(); }

bool WallpaperPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  return m_picker.handleGlobalKey(sym, modifiers, pressed, preedit);
}

InputArea* WallpaperPanel::initialFocusArea() const { return m_picker.initialFocusArea(); }

void WallpaperPanel::doLayout(Renderer& renderer, float width, float height) {
  m_picker.setContentScale(contentScale());
  m_picker.layout(renderer, width, height);
}

void WallpaperPanel::doUpdate(Renderer& renderer) { m_picker.update(renderer); }

void WallpaperPanel::onPanelCardOpacityChanged(float opacity) { m_picker.setPanelCardOpacity(opacity); }
