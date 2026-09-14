#pragma once

#include "shell/panel/panel.h"
#include "shell/wallpaper/panel/wallpaper_picker.h"

class ConfigService;
class HttpClient;
class ThumbnailService;
class WallpaperScanner;
class WaylandConnection;

namespace noctalia::theme {
  class ThemeService;
}
namespace scripting {
  class PluginManager;
}

class WallpaperPanel : public Panel {
public:
  WallpaperPanel(
      WaylandConnection* wayland, ConfigService* config, ThumbnailService* thumbnails, WallpaperScanner* scanner,
      noctalia::theme::ThemeService* themeService = nullptr, HttpClient* httpClient = nullptr,
      scripting::PluginManager* pluginManager = nullptr
  );

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

  [[nodiscard]] float preferredWidth() const override { return scaled(980.0F); }
  [[nodiscard]] float preferredHeight() const override { return scaled(700.0F); }
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  void setAnimationManager(AnimationManager* mgr) noexcept override;

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;

  ConfigService* m_config = nullptr;
  WallpaperPicker m_picker;
};
