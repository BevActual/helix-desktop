#include "shell/control_center/control_center_panel.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "net/localsend_service.h"
#include "net/tailscale_service.h"
#include "notification/notification_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/control_center/settings_search_target.h"
#include "shell/control_center/tab_search.h"
#include "shell/control_center/tabs/desktop_tab.h"
#include "shell/control_center/hub_tab.h"
#include "shell/control_center/tabs/launcher_tab.h"
#include "shell/control_center/tabs/ai_usage_tab.h"
#include "shell/control_center/tabs/calendar_tab.h"
#include "shell/control_center/tabs/camera_tab.h"
#include "shell/control_center/tabs/clock_tab.h"
#include "shell/control_center/tabs/clipboard_tab.h"
#include "shell/control_center/tabs/home_tab.h"
#include "shell/control_center/tabs/keybinds_tab.h"
#include "shell/control_center/tabs/media_tab.h"
#include "shell/control_center/tabs/monitor_tab.h"
#include "shell/control_center/tabs/capture_tab.h"
#include "shell/control_center/tabs/power_tab.h"
#include "shell/control_center/tabs/settings_tab.h"
#include "shell/control_center/tabs/native_redirect_tab.h"
#include "shell/control_center/tabs/firewall_tab.h"
#include "shell/control_center/tabs/printers_tab.h"
#include "shell/control_center/tabs/privacy_tab.h"
#include "shell/control_center/tabs/templates_tab.h"
#include "shell/control_center/tabs/wallpaper_tab.h"
#include "shell/control_center/tabs/weather_tab.h"
#include "shell/wallpaper/video_wallpaper.h"
#include "shell/wallpaper/wallhaven_client.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "shell/settings/settings_registry.h"
#include "shell/panel/panel_manager.h"
#include "wayland/wayland_seat.h"
#include "system/ai_usage_service.h"
#include "system/dependency_service.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"
#include "desktop/settings_routes.h"
#include "system/easyeffects_service.h"
#include "system/package_kit_service.h"
#include "system/screen_time_service.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/roving_list_nav.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/separator.h"
#include "ui/palette.h"
#include "ui/scroll_into_view.h"
#include "ui/split_pane_focus.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <wayland-client-protocol.h>

using namespace control_center;

namespace {

  constexpr Logger kLog("control-center");
  constexpr auto kMprisRefreshMinInterval = std::chrono::milliseconds(750);
  constexpr float kTabViewportClipInset = 1.0F;
  constexpr float kScrollEdgeFadePx = 28.0F;

  struct NativeDestination {
    std::string_view application;
    std::string_view page;
  };

  std::optional<NativeDestination> nativeDestinationForTab(std::string_view tab) {
    if (tab == "system") return NativeDestination{"helix-system-monitor", "overview"};
    if (tab == "screen-time") return NativeDestination{"helix-system-monitor", "screen-time"};
    if (tab == "audio") return NativeDestination{"helix-settings", "sound"};
    if (tab == "phone" || tab == "localsend" || tab == "syncthing" || tab == "cast")
      return NativeDestination{"helix-phone", ""};
    if (tab == "network" || tab == "tailscale") return NativeDestination{"helix-settings", "network"};
    if (tab == "bluetooth") return NativeDestination{"helix-settings", "bluetooth"};
    if (tab == "theme" || tab == "wallpaper") return NativeDestination{"helix-settings", "appearance"};
    if (tab == "privacy-security") return NativeDestination{"helix-settings", "privacy-security"};
    if (tab == "notifications") return NativeDestination{"helix-settings", "notifications"};
    if (tab == "power") return NativeDestination{"helix-settings", "power"};
    if (tab == "desktop" || tab == "launcher" || tab == "control-center")
      return NativeDestination{"helix-settings", "desktop-dock"};
    return std::nullopt;
  }

  void styleScrollEdgeFade(Box& box, ColorRole role, float opacity, bool fromTop) {
    const Color opaque = resolveColorSpec(colorSpecFromRole(role, opacity));
    Color clear = opaque;
    clear.a = 0.0F;
    Color mid = opaque;
    mid.a *= 0.42F;
    Color tail = opaque;
    tail.a *= 0.12F;
    RoundedRectStyle style;
    style.fillMode = FillMode::LinearGradient;
    style.gradientDirection = GradientDirection::Vertical;
    style.softness = 0.0F;
    style.radius = 0.0F;
    if (fromTop) {
      style.gradientStops = {
          GradientStop{.position = 0.0F, .color = opaque},
          GradientStop{.position = 0.35F, .color = mid},
          GradientStop{.position = 0.78F, .color = tail},
          GradientStop{.position = 1.0F, .color = clear},
      };
    } else {
      style.gradientStops = {
          GradientStop{.position = 0.0F, .color = clear},
          GradientStop{.position = 0.22F, .color = tail},
          GradientStop{.position = 0.65F, .color = mid},
          GradientStop{.position = 1.0F, .color = opaque},
      };
    }
    box.setStyle(style);
    box.setHitTestVisible(false);
  }

  float tabContentHeight(float viewportHeight) {
    return std::max(1.0F, std::floor(viewportHeight - kTabViewportClipInset));
  }

  using TabGroup = ControlCenterPanel::TabGroup;

  struct BuiltinTabSpec {
    const char* id;
    const char* titleKey;
    const char* glyph;
    bool hideable;
    TabGroup group;
  };

  // Sidebar order and clusters (docs/design/helix-hig.md, Settings structure). Live "app"
  // pages sit in the Apps cluster near the bottom, like third-party panes on macOS.
  constexpr BuiltinTabSpec kBuiltinTabs[] = {
      {"home", "control-center.tabs.home", "home", false, TabGroup::Home},
      {"network", "control-center.tabs.network", "wifi", true, TabGroup::Connectivity},
      {"bluetooth", "control-center.tabs.bluetooth", "bluetooth", true, TabGroup::Connectivity},
      {"tailscale", "control-center.tabs.tailscale", "circles", true, TabGroup::Connectivity},
      {"phone", "control-center.tabs.phone", "device-mobile", true, TabGroup::Connectivity},
      {"notifications", "control-center.tabs.notifications", "bell", true, TabGroup::Attention},
      {"audio", "control-center.tabs.audio", "volume", true, TabGroup::Attention},
      {"screen-time", "control-center.tabs.screen-time", "hourglass", true, TabGroup::Attention},
      {"theme", "settings.navigation.sections.theme", "palette", true, TabGroup::System},
      {"control-center", "settings.navigation.sections.control-center", "adjustments", true, TabGroup::System},
      {"launcher", "settings.navigation.sections.launcher", "rocket", true, TabGroup::System},
      {"privacy-security", "control-center.tabs.privacy-security", "shield-lock", true, TabGroup::System},
      {"system", "control-center.tabs.system", "activity-heartbeat", true, TabGroup::System},
      {"packages", "control-center.tabs.packages", "package", true, TabGroup::System},
      {"clock", "control-center.tabs.clock", "clock", true, TabGroup::System},
      {"desktop", "settings.navigation.sections.desktop", "layout-board", true, TabGroup::Desktop},
      {"monitor", "control-center.tabs.monitor", "device-desktop", true, TabGroup::Desktop},
      {"wallpaper", "settings.navigation.sections.wallpaper", "paint", true, TabGroup::Desktop},
      {"power", "control-center.tabs.power", "battery-charging-2", true, TabGroup::Desktop},
      {"keybinds", "settings.navigation.sections.keybinds", "keyboard", true, TabGroup::Devices},
      {"shell", "settings.navigation.sections.shell", "app-window", true, TabGroup::Devices},
      {"printers", "control-center.tabs.printers", "printer", true, TabGroup::Devices},
      {"camera", "control-center.tabs.camera", "camera", true, TabGroup::Devices},
      {"localsend", "control-center.tabs.localsend", "share", true, TabGroup::Sharing},
      {"syncthing", "control-center.tabs.syncthing", "folder", true, TabGroup::Sharing},
      {"cast", "control-center.tabs.cast", "screen-share", true, TabGroup::Sharing},
      {"calendar", "control-center.tabs.calendar", "calendar-event", true, TabGroup::Apps},
      {"weather", "control-center.tabs.weather", "weather-cloud-sun", true, TabGroup::Apps},
      {"media", "control-center.tabs.media", "disc-filled", true, TabGroup::Apps},
      {"capture", "control-center.tabs.capture", "screenshot", true, TabGroup::Apps},
      {"clipboard", "control-center.tabs.clipboard", "clipboard", true, TabGroup::Apps},
      {"ai", "control-center.tabs.ai", "sparkles", true, TabGroup::Apps},
      {"templates", "settings.navigation.sections.templates", "color-swatch", true, TabGroup::Apps},
      {"hooks", "settings.navigation.sections.hooks", "link", true, TabGroup::Advanced},
      {"niri", "settings.navigation.sections.niri", "niri", true, TabGroup::Advanced},
  };


  // Sidebar groups that host existing tabs as sub-tabs (HubTab). Child ids stay valid open
  // contexts everywhere (`panel-open control-center audio`, Home links, search); the panel
  // resolves them to the hub and selects the child.
  struct HubChildSpec {
    const char* childId;
    const char* hubId;
    const char* titleKey;
    const char* glyph;
  };

  constexpr HubChildSpec kHubChildren[] = {
      {"privacy", "privacy-security", "control-center.tabs.privacy", "eye-off"},
      {"firewall", "privacy-security", "control-center.tabs.firewall", "flame"},
      {"security", "privacy-security", "settings.navigation.sections.security", "lock"},
  };

  [[nodiscard]] std::optional<std::string_view> hubIdForChild(std::string_view id) {
    for (const auto& spec : kHubChildren) {
      if (id == spec.childId) {
        return spec.hubId;
      }
    }
    return std::nullopt;
  }

