#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"
#include "render/core/thumbnail_service.h"
#include "shell/control_center/control_center_services.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/shortcut_services.h"
#include "shell/control_center/tab.h"
#include "ui/controls/scroll_view.h"
#include "ui/signal.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class AccountsService;
class AsyncTextureCache;
class Button;
class CalendarService;
class CompositorPlatform;
class HttpClient;
class IpcService;
class ConfigService;
class DependencyService;
class Glyph;
class Image;
class InputArea;
class Label;
class BrightnessService;
class PipeWireService;
class SessionActionRunner;
class ProgressBar;
class ScrollView;
class Shortcut;
class Slider;
class Wallpaper;
class ClipboardService;
class UPowerService;

struct ShortcutPad {
  // Survives HomeTab::onClose(); button/glyph/label are nulled with the scene.
  std::unique_ptr<Shortcut> shortcut;
  std::string type; // config `type` this pad was created from
  Button* button = nullptr;
  Glyph* glyph = nullptr;
  Label* label = nullptr;
};

class HomeTab : public Tab {
public:
  explicit HomeTab(const ControlCenterServices& services);
  ~HomeTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onFrameTick(float deltaMs) override;
  void setActive(bool active) override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void layoutWallpaperBackground(Renderer& renderer);
  // The card fill is only the backdrop for an absent wallpaper: while one covers the card it is
  // cleared, so the card's rounded edge is the wallpaper's own and nothing shows around it.
  void syncUserCardFill();
  // Only the topmost ready wallpaper layer draws. Two stacked rounded images would each
  // antialias the same edge, hardening it; the placeholder steps aside once the crisp layer is
  // fully faded in.
  void syncWallpaperLayerVisibility();
  // Adds a card overlay for pointer and/or keyboard activation.
  struct CardOverlayOptions {
    bool keyboardFocus = true;
    bool pointerHitTest = true;
  };
  InputArea* addCardOverlay(Flex& card, std::function<void()> onActivate);
  InputArea* addCardOverlay(Flex& card, std::function<void()> onActivate, CardOverlayOptions options);
  void layoutCardOverlays();
  void syncWallpaperBackground(Renderer& renderer);
  void ensureWallpaperThumbnail(const std::string& path, int targetPx);
  void startCrispFade();
  void cancelCrispFade();
  void sync(Renderer& renderer);
  void warnOnOversizedAvatarSource(const std::string& path);
  void syncScaledFonts();
  void syncShortcuts();
  void syncHeaderActions();
  void syncStatusStrip();
  void buildQuickSurface(Flex& tab, float scale);
  void buildPinList(Flex& tab, float scale);
  void syncQuickControls();
  void runSessionAction(std::string_view action);
  bool resizeMediaArtToCard();
  [[nodiscard]] bool mediaExpanded() const;
  void onPanelCardOpacityChanged(float opacity) override;

  struct QuickToggleRow {
    std::unique_ptr<Shortcut> shortcut;
    Button* button = nullptr;
    Label* status = nullptr;
    std::string openTab;
  };

  MprisService* m_mpris = nullptr;
  PipeWireService* m_audio = nullptr;
  BrightnessService* m_brightness = nullptr;
  SessionActionRunner* m_session = nullptr;
  HttpClient* m_httpClient = nullptr;
  WeatherService* m_weather = nullptr;
  CalendarService* m_calendar = nullptr;
  ConfigService* m_config = nullptr;
  AccountsService* m_accounts = nullptr;
  Wallpaper* m_wallpaper = nullptr;
  ThumbnailService* m_thumbnails = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;
  UPowerService* m_upower = nullptr;
  ShortcutServices m_services;
  std::unique_ptr<SettingsPane> m_settings;
  bool m_active = false;

  Flex* m_rootLayout = nullptr;
  ScrollView* m_liveScroll = nullptr;
  ScrollViewState m_liveScrollState{};
  Flex* m_statusStrip = nullptr;
  Flex* m_dateTimeCard = nullptr;
  Flex* m_mediaCard = nullptr;
  Flex* m_mediaText = nullptr;
  Flex* m_userCard = nullptr;
  Flex* m_userMain = nullptr;
  InputArea* m_userAvatarArea = nullptr;
  Image* m_userAvatar = nullptr;

  Label* m_timeLabel = nullptr;
  Label* m_dateLabel = nullptr;
  Flex* m_weatherRow = nullptr;
  Glyph* m_weatherGlyph = nullptr;
  Label* m_weatherLine = nullptr;
  Flex* m_eventRow = nullptr;
  Glyph* m_eventGlyph = nullptr;
  Label* m_eventLine = nullptr;
  Label* m_userHost = nullptr;
  Label* m_userUptime = nullptr;
  Button* m_sessionButton = nullptr;
  Button* m_statusUpdates = nullptr;
  Button* m_statusAi = nullptr;
  Button* m_statusBattery = nullptr;
  Button* m_statusCapture = nullptr;
  Button* m_statusNightLight = nullptr;
  Button* m_statusCaffeine = nullptr;
  Button* m_statusDnd = nullptr;
  Button* m_statusLocalSend = nullptr;
  Button* m_statusPhone = nullptr;
  Button* m_statusPrivacy = nullptr;
  Button* m_statusCast = nullptr;
  Button* m_statusFirewall = nullptr;
  InputArea* m_userCardKeyboardArea = nullptr;
  InputArea* m_userCardArea = nullptr;
  InputArea* m_mediaCardArea = nullptr;
  InputArea* m_dateTimeCardArea = nullptr;
  InputArea* m_weatherCardArea = nullptr;
  // Survives onClose() so the oversized-source warning fires once per session.
  std::string m_sizeCheckedAvatarPath;

