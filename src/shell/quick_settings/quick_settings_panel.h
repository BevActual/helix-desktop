#pragma once

#include "shell/control_center/shortcut_services.h"
#include "shell/panel/panel.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

class Button;
class ConfigService;
class Flex;
class Glyph;
class Renderer;
class Shortcut;
class Slider;
class BrightnessService;
class PipeWireService;
class SessionActionRunner;

// GNOME-style quick settings popover: volume and brightness sliders, a grid of toggle
// tiles built from the shortcut registry, and a row for settings, lock and power.
class QuickSettingsPanel : public Panel {
public:
  QuickSettingsPanel(
      ConfigService* config, ShortcutServices shortcuts, PipeWireService* audio, BrightnessService* brightness,
      SessionActionRunner* session
  );
  ~QuickSettingsPanel() override;

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override { return PanelPlacement::Attached; }
  [[nodiscard]] std::string_view preferredAttachedBarPosition() const noexcept override { return "top"; }

private:
  struct Tile {
    std::unique_ptr<Shortcut> shortcut;
    Button* button = nullptr;
    std::string label;
    std::string icon;
    bool active = false;
  };

  static constexpr float kWidth = 360.0F;
  static constexpr float kPad = 16.0F;
  static constexpr float kGap = 8.0F;
  static constexpr float kRow = 44.0F;
  static constexpr float kTile = 56.0F;

  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void buildTiles();
  void syncTiles();
  void syncSliders();
  [[nodiscard]] std::vector<std::string> tileTypes() const;
  [[nodiscard]] bool hasBrightness() const;

  ConfigService* m_config;
  ShortcutServices m_shortcuts;
  PipeWireService* m_audio;
  BrightnessService* m_brightness;
  SessionActionRunner* m_session;

  Flex* m_root = nullptr;
  Slider* m_volume = nullptr;
  Glyph* m_volumeGlyph = nullptr;
  Slider* m_brightnessSlider = nullptr;
  std::vector<Tile> m_tiles;
  std::vector<Button*> m_footerButtons;
  std::chrono::steady_clock::time_point m_volumeTouched{};
  std::chrono::steady_clock::time_point m_brightnessTouched{};
};