  // Sidebar icon color per tab, the way macOS sidebars carry a tint per section: appearance tabs
  // take the accent itself, connectivity blue, power green, alerts red, media pink, tools yellow.
  // Fixed hues are harmonized toward the palette primary. Advanced and plain utilities stay neutral.
  std::optional<ColorSpec> sidebarTabTint(std::string_view id, TabGroup group) {
    constexpr Color kBlue = rgbHex(0x0A84FF);
    constexpr Color kGreen = rgbHex(0x30D158);
    constexpr Color kOrange = rgbHex(0xFF9F0A);
    constexpr Color kRed = rgbHex(0xFF453A);
    constexpr Color kPink = rgbHex(0xFF375F);
    constexpr Color kIndigo = rgbHex(0x5E5CE6);
    constexpr Color kPurple = rgbHex(0xBF5AF2);
    constexpr Color kCyan = rgbHex(0x64D2FF);
    constexpr Color kYellow = rgbHex(0xFFD60A);
    const auto fixed = [](const Color& c) { return fixedColorSpec(harmonizeToPrimary(c)); };
    const auto neutral = colorSpecFromRole(ColorRole::OnSurfaceVariant);

    if (group == TabGroup::Home || id == "theme" || id == "wallpaper" || id == "templates") {
      return colorSpecFromRole(ColorRole::Primary);
    }
    if (id == "desktop") {
      return colorSpecFromRole(ColorRole::Tertiary);
    }
    if (id == "network" || id == "bluetooth" || id == "tailscale" || id == "syncthing" || id == "localsend"
        || id == "cast" || id == "phone") {
      return fixed(kBlue);
    }
    if (id == "power" || id == "system") {
      return fixed(kGreen);
    }
    if (id == "audio" || id == "packages") {
      return fixed(kOrange);
    }
    if (id == "notifications" || id == "calendar") {
      return fixed(kRed);
    }
    if (id == "media" || id == "capture" || id == "launcher") {
      return fixed(kPink);
    }
    if (id == "screen-time" || id == "clock" || id == "privacy-security" || id == "privacy" || id == "firewall"
        || id == "security") {
      return fixed(kIndigo);
    }
    if (id == "ai") {
      return fixed(kPurple);
    }
    if (id == "monitor" || id == "weather") {
      return fixed(kCyan);
    }
    if (id == "clipboard") {
      return fixed(kYellow);
    }
    if (group == TabGroup::Apps) {
      return colorSpecFromRole(ColorRole::Secondary); // the rest of the app catalog
    }
    return neutral;
  }


  const BuiltinTabSpec* specById(std::string_view id) {
    for (const auto& spec : kBuiltinTabs) {
      if (id == spec.id) {
        return &spec;
      }
    }
    return nullptr;
  }

  std::string builtinTitle(const BuiltinTabSpec& spec) { return i18n::tr(spec.titleKey); }

  constexpr std::string_view kSettingsTabPrefix = "settings:";

  struct SettingsOpenTarget {
    bool isSettings = false;
    std::string section;
  };

  SettingsOpenTarget parseSettingsOpenContext(std::string_view context) {
    if (context == "settings") {
      return {.isSettings = true};
    }
    if (context.starts_with(kSettingsTabPrefix)) {
      return {.isSettings = true, .section = std::string(context.substr(kSettingsTabPrefix.size()))};
    }
    return {};
  }

  bool isSettingsEditorTab(std::string_view id) {
    return id == "shell"
        || id == "security"
        || id == "hooks"
        || id == "niri"
        || id == "control-center";
  }

  bool isLiveSettingsHost(std::string_view id) {
    return id == "theme"
        || id == "glass"
        || id == "icons"
        || id == "type"
        || id == "scale"
        || id == "roundness"
        || id == "effects"
        || id == "weather"
        || id == "notifications"
        || id == "power"
        || id == "system"
        || id == "packages"
        || id == "calendar"
        || id == "clock"
        || id == "audio"
        || id == "privacy-security"
        || id == "privacy"
        || id == "monitor"
        || id == "wallpaper"
        || id == "templates"
        || id == "keybinds"
        || id == "media"
        || id == "screen-time"
        || id == "ai"
        || id == "network"
        || id == "bluetooth"
        || id == "capture"
        || id == "clipboard"
        || id == "tailscale"
        || id == "localsend"
        || id == "phone"
        || id == "syncthing"
        || id == "firewall"
        || id == "printers"
        || id == "cast"
        || id == "privacy"
        || id == "home"
        || id == "desktop"
        || id == "camera"
        || id == "launcher";
  }

  [[nodiscard]] std::string resolveOpenTabId(std::string_view context) {
    if (context == "appearance"
        || context == "glass"
        || context == "icons"
        || context == "type"
        || context == "scale"
        || context == "roundness"
        || context == "effects"
        || context == "language"
        || context == "chrome"
        || context == "palettes") {
      return "theme";
    }
    if (context == "home") {
      return "home";
    }
    if (context.empty()) {
      return "theme";
    }
    if (context == "screenshot" || context == "screen-record" || context == "capture") {
      return "capture";
    }
    if (context == "ai" || context == "ai-usage") {
      return "ai";
    }
    if (context == "widgets"
        || context == "corners"
        || context == "hot-corners"
        || context == "screen-corners"
        || context == "hot_corners"
        || context == "screen_corners"
        || context == "bar"
        || context == "dock"
        || context == "windows"
        || context == "panels"
        || context == "osd") {
      return "desktop";
    }
    if (context == "wallpaper"
        || context.starts_with("wallpaper ")
        || context == "daily"
        || context == "bing"
        || context == "nasa") {
      return "wallpaper";
    }
    return std::string(context);
  }

  [[nodiscard]] std::string tabIdForOpenContext(std::string_view context) {
    const auto settingsTarget = parseSettingsOpenContext(context);
    if (settingsTarget.isSettings) {
      return tabIdForSettingsSection(settingsTarget.section);
    }
    return resolveOpenTabId(context);
  }

  [[nodiscard]] std::string wallpaperPickerContext(std::string_view context) {
    constexpr std::string_view kPrefix = "wallpaper ";
    if (context.starts_with(kPrefix)) {
      return StringUtils::trim(context.substr(kPrefix.size()));
    }
    if (context == "daily" || context == "bing" || context == "nasa") {
      return std::string(context);
    }
    return {};
  }

  [[nodiscard]] bool wallpaperContextMatches(WallpaperPicker::BrowseSource source, std::string_view context) {
    if (context.empty()) {
      return true;
    }
    if (context == "wallhaven") {
      return source == WallpaperPicker::BrowseSource::Wallhaven;
    }
    if (context == "video" || context == "mpvpaper") {
      return source == WallpaperPicker::BrowseSource::Video;
    }
    if (context == "daily" || context == "bing" || context == "nasa") {
      return source == WallpaperPicker::BrowseSource::Daily;
    }
    return true;
  }

