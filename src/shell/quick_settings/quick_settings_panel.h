#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"
#include "shell/control_center/shortcut_services.h"
#include "shell/panel/panel.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class Button;
class ConfigService;
class Flex;
class Label;
class Renderer;
class ScrollView;
class Shortcut;
class Slider;
class BrightnessService;
class PipeWireService;
class SessionActionRunner;

// Apple-split status surfaces: Control Center (Quick Settings) or Notification Center.
// Mode comes from PanelOpenRequest::context. Defaults to a floating popover under the
// clicked bar widget; may attach to the top bar when configured.
class QuickSettingsPanel : public Panel {
public:
  enum class ShadeMode { QuickSettings, Notifications };

  QuickSettingsPanel(
      ConfigService* config, ShortcutServices shortcuts, PipeWireService* audio, BrightnessService* brightness,
      SessionActionRunner* session
  );
  ~QuickSettingsPanel() override;

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void onConfigReloaded() override;

  [[nodiscard]] bool isContextActive(std::string_view context) const override;
  [[nodiscard]] bool requiresRecreateForContext(std::string_view context) const override;
  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;
  [[nodiscard]] float cornerRadius() const noexcept override;
  [[nodiscard]] std::string_view preferredAttachedBarPosition() const noexcept override { return "top"; }

private:
  struct ToggleTile {
    std::unique_ptr<Shortcut> shortcut;
    Flex* cell = nullptr;
    Button* iconButton = nullptr;
    Button* titleButton = nullptr;
    Button* menuButton = nullptr;
    Label* status = nullptr;
    std::string settingsContext;
    std::string label;
    std::string icon;
    std::string statusText;
    ShortcutSize size = ShortcutSize::Small;
    bool active = false;
  };

  struct TileDrag {
    bool armed = false;
    bool active = false;
    bool suppressClick = false;
    std::size_t sourceIndex = 0;
    std::size_t targetIndex = 0;
    float startX = 0.0F;
    float startY = 0.0F;
    Timer holdTimer;
  };

  // Wide enough that two-column tiles keep full titles (Bluetooth, Dark Mode) at elevated ui_scale.
  static constexpr float kQsColumnWidth = 420.0F;
  static constexpr float kNotifColumnWidth = 420.0F;

  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;

  // Ids that were unread when Notification Center opened, so the dot stays
  // visible after history is marked seen (which clears the bar badge).
  std::unordered_set<std::uint32_t> m_unreadAtOpen;

  void buildTiles();
  void populateTileGrid();
  void rebuildNotificationList();
  void syncSliders();
  void syncTiles();
  void syncMedia();
  void syncNotifications();
  void syncSessionVisibility();
  void applyTileStyle(ToggleTile& tile);
  void wireTileReorder(ToggleTile& tile, std::size_t index);
  void beginTileDrag(std::size_t index, float localX, float localY);
  void updateTileDrag(std::size_t index, float localX, float localY);
  void endTileDrag(bool commit);
  void resetTileDrag();
  void commitTileReorder(std::size_t from, std::size_t to);
  void refreshAfterTileReorder();
  [[nodiscard]] std::vector<ShortcutConfig> resolvedShortcutOrder() const;
  [[nodiscard]] std::optional<std::size_t> tileIndexAtScene(float sceneX, float sceneY) const;
  [[nodiscard]] bool hasBrightness() const;
  [[nodiscard]] bool hasMicrophone() const;
  [[nodiscard]] bool hasMedia() const;
  [[nodiscard]] ShadeMode activeShadeMode() const;
  [[nodiscard]] static ShadeMode shadeModeForContext(std::string_view context);
  [[nodiscard]] static std::string statusCaption(const Shortcut& shortcut);
  [[nodiscard]] static std::string tileTitle(const Shortcut& shortcut);
  [[nodiscard]] static const char* settingsContextForType(std::string_view type);

  ConfigService* m_config;
  ShortcutServices m_shortcuts;
  PipeWireService* m_audio;
  BrightnessService* m_brightness;
  SessionActionRunner* m_session;

  Flex* m_root = nullptr;
  Flex* m_notifColumn = nullptr;
  Flex* m_notifList = nullptr;
  Flex* m_qsColumn = nullptr;
  Flex* m_tileGrid = nullptr;
  Flex* m_mediaWell = nullptr;
  Flex* m_micRow = nullptr;
  Flex* m_brightnessRow = nullptr;
  Flex* m_sessionWell = nullptr;
  Label* m_notifEmpty = nullptr;
  Label* m_mediaTitle = nullptr;
  Label* m_mediaArtist = nullptr;
  Button* m_mediaPrev = nullptr;
  Button* m_mediaPlayPause = nullptr;
  Button* m_mediaNext = nullptr;
  Button* m_volumeButton = nullptr;
  Button* m_micButton = nullptr;
  Button* m_settingsButton = nullptr;
  Button* m_lockButton = nullptr;
  Button* m_suspendButton = nullptr;
  Button* m_clearNotifications = nullptr;
  ScrollView* m_notifScroll = nullptr;
  Slider* m_volume = nullptr;
  Slider* m_mic = nullptr;
  Slider* m_brightnessSlider = nullptr;
  std::unique_ptr<Shortcut> m_volumeShortcut;
  std::unique_ptr<Shortcut> m_micShortcut;
  std::vector<ToggleTile> m_tiles;
  std::vector<Button*> m_footerButtons;
  TileDrag m_tileDrag{};
  ShadeMode m_mode = ShadeMode::QuickSettings;
  std::uint64_t m_notificationSerial = 0;
  bool m_syncingSliders = false;
};