  // Two stacked layers: m_wallpaperPlaceholder shows the resident full-screen
  // wallpaper texture immediately (slightly soft), m_wallpaperBg holds the crisp
  // card-sized thumbnail and crossfades in over it once decoded.
  Image* m_wallpaperPlaceholder = nullptr;
  Image* m_wallpaperBg = nullptr;
  std::string m_loadedWallpaperPath;
  int m_loadedWallpaperSize = 0;
  std::string m_crispWorkingPath;
  int m_crispWorkingSize = 0;
  bool m_crispShown = false;
  bool m_crispNeedsFade = false;
  bool m_crispOpaque = false;
  bool m_placeholderReady = false;
  std::uint32_t m_wallpaperCrispAnimId = 0;
  ThumbnailService::Subscription m_thumbnailPendingSub;
  Signal<>::ScopedConnection m_wallpaperChangedConn;

  Label* m_mediaTrack = nullptr;
  Label* m_mediaArtist = nullptr;
  Flex* m_mediaTransport = nullptr;
  Button* m_mediaPrev = nullptr;
  Button* m_mediaPlayPause = nullptr;
  Button* m_mediaNext = nullptr;
  ProgressBar* m_mediaProgressBar = nullptr;
  Flex* m_mediaArtSlot = nullptr;
  Glyph* m_mediaArtFallback = nullptr;
  Image* m_mediaArt = nullptr;
  std::string m_loadedMediaArtUrl;
  std::unordered_set<std::string> m_pendingArtDownloads;
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
  std::string m_mediaPositionBusName;
  std::string m_mediaPositionTrackId;
  std::string m_mediaPositionTrackSignature;
  std::string m_mediaLastPlaybackStatus;
  std::int64_t m_mediaPositionUs = 0;
  std::chrono::steady_clock::time_point m_mediaPositionSampleAt;
  std::chrono::steady_clock::time_point m_nextRealtimeUpdateAt;
  std::chrono::steady_clock::time_point m_lastRealtimeMprisPollAt;
  Timer m_progressTimer;
  // Keeps the home tab clock ticking while the panel is open and idle; the clock
  // otherwise only refreshes when an unrelated service forces a redraw.
  Timer m_clockTimer;

  Flex* m_shortcutsColumn = nullptr;
  std::vector<ShortcutPad> m_shortcutPads;
  std::vector<QuickToggleRow> m_quickToggles;
  std::unique_ptr<Shortcut> m_volumeShortcut;
  Slider* m_volumeSlider = nullptr;
  Slider* m_brightnessSlider = nullptr;
  Label* m_volumeValue = nullptr;
  Label* m_brightnessValue = nullptr;
  Button* m_volumeButton = nullptr;
  Flex* m_brightnessRow = nullptr;
  Flex* m_sessionWell = nullptr;
  Button* m_lockButton = nullptr;
  Button* m_suspendButton = nullptr;
  bool m_syncingQuickSliders = false;

  // Drag-to-reorder for the optional pins (Quick Settings / Control Center edit gesture):
  // press a pin and move past the drag threshold to lift it; neighbours slide aside to show
  // the drop slot; release commits the new order to `control_center.shortcuts`.
  struct PinDrag {
    bool pressed = false;       // left button held on `source`
    bool active = false;        // moved past the threshold; pin is lifted
    bool suppressClick = false; // swallow the click that follows a drag release
    std::size_t source = 0;
    std::size_t target = 0;     // insertion slot in [0, count]
    float startPointerY = 0.0F; // pointer in column coords at press
    float pointerY = 0.0F;      // latest pointer in column coords
    std::vector<float> restY;   // each pin's laid-out y at lift time
  };
  PinDrag m_pinDrag;
  void onPinPress(std::size_t index, float localY, bool pressed);
  void onPinMotion(std::size_t index, float localY);
  void liftPin();
  void applyPinDragVisuals();
  void clearPinDragVisuals();
  void commitPinDrag();
  // Moves the pad and its row so the open panel shows the new order without a rebuild.
  void movePinInPlace(std::size_t source, std::size_t slot);
  [[nodiscard]] std::size_t pinDropSlot() const;
  [[nodiscard]] std::size_t padIndexForButton(const Button* button) const noexcept;
  [[nodiscard]] ShortcutPad* padForButton(const Button* button) noexcept;
};