  [[nodiscard]] std::optional<CaptureTab::View> captureViewForContext(std::string_view context) {
    if (context == "screenshot") {
      return CaptureTab::View::Screenshot;
    }
    if (context == "screen-record") {
      return CaptureTab::View::Record;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<DesktopTab::Section> desktopSectionForContext(std::string_view context) {
    if (context == "widgets"
        || context == "corners"
        || context == "hot-corners"
        || context == "screen-corners"
        || context == "hot_corners"
        || context == "screen_corners"
        || context == "bar"
        || context == "dock"
        || context == "windows"
        || context == "panels"
        || context == "osd") {
      return DesktopTab::sectionFromId(context);
    }
    return std::nullopt;
  }

} // namespace

ControlCenterPanel::ControlCenterPanel(const ControlCenterServices& services) {
  m_config = services.config;
  m_aiUsage = services.aiUsage;
  m_mpris = services.mpris;
  m_notificationManager = services.notifications;
  m_dependencies = services.dependencies;
  m_fileWatcher = services.fileWatcher;
  m_httpClient = services.httpClient;
  m_clipboard = services.clipboard;
  m_platform = services.platform;
  m_asyncTextures = services.asyncTextures;
  ControlCenterServices tabServices = services;
  tabServices.modalHost = &m_modalHost;
  appendBuiltinSlots(tabServices);
}

void ControlCenterPanel::appendBuiltinSlots(const ControlCenterServices& services) {
  WaylandConnection* wayland = services.platform != nullptr ? &services.platform->wayland() : nullptr;

  auto add = [this](std::string_view id, std::unique_ptr<Tab> tab) {
    const auto* spec = specById(id);
    if (spec == nullptr) {
      return;
    }
    TabSlot slot;
    slot.id = spec->id;
    slot.title = builtinTitle(*spec);
    slot.glyph = spec->glyph;
    slot.tab = std::move(tab);
    slot.hideable = spec->hideable;
    slot.group = spec->group;
    m_slots.push_back(std::move(slot));
  };

  auto hubChild = [](std::string_view id, std::unique_ptr<Tab> tab) {
    for (const auto& spec : kHubChildren) {
      if (id == spec.childId) {
        return HubTab::Child{
            .id = spec.childId, .title = i18n::tr(spec.titleKey), .glyph = spec.glyph, .tab = std::move(tab)
        };
      }
    }
    return HubTab::Child{.id = std::string(id), .title = std::string(id), .glyph = {}, .tab = std::move(tab)};
  };

  add("home", std::make_unique<HomeTab>(services));

  add("network", std::make_unique<NativeRedirectTab>("helix-settings", "network", "control-center.tabs.network", services.platform));
  add("bluetooth", std::make_unique<NativeRedirectTab>("helix-settings", "bluetooth", "control-center.tabs.bluetooth", services.platform));
  add("tailscale", std::make_unique<NativeRedirectTab>("helix-settings", "network", "control-center.tabs.tailscale", services.platform));
  add("phone", std::make_unique<NativeRedirectTab>("helix-phone", "", "control-center.tabs.phone", services.platform));

  add("notifications", std::make_unique<NativeRedirectTab>("helix-settings", "notifications", "control-center.tabs.notifications", services.platform));
  add("audio", std::make_unique<NativeRedirectTab>("helix-settings", "sound", "control-center.tabs.audio", services.platform));
  add("screen-time", std::make_unique<NativeRedirectTab>("helix-system-monitor", "screen-time", "control-center.tabs.screen-time", services.platform));

  add("theme", std::make_unique<NativeRedirectTab>("helix-settings", "appearance", "settings.navigation.sections.theme", services.platform));
  add(
      "control-center",
      std::make_unique<SettingsTab>(
          services, "control-center", std::unordered_set<std::string>{"home", "layout", "navigation"}
      )
  );
  add("launcher", std::make_unique<LauncherTab>(services));
  {
    std::vector<HubTab::Child> privacySecurity;
    privacySecurity.push_back(hubChild("privacy", std::make_unique<PrivacyTab>(services)));
    privacySecurity.push_back(hubChild("firewall", std::make_unique<FirewallTab>(services)));
    privacySecurity.push_back(hubChild("security", std::make_unique<SettingsTab>(services, "security")));
    add("privacy-security", std::make_unique<HubTab>(std::move(privacySecurity)));
  }
  add("system", std::make_unique<NativeRedirectTab>("helix-system-monitor", "overview", "control-center.tabs.system", services.platform));
  add("clock", std::make_unique<ClockTab>(services));

  add("desktop", std::make_unique<DesktopTab>(services));
  add("monitor", std::make_unique<MonitorTab>(services.brightness, services.config, services.displayConfig, services));
  add("wallpaper", std::make_unique<WallpaperTab>(services));
  add("power", std::make_unique<PowerTab>(services.upower, services.powerProfiles, &services));

  add("keybinds", std::make_unique<KeybindsTab>(services));
  add(
      "shell",
      std::make_unique<SettingsTab>(
          services, "shell",
          std::unordered_set<std::string>{"general", "keyboard-layout", "window-switcher"}
      )
  );
  add("printers", std::make_unique<PrintersTab>(services));
  add("camera", std::make_unique<CameraTab>(services));

  add("localsend", std::make_unique<NativeRedirectTab>("helix-phone", "", "control-center.tabs.localsend", services.platform));
  add("syncthing", std::make_unique<NativeRedirectTab>("helix-phone", "", "control-center.tabs.syncthing", services.platform));
  add("cast", std::make_unique<NativeRedirectTab>("helix-phone", "", "control-center.tabs.cast", services.platform));

  add("calendar", std::make_unique<CalendarTab>(services.config, services.calendar, services));
  add("weather", std::make_unique<WeatherTab>(services.weather, services.config, services));
  add("media", std::make_unique<MediaTab>(services.mpris, services.httpClient, services.spectrum, services));
  add("capture", std::make_unique<CaptureTab>(services));
  add("clipboard", std::make_unique<ClipboardTab>(services));
  add("ai", std::make_unique<AiUsageTab>(services));
  add("templates", std::make_unique<TemplatesTab>(services));

  add("hooks", std::make_unique<SettingsTab>(services, "hooks"));
  add("niri", std::make_unique<SettingsTab>(services, "niri"));
}

float ControlCenterPanel::preferredWidth() const {
  const float fullSize = m_config != nullptr ? static_cast<float>(m_config->config().controlCenter.width)
                                             : static_cast<float>(ControlCenterConfig::kDefaultWidth);
  if (isQuickHomeContext(pendingOpenContext())) {
    constexpr float kQuickHomeWidth = 460.0F;
    return (fullSize < kQuickHomeWidth ? fullSize : kQuickHomeWidth) * m_contentScale;
  }
  switch (sidebarModeForOpen(pendingOpenContext())) {
  case ControlCenterSidebarMode::Full:
    return fullSize * m_contentScale;
  case ControlCenterSidebarMode::None:
    return fullSize * 0.75F * m_contentScale;
  default:
  case ControlCenterSidebarMode::Compact:
    return fullSize * 0.85F * m_contentScale;
  }
}

float ControlCenterPanel::preferredHeight() const {
  const float fullSize = m_config != nullptr ? static_cast<float>(m_config->config().controlCenter.height)
                                             : static_cast<float>(ControlCenterConfig::kDefaultHeight);
  if (isQuickHomeContext(pendingOpenContext())) {
    constexpr float kQuickHomeHeight = 526.0F;
    constexpr float kQuickHomeMediaExtra = 120.0F;
    constexpr float kQuickHomePinRow = 48.0F;
    float height = kQuickHomeHeight;
    if (m_mpris != nullptr && m_mpris->activePlayer().has_value()) {
      height += kQuickHomeMediaExtra;
    }
    if (m_config != nullptr) {
      height += static_cast<float>(m_config->config().controlCenter.shortcuts.size()) * kQuickHomePinRow;
    }
    return (fullSize < height ? fullSize : height) * m_contentScale;
  }
  return fullSize * m_contentScale;
}

PanelPlacement ControlCenterPanel::panelPlacement() const noexcept {
  return m_config == nullptr ? PanelPlacement::Attached : m_config->config().shell.panel.controlCenterPlacement;
}

float ControlCenterPanel::cornerRadius() const noexcept { return Style::scaledRadius3xl(contentScale()); }

bool ControlCenterPanel::dismissTransientUi() {
  if (m_modalHost.isOpen()) {
    m_modalHost.requestCloseTop();
    return true;
  }
  const auto* slot = slotById(m_activeTabId);
  return slot != nullptr && slot->tab != nullptr && slot->tab->dismissTransientUi();
}

void ControlCenterPanel::create() {
  const auto createdAt = std::chrono::steady_clock::now();
  const float scale = contentScale();
  const ControlCenterSidebarMode sidebarMode = sidebarModeForOpen(pendingOpenContext());
  m_compact = sidebarMode == ControlCenterSidebarMode::Compact;
  m_showSidebar = sidebarMode != ControlCenterSidebarMode::None;

  for (auto& slot : m_slots) {
    if (slot.tab != nullptr) {
      slot.tab->setContentScale(scale);
      slot.tab->setPanelCardOpacity(panelCardOpacity());
    }
    for (const auto& spec : kBuiltinTabs) {
      if (slot.id == spec.id) {
        slot.title = builtinTitle(spec);
        break;
      }
    }
  }

  auto rootLayout = ui::row({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .padding = 0.0F,
  });

  if (m_showSidebar) {
    // The sidebar is chrome, not a card: no fill or radius of its own, it runs the panel's full
    // height and a hairline separates it from the content (macOS 27's edge-to-edge sidebar).
    auto sidebar = ui::column({
        .out = &m_sidebar,
        .align = m_compact ? FlexAlign::Start : FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .padding = 0.0F,
        .fillWidth = false,
        .fillHeight = true,
    });

    auto sidebarScrollArea = ui::inputArea({});
    sidebarScrollArea->setParticipatesInLayout(false);
    sidebarScrollArea->setZIndex(-1);
    m_sidebarScrollArea = sidebarScrollArea.get();
    wireSidebarScroll(m_sidebarScrollArea);
    sidebar->addChild(std::move(sidebarScrollArea));
    if (m_compact) {
      // The compact rail is one icon wide, so the search field cannot live here. A search glyph
      // sits where the field would be and reveals the field under the content header.
      sidebar->addChild(
          ui::button({
              .out = &m_searchButton,
              .glyph = "search",
              .glyphSize = 21.0F * scale,
              .contentAlign = ButtonContentAlign::Center,
              .variant = ButtonVariant::Tab,
              .tooltip = i18n::tr("control-center.settings.search-placeholder"),
              .minWidth = Style::controlHeightSm * scale,
              .minHeight = Style::controlHeight * scale,
              .paddingV = Style::spaceXs * scale,
              .paddingH = Style::spaceXs * scale,
              .radius = Style::scaledRadiusXl(scale),
              .onClick = [this]() { revealSettingsSearch(); },
              .configure = [this](Button& button) { wireSidebarScroll(button.inputArea()); },
          })
      );
    } else {
      sidebar->addChild(createSettingsSearchInput(scale));
    }

    const std::optional<float> sidebarScrollWidth =
        m_compact ? std::optional<float>{Style::controlHeightSm * scale} : std::nullopt;

    auto sidebarScroll = ui::scrollView({
        .out = &m_sidebarScrollView,
        .state = &m_sidebarScrollState,
        .contentScale = scale,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .fillWidth = false,
        .fillHeight = true,
        .width = sidebarScrollWidth,
        .flexGrow = 1.0F,
        .configure = [](ScrollView& scrollView) {
          scrollView.clearFill();
          scrollView.clearBorder();
        },
    });

    auto sidebarNav = std::make_unique<RovingListNavHost>(RovingListNavController::Options{
        .axis = RovingListNavAxis::Vertical,
        .mode = RovingListNavMode::FollowFocus,
        .keepItemsInTabOrder = false,
        .wrap = true,
        .scrollIntoView = [this](const Node* node) { scrollSidebarNodeIntoView(node); },
        .syncIndexFromSelection = {},
    });
    sidebarNav->setTabFocusKey("control-center.sidebar");
    if (!m_compact) {
      sidebarNav->setAlign(FlexAlign::Stretch);
      sidebarNav->setFillWidth(true);
    } else {
      sidebarNav->setAlign(FlexAlign::Center);
    }
    sidebarNav->setGap(Style::spaceXs * scale);
    m_sidebarNav = sidebarNav.get();
    m_groupDividers.fill(nullptr);

    TabGroup lastHeader = TabGroup::Home;
    for (auto& slot : m_slots) {
      if (slot.id == "home") {
        continue;
      }
      if (slot.group != TabGroup::Home && slot.group != lastHeader) {
        if (lastHeader != TabGroup::Home) {
          addSidebarGroupHeader(*sidebarNav, slot.group, scale);
        }
        lastHeader = slot.group;
      }
      addSidebarTabButton(*sidebarNav, slot, scale);
    }

    if (sidebarScroll->content() != nullptr) {
      if (!m_compact) {
        sidebarScroll->content()->setAlign(FlexAlign::Stretch);
      }
      sidebarScroll->content()->addChild(std::move(sidebarNav));
    }
    sidebar->addChild(std::move(sidebarScroll));
    sidebar->addChild(
        ui::box({
            .out = &m_sidebarEdgeFade,
            .participatesInLayout = false,
            .configure =
                [](Box& box) {
                  box.setHitTestVisible(false);
                  box.setZIndex(8);
                },
        })
    );
    rootLayout->addChild(std::move(sidebar));
    rootLayout->addChild(
        ui::box({
            .fill = colorSpecFromRole(ColorRole::Outline, 0.6F),
            .width = 1.0F,
            .configure = [](Box& box) { box.setHitTestVisible(false); },
        })
    );
  }

  auto content = ui::column({
      .out = &m_content,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .clipChildren = true,
      .flexGrow = 4.0F,
  });

  auto dismissArea = ui::inputArea({});
  dismissArea->setParticipatesInLayout(false);
  dismissArea->setZIndex(-1);
  dismissArea->setFocusable(false);
  dismissArea->setTabStop(false);
  dismissArea->setOnPress([this](const InputArea::PointerData&) {
    if (dismissTransientUi()) {
      PanelManager::instance().refresh();
    }
  });
  m_contentDismissArea = static_cast<InputArea*>(content->addChild(std::move(dismissArea)));

  auto header = ui::row({
      .out = &m_contentHeader,
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceSm * scale,
  });

  header->addChild(
      ui::label({
          .out = &m_contentTitle,
          .text = i18n::tr("control-center.tabs.home"),
          .fontSize = Style::fontSizeHeader * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Primary),
          .flexGrow = 1.0F,
      })
  );

  auto headerActions = ui::row({
      .out = &m_contentHeaderActions,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });

  headerActions->addChild(ui::button({
      .out = &m_nativeApplicationButton,
      .text = i18n::tr("control-center.open-settings"),
      .glyph = "settings",
      .tooltip = i18n::tr("control-center.open-settings"),
      .onClick = [this]() {
        const auto destination = nativeDestinationForTab(m_activeTabId);
        if (!destination.has_value()) return;
        (void)NativeRedirectTab::launch(destination->application, destination->page, m_platform);
      },
  }));

  m_contentHeaderActions->addChild(
      ui::button({
          .out = &m_closeButton,
          .glyph = "close",
          .onClick = []() { PanelManager::instance().close(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  header->addChild(std::move(headerActions));

  content->addChild(std::move(header));
  if (!isQuickHomeContext(pendingOpenContext()) && (!m_showSidebar || m_compact)) {
    auto search = createSettingsSearchInput(scale);
    if (m_compact) {
      // Hidden until the rail's search glyph (or `/`, Ctrl+F) asks for it; it folds away again
      // when it loses focus with nothing typed.
      search->setVisible(false);
      search->setOnFocusLoss([this]() {
        if (m_searchInput != nullptr && m_searchInput->visible() && StringUtils::trim(m_searchQuery).empty()) {
          m_searchInput->setVisible(false);
          PanelManager::instance().requestLayout();
        }
      });
    }
    content->addChild(std::move(search));
  }

  auto bodies = ui::column({
      .out = &m_tabBodies,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .clipChildren = true,
      .flexGrow = 1.0F,
  });

  content->addChild(std::move(bodies));
  content->addChild(
      ui::box({
          .out = &m_contentEdgeFade,
          .participatesInLayout = false,
          .configure =
              [](Box& box) {
                box.setHitTestVisible(false);
                box.setZIndex(8);
              },
      })
  );
  rootLayout->addChild(std::move(content));
  setRoot(std::move(rootLayout));

  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  m_modalHost.initialize(
      PanelManager::instance().inputDispatcher(),
      []() { PanelManager::instance().requestLayout(); },
      []() { PanelManager::instance().requestUpdateOnly(); }
  );
  m_modalAttached = false;

  syncTabVisibility();
  m_firstOpenAfterCreate = true;
  selectTab(tabIdForOpenContext(pendingOpenContext()));
  const auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - createdAt).count();
  kLog.debug("opened chrome for {} tabs in {}ms (active={})", m_slots.size(), elapsedMs, m_activeTabId);
}

void ControlCenterPanel::onPanelCardOpacityChanged(float opacity) {
  for (auto& slot : m_slots) {
    if (slot.tab != nullptr) {
      slot.tab->setPanelCardOpacity(opacity);
    }
  }
  if (m_sidebarEdgeFade != nullptr) {
    styleScrollEdgeFade(*m_sidebarEdgeFade, ColorRole::Surface, 1.0F, true);
  }
  if (m_contentEdgeFade != nullptr) {
    styleScrollEdgeFade(*m_contentEdgeFade, ColorRole::Surface, 1.0F, true);
  }
}

void ControlCenterPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr || m_content == nullptr || m_tabBodies == nullptr) {
    return;
  }

  if (m_showSidebar) {
    layoutFullSidebarWidth(renderer);
  }

  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);

  if (m_showSidebar && m_sidebarScrollView != nullptr) {
    const float widthBefore = m_sidebarScrollView->width();
    layoutFullSidebarWidth(renderer);
    if (std::abs(m_sidebarScrollView->width() - widthBefore) > 0.5F) {
      m_rootLayout->layout(renderer);
    }
  }

  const float contentInnerWidth =
      std::max(0.0F, m_content->width() - (m_content->paddingLeft() + m_content->paddingRight()));
  const float bodyWidth = m_tabBodies->width();
  const float bodyHeight = m_tabBodies->height();
  const float bodyContentHeight = tabContentHeight(bodyHeight);

  if (m_sidebarScrollArea != nullptr && m_sidebar != nullptr) {
    m_sidebarScrollArea->setPosition(0.0F, 0.0F);
    m_sidebarScrollArea->setSize(m_sidebar->width(), m_sidebar->height());
  }

  if (m_contentDismissArea != nullptr) {
    m_contentDismissArea->setPosition(0.0F, 0.0F);
    m_contentDismissArea->setFrameSize(m_content->width(), m_content->height());
  }

  const float fadeH = kScrollEdgeFadePx * contentScale();
  if (m_sidebarEdgeFade != nullptr && m_sidebarScrollView != nullptr) {
    styleScrollEdgeFade(*m_sidebarEdgeFade, ColorRole::Surface, 1.0F, true);
    m_sidebarEdgeFade->setPosition(m_sidebarScrollView->x(), m_sidebarScrollView->y());
    m_sidebarEdgeFade->setSize(m_sidebarScrollView->width(), fadeH);
  }
  if (m_contentEdgeFade != nullptr && m_tabBodies != nullptr) {
    styleScrollEdgeFade(*m_contentEdgeFade, ColorRole::Surface, 1.0F, true);
    m_contentEdgeFade->setPosition(m_tabBodies->x(), m_tabBodies->y());
    m_contentEdgeFade->setSize(m_tabBodies->width(), fadeH);
  }

  if (m_contentHeader != nullptr) {
    m_contentHeader->setSize(contentInnerWidth, 0.0F);
  }

  if (m_contentTitle != nullptr) {
    const float actionsWidth = m_contentHeaderActions != nullptr ? m_contentHeaderActions->width() : 0.0F;
    const float headerGap = m_contentHeader != nullptr ? m_contentHeader->gap() : 0.0F;
    const float titleWidth = std::max(0.0F, contentInnerWidth - actionsWidth - headerGap);
    m_contentTitle->setMaxWidth(titleWidth);
  }

  for (auto& slot : m_slots) {
    if (slot.container != nullptr && slot.container->visible()) {
      slot.container->setSize(bodyWidth, bodyContentHeight);
    }
  }

  layoutTabContainers(bodyWidth, bodyHeight);

  const auto layoutSlot = [this, bodyWidth, bodyContentHeight, &renderer](std::string_view id) {
    auto* slot = slotById(id);
    if (slot == nullptr || slot->tab == nullptr || slot->container == nullptr || !slot->container->visible()) {
      return;
    }
    slot->tab->layout(renderer, bodyWidth, bodyContentHeight);
  };

  if (m_tabTransitionActive) {
    layoutSlot(m_tabTransitionOutgoingId);
  }
  layoutSlot(m_activeTabId);
  attachModalHost(renderer, width, height);
}

void ControlCenterPanel::attachModalHost(Renderer& renderer, float width, float height) {
  Node* sceneRoot = root();
  if (sceneRoot == nullptr) {
    return;
  }
  if (!m_modalAttached) {
    m_modalHost.attach(*sceneRoot, m_content, renderer, width, height);
    m_modalAttached = true;
  } else {
    m_modalHost.resize(renderer, width, height);
  }
}

void ControlCenterPanel::doUpdate(Renderer& renderer) {
  if (!isTabFeatureAvailable(m_activeTabId) || (!m_activeTabForced && !isTabVisible(m_activeTabId))) {
    selectTab(firstVisibleTab());
  } else {
    syncTabVisibility();
  }
  syncPackagesBadge();
  syncAiBadge();
  if (auto* slot = slotById(m_activeTabId); slot != nullptr && slot->tab != nullptr) {
    slot->tab->update(renderer);
  }
  if (m_modalHost.isOpen()) {
    m_modalHost.update(renderer);
  }
}

void ControlCenterPanel::onFrameTick(float deltaMs) {
  if (auto* slot = slotById(m_activeTabId); slot != nullptr && slot->tab != nullptr) {
    slot->tab->onFrameTick(deltaMs);
  }
}

void ControlCenterPanel::onOpen(std::string_view context) {
  if (context == "settings" || context.starts_with("settings:") || settings::isRetiredSettingsTab(context)) {
    DeferredCall::callLater([section = std::string(context)] { PanelManager::instance().openSettings(section); });
    return;
  }
  if (m_dependencies != nullptr) {
    m_dependencies->rescan();
  }
  const bool animateTabSwitch = !m_firstOpenAfterCreate;
  m_firstOpenAfterCreate = false;
  const auto settingsTarget = parseSettingsOpenContext(context);
  std::string tabId;
  std::optional<CaptureTab::View> captureView;
  std::optional<DesktopTab::Section> desktopSection;
  if (settingsTarget.isSettings) {
    tabId = tabIdForSettingsSection(settingsTarget.section);
    captureView = captureViewForContext(settingsTarget.section);
    desktopSection = desktopSectionForContext(settingsTarget.section);
  } else {
    tabId = resolveOpenTabId(context);
    captureView = captureViewForContext(context);
    desktopSection = desktopSectionForContext(context);
  }
  selectTab(tabId, animateTabSwitch);
  if (Tab* openedTab = tabForId(tabId); openedTab != nullptr) {
    if (auto* capture = dynamic_cast<CaptureTab*>(openedTab); capture != nullptr && captureView.has_value()) {
      capture->applyOpenView(*captureView);
    }
    if (auto* desktop = dynamic_cast<DesktopTab*>(openedTab); desktop != nullptr && desktopSection.has_value()) {
      if (settingsTarget.isSettings) {
        desktop->applyOpenContext(settingsTarget.section);
      } else {
        desktop->applyOpenContext(context);
      }
    }
    if (auto* wallpaper = dynamic_cast<WallpaperTab*>(openedTab); wallpaper != nullptr) {
      wallpaper->applyOpenContext(wallpaperPickerContext(context));
    }
    if (settingsTarget.isSettings && tabId != "theme" && !desktopSection.has_value()) {
      openedTab->setConfigureMode(true);
    } else if (isLiveSettingsHost(tabId)) {
      openedTab->setConfigureMode(false);
    }
  }
  maybeOpenPendingEditor();
}

void ControlCenterPanel::queueBarWidget(std::string barName, std::string widgetName) {
  m_pendingOpenBarName = std::move(barName);
  m_pendingOpenWidgetInspectorName = std::move(widgetName);
}

void ControlCenterPanel::maybeOpenPendingEditor() {
  if (!m_pendingOpenWidgetInspectorName.empty()) {
    std::string barName = std::move(m_pendingOpenBarName);
    std::string widgetName = std::move(m_pendingOpenWidgetInspectorName);
    m_pendingOpenBarName.clear();
    m_pendingOpenWidgetInspectorName.clear();
    if (auto* slot = slotById("desktop"); slot != nullptr && slot->tab != nullptr) {
      if (auto* desktop = dynamic_cast<DesktopTab*>(slot->tab.get())) {
        desktop->applyOpenSection(DesktopTab::Section::Bar);
      }
      if (auto* pane = slot->tab->settingsPane(); pane != nullptr) {
        pane->openWidgetInspector(std::move(barName), std::move(widgetName));
      }
    }
  }
}

bool ControlCenterPanel::isContextActive(std::string_view context) const {
  const auto settingsTarget = parseSettingsOpenContext(context);
  if (settingsTarget.isSettings) {
    if (settingsTarget.section.empty()) {
      return isSettingsEditorTab(m_activeTabId) || m_activeTabId == "theme";
    }
    const std::string tabId = tabIdForSettingsSection(settingsTarget.section);
    if (!isTabIdActive(tabId)) {
      return false;
    }
    if (isLiveSettingsHost(tabId)) {
      // Settings panes are non-const; sub-tabs of hub groups are found through tabForId.
      if (Tab* tab = const_cast<ControlCenterPanel*>(this)->tabForId(tabId); tab != nullptr) {
        if (!tab->configureMode()) {
          return false;
        }
        if (tabId == "capture") {
          if (const auto* pane = tab->settingsPane(); pane != nullptr) {
            return pane->section() == settingsTarget.section;
          }
          return false;
        }
        if (tabId == "desktop") {
          if (desktopSectionForContext(settingsTarget.section).has_value()) {
            if (const auto* desktop = dynamic_cast<const DesktopTab*>(tab)) {
              return desktop->matchesOpenContext(settingsTarget.section);
            }
          }
        }
        return true;
      }
      return false;
    }
    return true;
  }
  const std::string requested = std::string(context);
  const std::string tabId = resolveOpenTabId(requested);
  if (!isTabIdActive(tabId)) {
    return false;
  }
  if (tabId == "wallpaper") {
    if (const auto* slot = slotById(tabId); slot != nullptr && slot->tab != nullptr) {
      if (slot->tab->configureMode()) {
        return false;
      }
      if (const auto* wallpaper = dynamic_cast<const WallpaperTab*>(slot->tab.get())) {
        return wallpaperContextMatches(wallpaper->browseSource(), wallpaperPickerContext(requested));
      }
    }
  }
  if (tabId == "capture") {
    if (const auto* slot = slotById(tabId); slot != nullptr && slot->tab != nullptr) {
      if (slot->tab->configureMode()) {
        return false;
      }
      if (const auto* capture = dynamic_cast<const CaptureTab*>(slot->tab.get())) {
        if (const auto view = captureViewForContext(requested)) {
          return capture->currentView() == *view;
        }
      }
    }
  }
  if (tabId == "desktop") {
    if (const auto* slot = slotById(tabId); slot != nullptr && slot->tab != nullptr) {
      if (slot->tab->configureMode()) {
        return false;
      }
      if (const auto* desktop = dynamic_cast<const DesktopTab*>(slot->tab.get())) {
        if (desktopSectionForContext(requested).has_value()) {
          return desktop->matchesOpenContext(requested);
        }
      }
    }
  }
  return true;
}

bool ControlCenterPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  KeyboardEvent event;
  event.sym = sym;
  event.modifiers = modifiers;
  event.pressed = pressed;
  event.preedit = preedit;
  if (m_modalHost.onKeyboardEvent(event)) {
    return true;
  }
  if (auto* slot = slotById(m_activeTabId); slot != nullptr && slot->tab != nullptr) {
    if (slot->tab->handleGlobalKey(sym, modifiers, pressed, preedit)) {
      return true;
    }
  }
  if (pressed && !preedit && m_searchInput != nullptr && !m_modalHost.isOpen()) {
    auto& dispatcher = PanelManager::instance().inputDispatcher();
    const InputArea* const focused = dispatcher.focusedArea();
    const bool textInputFocused = focused != nullptr && focused->textInputClient() != nullptr;
    if (!textInputFocused) {
      const bool slash = modifiers == 0 && sym == XKB_KEY_slash;
      const bool ctrlF = (modifiers & KeyMod::Ctrl) != 0 && (modifiers & ~KeyMod::Ctrl) == 0
          && (sym == XKB_KEY_f || sym == XKB_KEY_F);
      if ((slash || ctrlF) && m_searchInput->inputArea() != nullptr) {
        revealSettingsSearch();
        return true;
      }
    }
  }
  if (!m_showSidebar || m_sidebarNav == nullptr || m_sidebarScrollView == nullptr || m_content == nullptr) {
    return false;
  }

  const SplitPaneFocusConfig panes{
      .sidebarFocus = m_sidebarNav->focusArea(),
      .sidebarRoot = m_sidebarScrollView,
      .contentRoot = m_content,
      .headerFocus = nullptr,
  };
  auto& dispatcher = PanelManager::instance().inputDispatcher();
  const SplitPaneFocusResult splitResult =
      handleSplitPaneFocusNavigation(dispatcher, panes, sym, modifiers, pressed, preedit);
  return splitResult == SplitPaneFocusResult::Consumed;
}

void ControlCenterPanel::onClose() {
  if (m_tabTransitionAnimId != 0 && m_animations != nullptr) {
    m_animations->cancel(m_tabTransitionAnimId);
    m_tabTransitionAnimId = 0;
  }
  m_tabTransitionActive = false;
  m_activeTabId = "home";
  m_activeTabForced = false;
  for (auto& slot : m_slots) {
    if (slot.tab != nullptr) {
      slot.tab->setActive(false);
      slot.tab->onClose();
    }
    slot.button = nullptr;
    slot.container = nullptr;
    slot.headerActions = nullptr;
  }
  m_rootLayout = nullptr;
  m_sidebar = nullptr;
  m_sidebarScrollView = nullptr;
  m_sidebarScrollState = {};
  m_sidebarNav = nullptr;
  m_sidebarScrollArea = nullptr;
  m_groupDividers.fill(nullptr);
  m_searchInput = nullptr;
  m_searchButton = nullptr;
  m_searchQuery.clear();
  m_content = nullptr;
  m_contentDismissArea = nullptr;
  m_contentHeader = nullptr;
  m_contentHeaderActions = nullptr;
  m_contentTitle = nullptr;
  m_closeButton = nullptr;
  m_nativeApplicationButton = nullptr;
  m_tabBodies = nullptr;
  m_sidebarEdgeFade = nullptr;
  m_contentEdgeFade = nullptr;
  m_modalHost.closeAll();
  m_modalHost.detach();
  m_modalAttached = false;
  clearReleasedRoot();
}

bool ControlCenterPanel::deferExternalRefresh() const { return false; }

bool ControlCenterPanel::deferPointerRelayout() const { return deferExternalRefresh(); }

bool ControlCenterPanel::isTabFeatureAvailable(std::string_view id) const {
  if (id == "niri" || id == "windows") {
    return compositors::isNiri();
  }
  if (id == "packages") {
    return noctalia::packages::tabAvailable();
  }
  return true;
}

bool ControlCenterPanel::isTabVisible(std::string_view id) const {
  if (settings::isRetiredSettingsTab(id)) return false;
  if (id == "home" && !isQuickHomeContext(pendingOpenContext())) {
    return false;
  }
  if (!isTabFeatureAvailable(id)) {
    return false;
  }
  const auto* slot = slotById(id);
  if (slot == nullptr) {
    return false;
  }
  if (!slot->hideable || m_config == nullptr) {
    return true;
  }
  const auto& hidden = m_config->config().controlCenter.hiddenTabs;
  if (std::ranges::contains(hidden, id)) {
    return false;
  }
  if (id == "ai" && std::ranges::contains(hidden, "ai-usage")) {
    return false;
  }
  if (id == "capture"
      && (std::ranges::contains(hidden, "screenshot") || std::ranges::contains(hidden, "screen-record"))) {
    return false;
  }
  return true;
}

bool ControlCenterPanel::isTabShown(std::string_view id) const {
  if (id == m_activeTabId && m_activeTabForced) {
    if (!isTabFeatureAvailable(id)) {
      return false;
    }
  } else if (!isTabVisible(id)) {
    return false;
  }
  const auto* slot = slotById(id);
  if (slot == nullptr) {
    return false;
  }
  if (isTabSearchActive()) {
    return control_center::tabSearchMatches(slot->id, slot->title, m_searchQuery);
  }
  return true;
}

std::vector<ControlCenterPanel::TabCatalogEntry> ControlCenterPanel::hideableTabCatalog() {
  std::vector<TabCatalogEntry> out;
  out.reserve(std::size(kBuiltinTabs) - 1);
  for (const auto& spec : kBuiltinTabs) {
    if (!spec.hideable) {
      continue;
    }
    out.push_back({.key = spec.id, .title = builtinTitle(spec)});
  }
  return out;
}

std::vector<ControlCenterPanel::LauncherTabEntry> ControlCenterPanel::visibleTabsForLauncher() const {
  std::vector<LauncherTabEntry> out;
  out.reserve(m_slots.size());
  for (const auto& slot : m_slots) {
    if (slot.id == "home" || !isTabVisible(slot.id)) {
      continue;
    }
    out.push_back({.key = slot.id, .title = slot.title, .glyph = slot.glyph});
    if (const auto* hub = dynamic_cast<const HubTab*>(slot.tab.get()); hub != nullptr) {
      for (const auto& child : hub->children()) {
        out.push_back({.key = child.id, .title = child.title, .glyph = child.glyph});
      }
    }
  }
  return out;
}

std::string ControlCenterPanel::firstVisibleTab() const {
  for (const auto& slot : m_slots) {
    if (isTabVisible(slot.id)) {
      return slot.id;
    }
  }
  return "home";
}

bool ControlCenterPanel::groupHasShownTab(TabGroup group) const {
  for (const auto& slot : m_slots) {
    if (slot.group == group && isTabShown(slot.id)) {
      return true;
    }
  }
  return false;
}

void ControlCenterPanel::addSidebarGroupHeader(RovingListNavHost& nav, TabGroup group, float scale) {
  // Clusters are unlabeled, like macOS System Settings: a divider, not a caption.
  const auto index = static_cast<std::size_t>(group);
  if (group == TabGroup::Home || index >= m_groupDividers.size()) {
    return;
  }
  auto sep = ui::separator({.spacing = Style::spaceXs * scale});
  m_groupDividers[index] = sep.get();
  nav.addChild(std::move(sep));
}

void ControlCenterPanel::addSidebarTabButton(RovingListNavHost& nav, TabSlot& slot, float scale) {
  const std::string id = slot.id;
  const auto onClick = [this, id]() {
    selectTab(id, true);
    PanelManager::instance().refresh();
  };
  nav.addChild(
      ui::button({
          .out = &slot.button,
          .text = m_compact ? std::optional<std::string>{} : std::optional<std::string>{slot.title},
          .glyph = slot.glyph,
          .glyphSize = 21.0F * scale,
          .contentAlign = m_compact ? ButtonContentAlign::Center : ButtonContentAlign::Start,
          .variant = ButtonVariant::Tab,
          .tooltip = m_compact ? slot.title : std::string{},
          .minWidth = m_compact ? std::optional<float>{Style::controlHeightSm * scale} : std::optional<float>{},
          .minHeight = Style::controlHeightSm * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = (m_compact ? Style::spaceXs : Style::spaceSm) * scale,
          .gap = Style::spaceSm * scale,
          .radius = Style::scaledRadiusXl(scale),
          .onClick = onClick,
          .configure = [this, scale, tint = sidebarTabTint(slot.id, slot.group)](Button& button) {
            if (button.label() != nullptr) {
              button.label()->setFontWeight(FontWeight::Bold);
              button.label()->setFontSize(Style::fontSizeBody * scale);
            }
            button.setGlyphTint(tint);
            wireSidebarScroll(button.inputArea());
          },
      })
  );
  nav.registerItem(slot.button, onClick);
}

void ControlCenterPanel::syncPackagesBadge() {
  auto* slot = slotById("packages");
  if (slot == nullptr || slot->button == nullptr) {
    return;
  }
  const std::size_t count = noctalia::packages::pendingUpdateCount();
  if (count == 0) {
    slot->button->setBadge("");
    slot->button->setTooltip(m_compact ? slot->title : std::string{});
    return;
  }
  const std::string badge = count > 99 ? std::string("99+") : std::to_string(count);
  slot->button->setBadge(badge);
  slot->button->setBadgeFontSize(Style::fontSizeCaption * contentScale());
  const std::string waiting = i18n::tr("control-center.packages.updates-waiting", "count", std::to_string(count));
  slot->button->setTooltip(m_compact ? slot->title + " · " + waiting : waiting);
}

void ControlCenterPanel::syncAiBadge() {
  auto* slot = slotById("ai");
  if (slot == nullptr || slot->button == nullptr || m_aiUsage == nullptr) {
    return;
  }
  const auto hit = noctalia::ai_usage::hottestQuota(m_aiUsage->snapshot());
  const int attention = m_aiUsage->attentionPercent();
  if (!hit.has_value() || hit->window.percent + 0.5F < static_cast<float>(attention)) {
    slot->button->setBadge("");
    slot->button->setTooltip(m_compact ? slot->title : std::string{});
    return;
  }
  const int percent = static_cast<int>(std::lround(hit->window.percent));
  slot->button->setBadge(std::to_string(percent));
  slot->button->setBadgeFontSize(Style::fontSizeCaption * contentScale());
  const std::string waiting = i18n::tr(
      "control-center.ai-usage.attention-tooltip", "provider", noctalia::ai_usage::providerLabel(hit->provider),
      "window", noctalia::ai_usage::windowLabel(hit->window.key), "percent", std::to_string(percent)
  );
  slot->button->setTooltip(m_compact ? slot->title + " · " + waiting : waiting);
}

void ControlCenterPanel::syncTabVisibility() {
  for (auto& slot : m_slots) {
    const bool visible = isTabShown(slot.id);
    if (slot.button != nullptr) {
      slot.button->setVisible(visible);
    }
    if (!visible) {
      if (slot.container != nullptr) {
        slot.container->setVisible(false);
      }
      if (slot.headerActions != nullptr) {
        slot.headerActions->setVisible(false);
      }
    }
  }

  const auto setHeaderVisible = [](Node* node, bool visible) {
    if (node == nullptr) {
      return;
    }
    node->setVisible(visible);
    node->setParticipatesInLayout(visible);
  };
  for (std::size_t i = 0; i < m_groupDividers.size(); ++i) {
    setHeaderVisible(m_groupDividers[i], groupHasShownTab(static_cast<TabGroup>(i)));
  }
}

void ControlCenterPanel::updateTabChrome(std::string_view id) {
  if (m_nativeApplicationButton != nullptr) {
    const auto destination = nativeDestinationForTab(id);
    const bool monitor = destination.has_value() && destination->application == "helix-system-monitor";
    const bool phone = destination.has_value() && destination->application == "helix-phone";
    const auto* activeSlot = slotById(id);
    const bool hasSettingsLink = activeSlot != nullptr && activeSlot->tab != nullptr && activeSlot->tab->settingsPane() != nullptr;
    const bool showNativeButton = destination.has_value() && (monitor || !hasSettingsLink);
    const bool available = destination.has_value()
        && process::commandExists(monitor ? "helix-system-monitor" : phone ? "helix-phone" : "helix-settings");
    const auto label = i18n::tr(monitor ? "control-center.open-system-monitor" : phone ? "control-center.open-phone" : "control-center.open-settings");
    m_nativeApplicationButton->setVisible(showNativeButton);
    m_nativeApplicationButton->setParticipatesInLayout(showNativeButton);
    m_nativeApplicationButton->setText(label);
    m_nativeApplicationButton->setEnabled(available);
    m_nativeApplicationButton->setTooltip(available ? label : i18n::tr(monitor ? "control-center.install-system-monitor" : phone ? "control-center.install-phone" : "control-center.install-settings"));
    m_nativeApplicationButton->setGlyph(monitor ? "activity-heartbeat" : phone ? "device-mobile" : "settings");
  }
  for (auto& slot : m_slots) {
    const bool tabEnabled = isTabShown(slot.id);
    const bool active = tabEnabled && slot.id == id;
    if (slot.tab != nullptr) {
      slot.tab->setActive(active);
      if (!active) {
        slot.tab->setConfigureMode(false);
      }
    }
    if (slot.button != nullptr) {
      slot.button->setVisible(tabEnabled);
      slot.button->setVariant(active ? ButtonVariant::TabActive : ButtonVariant::Tab);
    }
    if (active && m_contentTitle != nullptr) {
      m_contentTitle->setText(slot.title);
      m_contentTitle->setColor(
          slot.id == "home" ? colorSpecFromRole(ColorRole::Primary) : colorSpecFromRole(ColorRole::OnSurface)
      );
    }
    if (slot.headerActions != nullptr) {
      slot.headerActions->setVisible(active);
    }
  }

  if (m_contentTitle != nullptr) {
    m_contentTitle->setVisible(true);
  }
  if (m_contentHeaderActions != nullptr) {
    m_contentHeaderActions->setVisible(true);
  }
  if (m_sidebarNav != nullptr) {
    m_sidebarNav->notifyExternalSelectionChanged();
  }
}

void ControlCenterPanel::applyTabContainerVisibility(std::string_view activeId) {
  for (auto& slot : m_slots) {
    if (slot.container != nullptr) {
      slot.container->setVisible(isTabShown(slot.id) && slot.id == activeId);
    }
  }
}

void ControlCenterPanel::layoutTabContainers(float bodyWidth, float bodyHeight) {
  const float travel = bodyHeight > 0.0F ? bodyHeight : 0.0F;
  const float contentHeight = tabContentHeight(bodyHeight);
  for (auto& slot : m_slots) {
    auto* container = slot.container;
    if (container == nullptr || !container->visible()) {
      continue;
    }

    container->setSize(bodyWidth, contentHeight);

    float offsetY = 0.0F;
    float opacity = 1.0F;
    if (m_tabTransitionActive && travel > 0.0F) {
      const auto direction = static_cast<float>(m_tabTransitionDirection);
      if (slot.id == m_tabTransitionOutgoingId) {
        offsetY = -direction * travel * m_tabTransitionProgress;
        opacity = 1.0F - 0.3F * m_tabTransitionProgress;
      } else if (slot.id == m_activeTabId) {
        offsetY = direction * travel * (1.0F - m_tabTransitionProgress);
        opacity = 0.7F + 0.3F * m_tabTransitionProgress;
      }
    }

    container->setPosition(0.0F, offsetY);
    container->setOpacity(opacity);
    if (m_tabTransitionActive) {
      container->setZIndex(slot.id == m_activeTabId ? 1 : 0);
    } else {
      container->setZIndex(0);
    }
  }
}

void ControlCenterPanel::resetTabContainerTransforms() {
  for (auto& slot : m_slots) {
    if (slot.container == nullptr) {
      continue;
    }
    slot.container->setPosition(0.0F, 0.0F);
    slot.container->setOpacity(1.0F);
    slot.container->setZIndex(0);
  }
}

int ControlCenterPanel::visibleTabOrdinal(std::string_view id) const {
  int ordinal = 0;
  for (const auto& slot : m_slots) {
    if (!isTabShown(slot.id)) {
      continue;
    }
    if (slot.id == id) {
      return ordinal;
    }
    ++ordinal;
  }
  return 0;
}

void ControlCenterPanel::applyTabTransitionLayout() {
  if (m_tabBodies == nullptr) {
    return;
  }
  layoutTabContainers(m_tabBodies->width(), m_tabBodies->height());
}

void ControlCenterPanel::startTabTransition(std::string_view from, std::string_view to) {
  if (m_animations == nullptr || m_tabBodies == nullptr) {
    applyTabContainerVisibility(to);
    resetTabContainerTransforms();
    return;
  }

  m_tabTransitionActive = true;
  m_tabTransitionOutgoingId = std::string(from);
  m_tabTransitionProgress = 0.0F;

  const int fromOrdinal = visibleTabOrdinal(from);
  const int toOrdinal = visibleTabOrdinal(to);
  m_tabTransitionDirection = toOrdinal >= fromOrdinal ? 1 : -1;

  for (auto& slot : m_slots) {
    if (slot.container == nullptr) {
      continue;
    }
    slot.container->setVisible(slot.id == from || slot.id == to);
  }

  applyTabTransitionLayout();
  PanelManager::instance().requestLayout();
  PanelManager::instance().requestRedraw();
  PanelManager::instance().requestFrameTick();

  m_tabTransitionAnimId = m_animations->animate(
      0.0F, 1.0F, static_cast<float>(Style::animNormal), Easing::EaseOutCubic,
      [this](float progress) {
        m_tabTransitionProgress = progress;
        applyTabTransitionLayout();
        PanelManager::instance().requestRedraw();
      },
      [this]() {
        m_tabTransitionAnimId = 0;
        finishTabTransition();
        PanelManager::instance().requestLayout();
        PanelManager::instance().requestRedraw();
      },
      m_tabBodies
  );
}

void ControlCenterPanel::finishTabTransition() {
  m_tabTransitionActive = false;
  resetTabContainerTransforms();
  applyTabContainerVisibility(m_activeTabId);
}

void ControlCenterPanel::wireSidebarScroll(InputArea* area) {
  if (area == nullptr) {
    return;
  }
  area->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return false;
    }
    if (m_sidebarScrollView != nullptr && m_sidebarScrollView->scrollable()) {
      const float delta = data.scrollDelta(Style::scrollWheelStep);
      if (data.axisSource == WL_POINTER_AXIS_SOURCE_FINGER) {
        m_sidebarScrollView->setScrollOffset(m_sidebarScrollView->scrollOffset() + delta);
      } else {
        m_sidebarScrollView->scrollBy(delta);
      }
      PanelManager::instance().requestRedraw();
      return true;
    }
    const float steps = data.scrollSteps();
    if (steps == 0.0F) {
      return false;
    }
    selectAdjacentVisibleTab(steps > 0.0F ? 1 : -1);
    return true;
  });
}

void ControlCenterPanel::selectAdjacentVisibleTab(int direction) {
  if (direction == 0 || isQuickHomeContext(pendingOpenContext())) {
    return;
  }

  std::vector<std::string> visible;
  for (const auto& slot : m_slots) {
    if (isTabShown(slot.id)) {
      visible.push_back(slot.id);
    }
  }
  if (visible.empty()) {
    return;
  }

  int activeOrdinal = 0;
  for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
    if (visible[static_cast<std::size_t>(i)] == m_activeTabId) {
      activeOrdinal = i;
      break;
    }
  }

  const int targetOrdinal = activeOrdinal + direction;
  if (targetOrdinal < 0 || targetOrdinal >= static_cast<int>(visible.size())) {
    return;
  }
  selectTab(visible[static_cast<std::size_t>(targetOrdinal)], true);
  PanelManager::instance().refresh();
}

void ControlCenterPanel::ensureTabUi(TabSlot& slot) {
  if (slot.tab == nullptr || m_tabBodies == nullptr || slot.container != nullptr) {
    return;
  }

  auto container = slot.tab->create();
  container->setFlexGrow(1.0F);
  container->setParticipatesInLayout(false);
  container->setVisible(false);
  slot.container = container.get();
  m_tabBodies->addChild(std::move(container));

  if (m_contentHeaderActions == nullptr || slot.headerActions != nullptr) {
    return;
  }
  auto actions = slot.tab->createHeaderActions();
  slot.headerActions = actions.get();
  if (actions == nullptr) {
    return;
  }
  actions->setVisible(false);
  std::size_t insertAt = m_contentHeaderActions->children().size();
  if (m_closeButton != nullptr) {
    const auto& kids = m_contentHeaderActions->children();
    for (std::size_t i = 0; i < kids.size(); ++i) {
      if (kids[i].get() == m_closeButton) {
        insertAt = i;
        break;
      }
    }
  }
  m_contentHeaderActions->insertChildAt(insertAt, std::move(actions));
}

void ControlCenterPanel::selectTab(std::string_view id, bool animated) {
  if (settings::isRetiredSettingsTab(id) || id == "security") {
    DeferredCall::callLater([section = std::string(id)] { PanelManager::instance().openSettings(section); });
    return;
  }
  std::string target(id);
  std::string hubChild;
  if (const auto hubId = hubIdForChild(id); hubId.has_value()) {
    hubChild = std::string(id);
    target = std::string(*hubId);
  }
  if (slotById(target) == nullptr || !isTabFeatureAvailable(target)) {
    target = firstVisibleTab();
  }
  m_activeTabForced = !isTabVisible(target);

  const std::string previous = m_activeTabId;
  const bool tabChanged = target != previous;

  if (auto* slot = slotById(target); slot != nullptr) {
    ensureTabUi(*slot);
    if (!hubChild.empty()) {
      if (auto* hub = dynamic_cast<HubTab*>(slot->tab.get()); hub != nullptr) {
        (void)hub->selectChild(hubChild);
      }
    }
  }
  if (tabChanged && animated) {
    if (auto* previousSlot = slotById(previous); previousSlot != nullptr) {
      ensureTabUi(*previousSlot);
    }
  }

  if (m_tabTransitionAnimId != 0 && m_animations != nullptr) {
    m_animations->cancel(m_tabTransitionAnimId);
    m_tabTransitionAnimId = 0;
    finishTabTransition();
  }

  m_activeTabId = target;
  if (target == "notifications" && m_notificationManager != nullptr) {
    m_notificationManager->markNotificationHistorySeen();
  }

  updateTabChrome(target);

  if (tabChanged && animated && m_animations != nullptr && m_tabBodies != nullptr) {
    startTabTransition(previous, target);
  } else {
    m_tabTransitionActive = false;
    applyTabContainerVisibility(target);
    resetTabContainerTransforms();
  }

  scheduleMprisRefreshFor(hubChild.empty() ? std::string_view(target) : std::string_view(hubChild));
  syncActiveTabSearch();
}

void ControlCenterPanel::scheduleMprisRefreshFor(std::string_view id) {
  if (m_mpris == nullptr || m_mprisRefreshScheduled || (id != "home" && id != "media")) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (m_lastMprisRefreshAt.time_since_epoch().count() != 0 && now - m_lastMprisRefreshAt < kMprisRefreshMinInterval) {
    return;
  }

  m_lastMprisRefreshAt = now;
  m_mprisRefreshScheduled = true;
  DeferredCall::callLater([this]() {
    m_mprisRefreshScheduled = false;
    if (m_mpris == nullptr || !PanelManager::instance().isOpenPanel("control-center")) {
      return;
    }
    m_mpris->refreshPlayers();
    PanelManager::instance().requestUpdateOnly();
    PanelManager::instance().requestRedraw();
  });
}

bool ControlCenterPanel::isDirectSectionOpenContext(std::string_view context) const {
  if (context.empty() || context == "home") {
    return false;
  }
  if (context == "settings" || context.find(':') != std::string_view::npos) {
    return true;
  }
  if (context == "appearance"
      || captureViewForContext(context).has_value() || desktopSectionForContext(context).has_value()) {
    return true;
  }
  for (const auto& spec : kBuiltinTabs) {
    if (spec.hideable && context == spec.id) {
      return true;
    }
  }
  return false;
}

bool ControlCenterPanel::isQuickHomeContext(std::string_view context) noexcept { return context == "home"; }

bool ControlCenterPanel::requiresRecreateForContext(std::string_view context) const {
  return isQuickHomeContext(context) != isQuickHomeContext(pendingOpenContext());
}

ControlCenterSidebarMode ControlCenterPanel::sidebarModeForOpen(std::string_view context) const {
  if (isQuickHomeContext(context)) {
    return ControlCenterSidebarMode::None;
  }
  if (m_config == nullptr) {
    return ControlCenterSidebarMode::Compact;
  }
  const auto& cc = m_config->config().controlCenter;
  const auto settings = parseSettingsOpenContext(context);
  if (context.empty() || settings.isSettings) {
    return cc.sidebarMode;
  }
  return isDirectSectionOpenContext(context) ? cc.sidebarSectionMode : cc.sidebarMode;
}

void ControlCenterPanel::revealSettingsSearch() {
  if (m_searchInput == nullptr || m_searchInput->inputArea() == nullptr) {
    return;
  }
  if (!m_searchInput->visible()) {
    m_searchInput->setVisible(true);
    PanelManager::instance().requestLayout();
  }
  PanelManager::instance().inputDispatcher().setFocus(m_searchInput->inputArea());
}

std::unique_ptr<Input> ControlCenterPanel::createSettingsSearchInput(float scale) {
  return ui::input({
      .out = &m_searchInput,
      .value = m_searchQuery,
      .placeholder = i18n::tr("control-center.settings.search-placeholder"),
      .fontSize = Style::fontSizeBody * scale,
      .controlHeight = Style::controlHeight * scale,
      .horizontalPadding = Style::spaceSm * scale,
      .clearButtonEnabled = true,
      .flexGrow = m_showSidebar ? 0.0F : 1.0F,
      .onChange = [this](const std::string& value) { applySettingsSearch(value); },
  });
}

void ControlCenterPanel::applySettingsSearch(std::string value) {
  if (m_searchQuery == value) {
    return;
  }
  m_searchQuery = std::move(value);
  const bool searching = !m_searchQuery.empty();
  if (searching && isTabSearchActive()) {
    const std::string best = bestTabSearchId();
    if (!best.empty() && best != m_activeTabId) {
      selectTab(best, false);
    }
    if (auto* active = slotById(m_activeTabId); active != nullptr && active->tab != nullptr) {
      const std::string query = StringUtils::trim(m_searchQuery);
      if (auto* desktop = dynamic_cast<DesktopTab*>(active->tab.get())) {
        if (desktopSectionForContext(query).has_value()) {
          desktop->applyOpenContext(query, false);
        }
      }
    }
  } else if (searching) {
    auto* slot = slotById(m_activeTabId);
    const bool hasSettingsPane = isSettingsEditorTab(m_activeTabId)
        || isLiveSettingsHost(m_activeTabId)
        || (slot != nullptr && slot->tab != nullptr && slot->tab->settingsPane() != nullptr);

    std::vector<std::string> currentSections;
    if (m_activeTabId == "capture") {
      currentSections = {"screenshot", "screen-record"};
    } else if (m_activeTabId == "theme") {
      currentSections = {
          "theme", "niri", "chrome", "glass", "icons", "type", "scale", "roundness", "effects", "language"};
    } else if (m_activeTabId == "desktop") {
      currentSections = {
          "desktop", "widgets", "corners", "hot-corners", "screen-corners", "bar", "dock", "windows", "panels", "osd"};
    } else if (slot != nullptr && slot->tab != nullptr) {
      if (const auto* pane = slot->tab->settingsPane(); pane != nullptr && !pane->section().empty()) {
        currentSections.push_back(pane->section());
      }
    }

    std::vector<settings::SettingEntry> registry;
    if (m_config != nullptr) {
      const Config& cfg = m_config->config();
      const BarConfig* bar = cfg.bars.empty() ? nullptr : &cfg.bars.front();
      registry = settings::buildSettingsRegistry(cfg, bar);
    }

    const auto target = resolveSettingsSearchTarget(
        m_activeTabId, hasSettingsPane, currentSections, registry, m_searchQuery
    );
    if (!target.stayOnCurrent) {
      selectTab(target.tabId, false);
    }
    if (auto* active = slotById(m_activeTabId); active != nullptr && active->tab != nullptr) {
      if (auto* desktop = dynamic_cast<DesktopTab*>(active->tab.get())) {
        const std::string query = StringUtils::trim(m_searchQuery);
        if (desktopSectionForContext(query).has_value()) {
          desktop->applyOpenContext(query, false);
        } else if (!target.sectionId.empty() && desktopSectionForContext(target.sectionId).has_value()) {
          desktop->applyOpenContext(target.sectionId, false);
        }
      }
      if (target.configureMode) {
        active->tab->setConfigureMode(true);
      }
    }
  }
  syncTabVisibility();
  syncActiveTabSearch();
  PanelManager::instance().requestLayout();
}

void ControlCenterPanel::syncActiveTabSearch() {
  auto* slot = slotById(m_activeTabId);
  if (slot != nullptr && slot->tab != nullptr) {
    slot->tab->setSettingsSearchQuery(m_searchQuery);
  }
}

bool ControlCenterPanel::isTabSearchActive() const {
  if (StringUtils::trim(m_searchQuery).size() < 2) {
    return false;
  }
  for (const auto& slot : m_slots) {
    if (!isTabVisible(slot.id) && !(slot.id == m_activeTabId && m_activeTabForced)) {
      continue;
    }
    if (control_center::tabSearchMatches(slot.id, slot.title, m_searchQuery)) {
      return true;
    }
  }
  return false;
}

std::string ControlCenterPanel::bestTabSearchId() const {
  int bestScore = 0;
  std::string bestId;
  for (const auto& slot : m_slots) {
    if (!isTabVisible(slot.id) && !(slot.id == m_activeTabId && m_activeTabForced)) {
      continue;
    }
    const int score = control_center::tabSearchScore(slot.id, slot.title, m_searchQuery);
    if (score > bestScore) {
      bestScore = score;
      bestId = slot.id;
    }
    if (const auto* hub = dynamic_cast<const HubTab*>(slot.tab.get()); hub != nullptr) {
      for (const auto& child : hub->children()) {
        const int childScore = control_center::tabSearchScore(child.id, child.title, m_searchQuery);
        if (childScore > bestScore) {
          bestScore = childScore;
          bestId = child.id;
        }
      }
    }
  }
  return bestId;
}

void ControlCenterPanel::layoutFullSidebarWidth(Renderer& renderer) {
  if (m_sidebarScrollView == nullptr || m_sidebarNav == nullptr) {
    return;
  }

  const float scale = contentScale();
  const float fontSize = Style::fontSizeBody * scale;
  const float paddingH = Style::spaceSm * scale * 2.0F;
  const float gap = Style::spaceSm * scale;
  const float glyphW = 21.0F * scale;

  float maxTabWidth = Style::controlHeightSm * scale;
  if (!m_compact) {
    for (const auto& slot : m_slots) {
      if (!isTabVisible(slot.id)) {
        continue;
      }
      const TextMetrics text = renderer.measureText(slot.title, fontSize, FontWeight::Bold);
      maxTabWidth = std::max(maxTabWidth, paddingH + glyphW + gap + text.width);
    }
  }
  if (m_searchInput != nullptr && !m_compact) {
    maxTabWidth = std::max(maxTabWidth, 168.0F * scale);
  }

  const float contentWidth = std::ceil(maxTabWidth);

  float targetWidth = contentWidth;
  const float scrollHeight = m_sidebarScrollView->height();
  if (scrollHeight > 0.0F) {
    LayoutConstraints navConstraints;
    navConstraints.setExactWidth(contentWidth);
    const float navHeight = m_sidebarNav->measure(renderer, navConstraints).height;
    if (navHeight > scrollHeight + 0.5F) {
      targetWidth = contentWidth + m_sidebarScrollView->scrollbarGutter();
    }
  }

  if (m_searchInput != nullptr && !m_compact) {
    const float searchHeight = Style::controlHeight * scale;
    if (std::abs(m_searchInput->width() - targetWidth) > 0.5F
        || std::abs(m_searchInput->height() - searchHeight) > 0.5F) {
      m_searchInput->setSize(targetWidth, searchHeight);
    }
  }

  if (std::abs(m_sidebarScrollView->width() - targetWidth) > 0.5F) {
    m_sidebarScrollView->setSize(targetWidth, m_sidebarScrollView->height());
  }
}

void ControlCenterPanel::scrollSidebarNodeIntoView(const Node* node) {
  if (node == nullptr || m_sidebarScrollView == nullptr) {
    return;
  }
  scrollNodeIntoScrollView(*m_sidebarScrollView, &m_sidebarScrollState, *node, Style::spaceXs * contentScale());
  PanelManager::instance().requestLayout();
}

void ControlCenterPanel::scrollFocusedInputIntoView(InputArea* area) {
  if (area == nullptr) {
    return;
  }

  if (m_sidebarScrollView != nullptr && m_sidebarScrollView->content() != nullptr) {
    for (const Node* node = area; node != nullptr; node = node->parent()) {
      if (node == m_sidebarScrollView->content()) {
        scrollNodeIntoScrollView(*m_sidebarScrollView, &m_sidebarScrollState, *area, Style::spaceXs * contentScale());
        PanelManager::instance().requestLayout();
        return;
      }
    }
  }

  if (ScrollView* scrollView = findEnclosingScrollView(area)) {
    scrollNodeIntoScrollView(*scrollView, nullptr, *area, Style::spaceMd * contentScale());
    PanelManager::instance().requestLayout();
  }
}

ControlCenterPanel::TabSlot* ControlCenterPanel::slotById(std::string_view id) {
  return const_cast<TabSlot*>(std::as_const(*this).slotById(id));
}

const ControlCenterPanel::TabSlot* ControlCenterPanel::slotById(std::string_view id) const {
  for (const auto& slot : m_slots) {
    if (slot.id == id) {
      return &slot;
    }
  }
  return nullptr;
}

Tab* ControlCenterPanel::tabForId(std::string_view id) {
  return const_cast<Tab*>(std::as_const(*this).tabForId(id));
}

const Tab* ControlCenterPanel::tabForId(std::string_view id) const {
  if (const auto hubId = hubIdForChild(id); hubId.has_value()) {
    const auto* slot = slotById(*hubId);
    const auto* hub = slot != nullptr ? dynamic_cast<const HubTab*>(slot->tab.get()) : nullptr;
    return hub != nullptr ? hub->child(id) : nullptr;
  }
  const auto* slot = slotById(id);
  return slot != nullptr ? slot->tab.get() : nullptr;
}

bool ControlCenterPanel::isTabIdActive(std::string_view id) const {
  if (const auto hubId = hubIdForChild(id); hubId.has_value()) {
    if (m_activeTabId != *hubId) {
      return false;
    }
    const auto* slot = slotById(*hubId);
    const auto* hub = slot != nullptr ? dynamic_cast<const HubTab*>(slot->tab.get()) : nullptr;
    return hub != nullptr && hub->activeChildId() == id;
  }
  return m_activeTabId == id;
}
