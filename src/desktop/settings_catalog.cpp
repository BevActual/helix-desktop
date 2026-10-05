#include "desktop/settings_catalog.h"
#include "desktop/settings_routes.h"
#include "desktop/shortcuts.h"
#include "desktop/notification_rules.h"
#include "config/config_export.h"
#include "theme/builtin_palettes.h"
#include "theme/community_palettes.h"
#include "theme/custom_palettes.h"
#include <sstream>
#include <array>
#include <cmath>
#include <stdexcept>
#include <filesystem>
#include <set>

namespace helix {
namespace {
struct Setting {
  const char* id;
  const char* page;
  const char* title;
  const char* group;
  const char* type;
  bool desktop;
  double minimum = 0;
  double maximum = 0;
};
constexpr Setting settings[] = {
  {"control_center.shortcuts", "desktop-dock", "Quick shortcuts", "Control Center", "shortcut-list", true},
  {"control_center.visible_tabs", "desktop-dock", "Visible pages", "Control Center", "tab-list", true},
  {"control_center.sidebar", "desktop-dock", "Sidebar", "Control Center", "sidebar-mode", true},
  {"control_center.sidebar_section", "desktop-dock", "Sidebar for detail views", "Control Center", "sidebar-mode", true},
  {"control_center.width", "desktop-dock", "Window width", "Control Center", "integer", true, 600, 1600},
  {"control_center.height", "desktop-dock", "Window height", "Control Center", "integer", true, 480, 1200},
  {"control_center.show_session_button", "desktop-dock", "Show session actions", "Control Center", "boolean", true},
  {"shell.panel.control_center_placement", "desktop-dock", "Panel placement", "Control Center", "panel-placement", true},
  {"shell.panel.control_center_position", "desktop-dock", "Floating position", "Control Center", "panel-position", true},
  {"shell.panel.open_near_click_control_center", "desktop-dock", "Open near the pointer", "Control Center", "boolean", true},
  {"hot_corners.enabled", "desktop-dock", "Enable hot corners", "Hot Corners", "boolean", true},
  {"hot_corners.delay_ms", "desktop-dock", "Hold delay (milliseconds)", "Hot Corners", "integer", true, 0, 2000},
  {"hot_corners.top_left.action", "desktop-dock", "Top left action", "Hot Corners", "hot-corner-action", true},
  {"hot_corners.top_left.command", "desktop-dock", "Top left command", "Hot Corner Commands", "string", true},
  {"hot_corners.top_right.action", "desktop-dock", "Top right action", "Hot Corners", "hot-corner-action", true},
  {"hot_corners.top_right.command", "desktop-dock", "Top right command", "Hot Corner Commands", "string", true},
  {"hot_corners.bottom_left.action", "desktop-dock", "Bottom left action", "Hot Corners", "hot-corner-action", true},
  {"hot_corners.bottom_left.command", "desktop-dock", "Bottom left command", "Hot Corner Commands", "string", true},
  {"hot_corners.bottom_right.action", "desktop-dock", "Bottom right action", "Hot Corners", "hot-corner-action", true},
  {"hot_corners.bottom_right.command", "desktop-dock", "Bottom right command", "Hot Corner Commands", "string", true},
  {"shell.offline_mode", "privacy-security", "Restrict shell network requests", "Network Privacy", "boolean", true},
  {"shell.external_ip_enabled", "privacy-security", "Look up public IP address", "Network Privacy", "boolean", true},
  {"shell.telemetry_enabled", "privacy-security", "Anonymous startup ping", "Network Privacy", "boolean", true},
  {"shell.screen_time_enabled", "privacy-security", "Record application screen time", "Activity Privacy", "boolean", true},
  {"shell.password_style", "privacy-security", "Password masking", "Authentication", "password-mask", true},
  {"lockscreen.fingerprint", "privacy-security", "Use fingerprint authentication", "Authentication", "boolean", true},
  {"lockscreen.blurred_desktop", "privacy-security", "Use blurred desktop background", "Lock Screen", "boolean", true},
  {"lockscreen.blur_intensity", "privacy-security", "Background blur", "Lock Screen", "number", true, 0, 1},
  {"lockscreen.tint_intensity", "privacy-security", "Background tint", "Lock Screen", "number", true, 0, 1},
  {"lockscreen.monitors", "privacy-security", "Lock-screen displays", "Lock Screen", "display-list", true},
  {"battery.warning_threshold", "system", "Low battery warning (%)", "Power", "integer", true, 0, 100},
  {"lockscreen.lock_before_suspend", "system", "Lock before system suspend", "Session", "boolean", true},
  {"lockscreen.lid_close_keep_external_displays", "system", "Keep external displays active when closing the lid", "Session", "boolean", true},
  {"idle.pre_action_fade_seconds", "system", "Fade before idle actions (seconds)", "Idle", "number", true, 0, 120},
  {"notification.filter", "notifications", "Application Rules", "Application Rules", "notification-rules", true},
  {"notification.monitors", "notifications", "Notification displays", "Displays", "display-list", true},
  {"notification.layer", "notifications", "Popup layer", "Popups", "shell-layer", true},
  {"notification.background_opacity", "notifications", "Popup opacity", "Popups", "number", true, 0, 1},
  {"notification.border", "notifications", "Show popup outline", "Popups", "boolean", true},
  {"osd.enabled", "desktop-dock", "Show on-screen indicators", "On-Screen Indicators", "boolean", true},
  {"osd.position", "desktop-dock", "Indicator position", "On-Screen Indicators", "screen-position", true},
  {"osd.position_vertical", "desktop-dock", "Vertical indicator position", "On-Screen Indicators", "screen-position", true},
  {"osd.kinds.volume", "desktop-dock", "Volume indicators", "Indicator Events", "boolean", true},
  {"osd.orientation", "desktop-dock", "Indicator orientation", "On-Screen Indicators", "orientation", true},
  {"osd.scale", "desktop-dock", "Indicator scale", "On-Screen Indicators", "number", true, 0.5, 2.5},
  {"osd.monitors", "desktop-dock", "Indicator displays", "On-Screen Indicators", "display-list", true},
  {"osd.kinds.volume_output", "desktop-dock", "Output volume", "Indicator Events", "boolean", true},
  {"osd.kinds.volume_input", "desktop-dock", "Microphone volume", "Indicator Events", "boolean", true},
  {"osd.kinds.brightness", "desktop-dock", "Brightness", "Indicator Events", "boolean", true},
  {"osd.kinds.wifi", "desktop-dock", "Wi-Fi", "Indicator Events", "boolean", true},
  {"osd.kinds.bluetooth", "desktop-dock", "Bluetooth", "Indicator Events", "boolean", true},
  {"osd.kinds.power_profile", "desktop-dock", "Power profile", "Indicator Events", "boolean", true},
  {"osd.kinds.keyboard_layout", "desktop-dock", "Keyboard layout", "Indicator Events", "boolean", true},
  {"osd.kinds.lock_keys", "desktop-dock", "Caps Lock and Num Lock", "Indicator Events", "boolean", true},
  {"osd.kinds.media", "desktop-dock", "Media", "Indicator Events", "boolean", true},
  {"osd.kinds.privacy", "desktop-dock", "Privacy", "Indicator Events", "boolean", true},
  {"notification.enable_daemon", "notifications", "Enable Helix notifications", "Notifications", "boolean", true},
  {"notification.show_app_name", "notifications", "Show application names", "Popups", "boolean", true},
  {"notification.show_actions", "notifications", "Show notification actions", "Popups", "boolean", true},
  {"notification.position", "notifications", "Popup position", "Popups", "screen-position", true},
  {"notification.scale", "notifications", "Popup scale", "Popups", "number", true, 0.5, 2.5},
  {"notification.offset_x", "notifications", "Horizontal margin", "Popups", "integer", true, 0, 200},
  {"notification.offset_y", "notifications", "Vertical margin", "Popups", "integer", true, 0, 200},
  {"notification.max_visible", "notifications", "Maximum visible popups", "Popups", "integer", true, 0, 20},
  {"notification.collapse_on_dismiss", "notifications", "Collapse dismissed popups", "Popups", "boolean", true},
  {"notification.keep_dismissed_in_history", "notifications", "Keep dismissed notifications", "History", "boolean", true},
  {"notification.history_retention_hours", "notifications", "History retention (hours)", "History", "integer", true, 0, 8760},
  {"dock.pinned", "desktop-dock", "Pinned dock applications", "Dock Favorites", "application-list", true},
  {"shell.launcher.pinned", "desktop-dock", "Pinned launcher applications", "Launcher Favorites", "application-list", true},
  {"theme.mode", "appearance", "Appearance", "Colors", "mode", false},
  {"theme.source", "appearance", "Palette source", "Colors", "source", false},
  {"theme.community_palette", "appearance", "Community palette", "Colors", "community", false},
  {"theme.custom_palette", "appearance", "Custom palette", "Colors", "custom", false},
  {"theme.builtin", "appearance", "Palette", "Colors", "palette", false},
  {"theme.pure_black_dark", "appearance", "Pure black dark surfaces", "Colors", "boolean", false},
  {"shell.font_family", "appearance", "Interface font", "Typography", "string", true},
  {"accessibility.ui_scale", "accessibility", "Interface scale", "Presentation", "number", true, 0.5, 3},
  {"accessibility.high_contrast", "accessibility", "High contrast", "Presentation", "boolean", true},
  {"shell.animation.enabled", "accessibility", "Animations", "Motion", "boolean", true},
  {"shell.panel.transparency_mode", "appearance", "Shell material", "Shell chrome", "material", true},
  {"shell.app_icon_colorize", "appearance", "Tint shell application icons", "Icons", "boolean", true},
  {"wallpaper.enabled", "appearance", "Show desktop wallpaper", "Wallpaper", "boolean", true},
  {"wallpaper.fill_mode", "appearance", "Image fit", "Wallpaper", "wallpaper-fit", true},
  {"wallpaper.directory", "appearance", "Wallpaper folder", "Wallpaper", "directory", true},
  {"wallpaper.directory_light", "appearance", "Light appearance folder", "Wallpaper", "directory", true},
  {"wallpaper.directory_dark", "appearance", "Dark appearance folder", "Wallpaper", "directory", true},
  {"wallpaper.wallhaven.download_dir", "appearance", "Wallhaven download folder", "Wallhaven", "directory", true},
  {"wallpaper.transition_duration", "appearance", "Transition duration (milliseconds)", "Wallpaper", "number", true, 100, 30000},
  {"wallpaper.transition_on_startup", "appearance", "Animate wallpaper on startup", "Wallpaper", "boolean", true},
  {"wallpaper.automation.enabled", "appearance", "Automatically change wallpaper", "Wallpaper Automation", "boolean", true},
  {"wallpaper.automation.interval_seconds", "appearance", "Change interval (seconds)", "Wallpaper Automation", "integer", true, 1, 86400},
  {"wallpaper.automation.source", "appearance", "Wallpaper source", "Wallpaper Automation", "wallpaper-source", true},
  {"wallpaper.automation.order", "appearance", "Folder order", "Wallpaper Automation", "wallpaper-order", true},
  {"wallpaper.automation.recursive", "appearance", "Include subfolders", "Wallpaper Automation", "boolean", true},
  {"wallpaper.daily.provider", "appearance", "Daily image provider", "Wallpaper Automation", "daily-provider", true},
  {"wallpaper.video.directory", "appearance", "Video folder", "Video Wallpaper", "directory", true},
  {"wallpaper.video.mute", "appearance", "Mute video audio", "Video Wallpaper", "boolean", true},
  {"wallpaper.video.hardware_decode", "appearance", "Hardware decoding", "Video Wallpaper", "boolean", true},
  {"wallpaper.video.auto_pause", "appearance", "Pause video behind windows", "Video Wallpaper", "video-pause", true},
  {"dock.enabled", "desktop-dock", "Show dock", "Dock", "boolean", true},
  {"dock.position", "desktop-dock", "Position", "Dock", "position", true},
  {"dock.auto_hide", "desktop-dock", "Automatically hide", "Dock", "boolean", true},
  {"dock.smart_auto_hide", "desktop-dock", "Hide behind windows", "Dock", "boolean", true},
  {"dock.icon_size", "desktop-dock", "Icon size", "Dock", "integer", true, 16, 128},
  {"dock.magnification", "desktop-dock", "Magnify icons", "Dock", "boolean", true},
  {"dock.active_monitor_only", "desktop-dock", "Show on active display only", "Dock", "boolean", true},
  {"dock.show_running", "desktop-dock", "Show running applications", "Dock", "boolean", true},
  {"dock.show_dots", "desktop-dock", "Show running indicators", "Dock", "boolean", true},
  {"dock.show_instance_count", "desktop-dock", "Show window count badges", "Dock", "boolean", true},
  {"dock.reserve_space", "desktop-dock", "Reserve space for the dock", "Dock", "boolean", true},
  {"dock.launcher_position", "desktop-dock", "Launcher button", "Dock", "dock-launcher", true},
  {"dock.magnification_scale", "desktop-dock", "Magnification amount", "Dock", "number", true, 1, 2},
  {"dock.main_axis_padding", "desktop-dock", "Padding along the dock", "Dock Layout", "integer", true, 0, 100},
  {"dock.cross_axis_padding", "desktop-dock", "Padding across the dock", "Dock Layout", "integer", true, 0, 100},
  {"dock.item_spacing", "desktop-dock", "Space between icons", "Dock Layout", "integer", true, 0, 100},
  {"dock.margin_ends", "desktop-dock", "Inset from display ends", "Dock Layout", "integer", true, 0, 500},
  {"dock.margin_edge", "desktop-dock", "Distance from display edge", "Dock Layout", "integer", true, 0, 100},
  {"shell.launcher.categories", "desktop-dock", "Show application categories", "Launcher", "boolean", true},
  {"shell.launcher.app_grid", "desktop-dock", "Show applications as a grid", "Launcher", "boolean", true},
  {"shell.launcher.show_app_actions", "desktop-dock", "Show application actions", "Launcher", "boolean", true},
  {"shell.launcher.show_app_origin_indicator", "desktop-dock", "Show application source", "Launcher", "boolean", true},
  {"shell.launcher.show_icons", "desktop-dock", "Show application icons", "Launcher", "boolean", true},
  {"shell.launcher.sort_by_usage", "desktop-dock", "Sort by usage", "Launcher", "boolean", true},
  {"shell.launcher.compact", "desktop-dock", "Compact results", "Launcher", "boolean", true},
  {"audio.enable_sounds", "sound", "Interface sounds", "Sounds", "boolean", false},
  {"audio.sound_volume", "sound", "Sound volume", "Sounds", "number", false, 0, 1},
};
constexpr std::pair<std::string_view, std::string_view> controlCenterTabs[] = {
  {"network", "Network"}, {"bluetooth", "Bluetooth"}, {"tailscale", "Tailscale"}, {"phone", "Phone"},
  {"notifications", "Notifications"}, {"audio", "Sound"}, {"screen-time", "Screen Time"}, {"theme", "Appearance"},
  {"control-center", "Control Center"}, {"launcher", "Launcher"}, {"privacy-security", "Privacy & Security"},
  {"system", "System Monitor"}, {"packages", "Software"}, {"clock", "Clock"}, {"desktop", "Desktop & Dock"},
  {"monitor", "Displays"}, {"wallpaper", "Wallpaper"}, {"power", "Power"}, {"keybinds", "Keyboard"},
  {"shell", "Shell"}, {"printers", "Printers"}, {"camera", "Camera"}, {"localsend", "LocalSend"},
  {"syncthing", "Sync"}, {"cast", "Cast"}, {"calendar", "Calendar"}, {"weather", "Weather"},
  {"media", "Media"}, {"capture", "Capture"}, {"clipboard", "Clipboard"}, {"ai", "AI Usage"},
  {"templates", "Templates"}, {"plugins", "Extensions"}, {"hooks", "Automation"}, {"niri", "Niri"}, {"umbriel", "Umbriel"},
};
const Setting* find(std::string_view id) {
  for (const auto& setting : settings) if (setting.id == id) return &setting;
  return nullptr;
}
}
std::vector<std::string> settingPath(std::string_view id) {
  if (id == "control_center.visible_tabs") return {"control_center", "hidden_tabs"};
  if (id == "idle.behavior") return {"idle", "behavior"};
  if (id.starts_with("idle:")) {
    const auto separator = id.rfind(':');
    if (separator > 5) {
      const auto field = id.substr(separator + 1);
      if (field == "enabled" || field == "timeout" || field == "locked_timeout" || field == "action" || field == "lock_before_suspend")
        return {"idle", "behavior", std::string(id.substr(5, separator - 5)), std::string(field)};
    }
  }
  if (id.starts_with("bar:")) {
    auto separator = id.rfind(':');
    if (separator > 4) {
      auto field = id.substr(separator + 1);
      if (field == "enabled" || field == "position" || field == "auto_hide" || field == "smart_auto_hide" || field == "reserve_space" || field == "show_on_workspace_switch" || field == "shadow" || field == "thickness" || field == "start" || field == "center" || field == "end")
        return {"bar", std::string(id.substr(4, separator - 4)), std::string(field)};
    }
  }
  if (!find(id)) throw std::invalid_argument("Unknown editable setting");
  std::vector<std::string> result;
  std::stringstream stream{std::string(id)};
  std::string part;
  while (std::getline(stream, part, '.')) result.push_back(part);
  return result;
}
bool isPortableSetting(std::string_view id) { auto* setting = find(id); return setting && !setting->desktop; }
ConfigOverrideValue settingValue(const Json& value, std::string_view id) {
  if (id == "control_center.shortcuts") return shortcutValues(value);
  if (id == "control_center.visible_tabs") throw std::invalid_argument("Control Center pages require UpdateOrderedList");
  if (id == "notification.filter") return notificationRules(value);
  if (value.is_array()) return value.get<std::vector<std::string>>();
  if (value.is_boolean()) return value.get<bool>();
  if (value.is_number_integer()) return value.get<std::int64_t>();
  if (value.is_number_float()) return value.get<double>();
  if (value.is_string()) return value.get<std::string>();
  throw std::invalid_argument("Setting must have a primitive value");
}
Json settingsCatalog(ConfigService& config, bool helixDesktop) {
  auto table = config_export::serialize(config.config());
  Json result = Json::array();
  for (const auto& setting : settings) {
    if (setting.desktop && !helixDesktop) continue;
    auto path = settingPath(setting.id);
    if (std::string_view(setting.type) == "shortcut-list") {
      Json values = Json::array(), options = Json::array(), labels = Json::array();
      for (const auto& shortcut : config.config().controlCenter.shortcuts) values.push_back(shortcutValue(shortcut));
      for (const auto& [id, label] : builtInShortcuts) if (shortcutAvailable(id, config.config())) { options.push_back(id); labels.push_back(label); }
      for (const auto& shortcut : config.config().controlCenter.shortcuts) if (std::ranges::find(options, Json(shortcut.type)) == options.end()) {
        options.push_back(shortcut.type); labels.push_back(shortcut.type);
      }
      result.push_back({{"id", setting.id}, {"page", setting.page}, {"title", setting.title}, {"group", setting.group},
        {"type", setting.type}, {"value", values}, {"options", options}, {"option-labels", labels},
        {"overridden", config.hasOverride(path)}, {"description", "Add, resize and arrange shortcuts in Control Center Home. Saved extension shortcuts remain editable."}});
      continue;
    }
    if (std::string_view(setting.type) == "tab-list") {
      Json options = Json::array(), labels = Json::array(), visible = Json::array();
      const auto& hidden = config.config().controlCenter.hiddenTabs;
      for (const auto& [id, name] : controlCenterTabs) {
        if (::settings::isRetiredSettingsTab(id)) continue;
        options.push_back(id); labels.push_back(name);
        bool shown = std::ranges::find(hidden, id) == hidden.end();
        if (id == "ai" && std::ranges::find(hidden, "ai-usage") != hidden.end()) shown = false;
        if (id == "capture" && (std::ranges::find(hidden, "screenshot") != hidden.end() || std::ranges::find(hidden, "screen-record") != hidden.end())) shown = false;
        if (shown) visible.push_back(id);
      }
      result.push_back({{"id", setting.id}, {"page", setting.page}, {"title", setting.title}, {"group", setting.group},
        {"type", setting.type}, {"value", visible}, {"options", options}, {"option-labels", labels},
        {"minimum", 0}, {"maximum", options.size()}, {"overridden", config.hasOverride(path)},
        {"description", "Choose which built-in pages appear in Control Center. Home always remains available; existing plugin visibility is preserved."}});
      continue;
    }
    if (std::string_view(setting.type) == "notification-rules") {
      Json values = Json::array(); for (const auto& rule : config.config().notification.filters) values.push_back(notificationRule(rule));
      result.push_back({{"id", setting.id}, {"page", setting.page}, {"title", setting.title}, {"group", setting.group}, {"type", setting.type},
        {"value", values}, {"options", Json::array()}, {"minimum", 0}, {"maximum", 256}, {"overridden", config.hasOverride(path)}}); continue;
    }
    const toml::node* node = &table;
    for (const auto& part : path) { auto* parent = node ? node->as_table() : nullptr; node = parent ? parent->get(part) : nullptr; }
    if (!node) continue;
    Json value;
    if (auto* b = node->as_boolean()) value = b->get();
    else if (auto* i = node->as_integer()) value = i->get();
    else if (auto* d = node->as_floating_point()) value = d->get();
    else if (auto* text = node->as_string()) value = text->get();
    else if (auto* array = node->as_array()) {
      value = Json::array(); for (const auto& item : *array) if (item.is_string()) value.push_back(item.as_string()->get());
    } else continue;
    Json options = Json::array();
    std::string_view type = setting.type;
    if (type == "mode") options = {"dark", "light", "auto"};
    if (type == "source") {
      options = {"builtin"};
      if (helixDesktop) options.push_back("wallpaper");
      if (!noctalia::theme::availableCommunityPalettes().empty()) options.push_back("community");
      if (!noctalia::theme::availableCustomPalettes().empty()) options.push_back("custom");
    }
    if (type == "community") for (const auto& palette : noctalia::theme::availableCommunityPalettes()) options.push_back(palette.name);
    if (type == "custom") for (const auto& palette : noctalia::theme::availableCustomPalettes()) options.push_back(palette.name);
    if ((type == "community" || type == "custom") && options.empty()) continue;
    if (type == "shell-layer") options = {"top", "overlay"};
    if (type == "sidebar-mode") options = {"full", "compact", "none"};
    if (type == "panel-placement") options = {"attached", "floating"};
    if (type == "panel-position") options = {"auto", "center", "top_left", "top_center", "top_right", "center_left", "center_right", "bottom_left", "bottom_center", "bottom_right"};
    if (type == "orientation") options = {"horizontal", "vertical"};
    if (type == "screen-position") options = {"top_left", "top_center", "top_right", "bottom_left", "bottom_center", "bottom_right"};
    if (type == "dock-launcher") options = {"none", "start", "end"};
    if (type == "wallpaper-fit") options = {"center", "crop", "fit", "stretch", "repeat", "span"};
    if (type == "wallpaper-source") options = {"folder", "daily"};
    if (type == "wallpaper-order") options = {"random", "alphabetical"};
    if (type == "daily-provider") options = {"bing", "nasa"};
    if (type == "video-pause") options = {"off", "max", "full"};
    if (type == "hot-corner-action") options = {"none", "launcher", "control_center", "window_switcher", "command"};
    if (type == "password-mask") options = {"default", "random"};
    if (type == "material") options = {"solid", "soft", "glass"};
    if (type == "position") options = {"bottom", "left", "right", "top"};
    if (type == "palette") for (const auto& palette : noctalia::theme::builtinPalettes()) options.push_back(palette.name);
    Json labels = options;
    if (type == "mode") labels = {"Dark", "Light", "Automatic"};
    if (type == "source") {
      labels = Json::array();
      for (const auto& source : options) {
        if (source == "builtin") labels.push_back("Built-in");
        else if (source == "wallpaper") labels.push_back("Wallpaper");
        else if (source == "community") labels.push_back("Community");
        else labels.push_back("Custom");
      }
    }
    if (type == "material") labels = {"Solid", "Soft", "Glass"};
    if (type == "position") labels = {"Bottom", "Left", "Right", "Top"};
    if (type == "wallpaper-source") labels = {"Folder", "Daily picture"};
    if (type == "wallpaper-order") labels = {"Random", "Alphabetical"};
    if (type == "daily-provider") labels = {"Bing", "NASA"};
    if (type == "video-pause") labels = {"Never", "Behind maximized windows", "Behind fullscreen windows"};
    if (type == "hot-corner-action") labels = {"None", "Launcher", "Control Center", "Window switcher", "Custom command"};
    if (type == "password-mask") labels = {"Filled circles", "Random icons"};
    if (type == "shell-layer") labels = {"Normal", "Above fullscreen windows"};
    if (type == "sidebar-mode") labels = {"Full sidebar", "Compact sidebar", "No sidebar"};
    if (type == "panel-placement") labels = {"Attached to bar", "Floating"};
    if (type == "panel-position") labels = {"Automatic", "Center", "Top left", "Top center", "Top right", "Center left", "Center right", "Bottom left", "Bottom center", "Bottom right"};
    if (type == "orientation") labels = {"Horizontal", "Vertical"};
    if (type == "screen-position") labels = {"Top left", "Top center", "Top right", "Bottom left", "Bottom center", "Bottom right"};
    if (type == "dock-launcher") labels = {"Hidden", "Start", "End"};
    if (type == "wallpaper-fit") labels = {"Center", "Fill", "Fit", "Stretch", "Tile", "Span displays"};
    if (type == "wallpaper-source") labels = {"Folder", "Daily image"};
    if (type == "wallpaper-order") labels = {"Random", "Alphabetical"};
    if (type == "daily-provider") labels = {"Bing", "NASA"};
    if (type == "video-pause") labels = {"Never", "Maximized windows", "Fullscreen windows"};
    result.push_back({{"id", setting.id}, {"page", setting.page}, {"title", setting.title},
      {"group", setting.group}, {"type", setting.type}, {"value", value}, {"options", options}, {"option-labels", labels},
      {"minimum", setting.minimum}, {"maximum", setting.maximum}, {"overridden", config.hasOverride(path)}});
  }
  for (auto& entry : result) {
    const auto id = entry.at("id").get<std::string>();
    if (id == "control_center.sidebar") entry["description"] = "Controls the navigation sidebar in the full Control Center. The quick Home panel stays compact.";
    if (id == "control_center.sidebar_section") entry["description"] = "Controls the sidebar when opening a detail view directly from a bar control or command.";
    if (id == "control_center.width") entry["description"] = "Maximum width of the full Control Center. Compact and sidebar-free views scale from this value.";
    if (id == "control_center.height") entry["description"] = "Maximum height of the Control Center. Scrollable content remains accessible when it does not fit.";
    if (id == "control_center.show_session_button") entry["description"] = "Shows Lock, Suspend and Power actions on Home.";
    if (id == "shell.panel.control_center_placement") entry["description"] = "Attached follows the bar; Floating uses a screen position.";
    if (id == "shell.panel.control_center_position") entry["description"] = "Used only when Control Center is floating. Automatic follows the bar.";
    if (id == "shell.panel.open_near_click_control_center") entry["description"] = "When floating, opens near the click instead of the chosen screen anchor where supported.";
    if (id == "hot_corners.delay_ms") entry["description"] = "Keep the pointer in a corner for this long before triggering its action. Zero triggers immediately.";
    if (id.starts_with("hot_corners.") && id.ends_with(".command")) entry["description"] = "Runs when hot corners are enabled and this corner uses Custom command. Saving does not run the command.";

    if (entry.at("id") == "shell.offline_mode") entry["description"] = "Blocks Helix Shell HTTP requests. Does not disconnect the network or restrict other applications.";
    if (entry.at("id") == "shell.external_ip_enabled") entry["description"] = "Uses an external service to resolve your public address. Unavailable while shell network requests are restricted.";
    if (entry.at("id") == "shell.telemetry_enabled") entry["description"] = "Sends an anonymous ping when Helix Shell starts. Unavailable while shell network requests are restricted.";
    if (entry.at("id") == "shell.screen_time_enabled") entry["description"] = "Records focused-application usage locally while Helix is running. Saved reports are available in Helix System Monitor. Disabling recording preserves existing history.";
    if (entry.at("id") == "lockscreen.fingerprint") entry["description"] = "Allows fingerprint unlock when a configured reader and fprintd are available. Password unlock remains available.";
    if (entry.at("id") == "lockscreen.blurred_desktop") entry["description"] = "Uses compositor screen capture where supported by helix-niri.";
    if (entry.at("id") == "lockscreen.monitors") entry["description"] = "Selects lock-screen presentation displays. This does not disable session locking. An empty list uses all displays.";
    if (entry.at("id") == "shell.font_family") entry["description"] = "Changes Helix Shell text. Application typography follows this device’s native defaults.";
    if (entry.at("id") == "accessibility.ui_scale") entry["description"] = "Scales Helix Shell. Applications use the device’s native scaling.";
    if (entry.at("id") == "accessibility.high_contrast") entry["description"] = "Uses high contrast in Helix Shell and native high-contrast styling in Settings.";
    if (entry.at("id") == "shell.animation.enabled") entry["description"] = "Controls motion in Helix Shell and Settings.";
    if (entry.at("id") == "notification.max_visible") entry["description"] = "Zero lets available screen space determine the limit.";
    if (entry.at("id") == "notification.history_retention_hours") entry["description"] = "Zero keeps history without time-based expiry.";
    if (entry.at("id") == "notification.enable_daemon") entry["description"] = "Controls the Helix notification server for this desktop.";
  }
  if (helixDesktop) for (const auto& behavior : config.config().idle.behaviors) {
    for (const std::string field : {"enabled", "timeout", "locked_timeout", "action", "lock_before_suspend"}) {
      if (field == "lock_before_suspend" && behavior.action != "suspend") continue;
      const auto id = "idle:" + behavior.name + ":" + field;
      Json value = field == "action" ? Json(behavior.action) : field == "timeout" ? Json(behavior.timeoutSeconds) : field == "locked_timeout" ? Json(behavior.lockedTimeoutSeconds) : Json(field == "enabled" ? behavior.enabled : behavior.lockBeforeSuspend);
      Json options = Json::array(), labels = Json::array();
      if (field == "action") {
        options = {"lock", "screen_off", "suspend", "lock_and_suspend"}; labels = {"Lock session", "Turn off displays", "Suspend", "Lock and suspend"};
        if (!behavior.command.empty() || behavior.action == "command") { options.push_back("command"); labels.push_back("Existing custom command"); }
      }
      const bool time = field == "timeout" || field == "locked_timeout";
      Json entry = {{"id", id}, {"page", "system"}, {"group", "Idle · " + behavior.name},
        {"title", field == "enabled" ? "Enable timer" : field == "timeout" ? "Idle delay (seconds)" : field == "locked_timeout" ? "Delay while locked (seconds)" : field == "action" ? "Action" : "Lock before suspending"},
        {"type", field == "action" ? "idle-action" : time ? "number" : "boolean"}, {"value", value}, {"options", options}, {"option-labels", labels},
        {"minimum", 0}, {"maximum", time ? 86400 : 0}, {"overridden", false}, {"group-overridden", config.hasOverride({"idle", "behavior"})}};
      if (field == "locked_timeout") entry["description"] = "Zero uses the normal idle delay.";
      if (field == "timeout") entry["description"] = "Zero disables this timer while unlocked.";
      result.push_back(std::move(entry));
    }
  }
  if (helixDesktop) for (const auto& bar : config.config().bars) {
    for (const std::string field : {"start", "center", "end"}) {
      const auto id = "bar:" + bar.name + ":" + field;
      Json options = Json::array();
      std::vector<std::string> names;
      for (const auto& [name, widget] : config.config().widgets) names.push_back(name);
      for (const auto& configuredBar : config.config().bars) {
        for (const auto* section : {&configuredBar.startWidgets, &configuredBar.centerWidgets, &configuredBar.endWidgets})
          names.insert(names.end(), section->begin(), section->end());
      }
      std::ranges::sort(names); names.erase(std::unique(names.begin(), names.end()), names.end());
      for (const auto& name : names) options.push_back(name);
      const auto& values = field == "start" ? bar.startWidgets : field == "center" ? bar.centerWidgets : bar.endWidgets;
      result.push_back({{"id", id}, {"page", "desktop-dock"}, {"title", field == "start" ? "Start widgets" : field == "center" ? "Center widgets" : "End widgets"},
        {"group", "Bar · " + bar.name + " · Widgets"}, {"type", "widget-list"}, {"value", values}, {"options", options},
        {"minimum", 0}, {"maximum", 256}, {"overridden", config.hasOverride(settingPath(id))}});
    }
    for (const std::string field : {"enabled", "position", "auto_hide", "smart_auto_hide", "reserve_space", "show_on_workspace_switch", "shadow", "thickness"}) {
      auto id = "bar:" + bar.name + ":" + field;
      Json value = field == "position" ? Json(bar.position) : field == "thickness" ? Json(bar.thickness) : Json(field == "enabled" ? bar.enabled : field == "auto_hide" ? bar.autoHide : field == "smart_auto_hide" ? bar.smartAutoHide : field == "reserve_space" ? bar.reserveSpace : field == "show_on_workspace_switch" ? bar.showOnWorkspaceSwitch : bar.shadow);
      std::string title = field == "enabled" ? "Show bar" : field == "position" ? "Position" : field == "auto_hide" ? "Automatically hide" : field == "smart_auto_hide" ? "Hide behind windows" : field == "reserve_space" ? "Reserve space for the bar" : field == "show_on_workspace_switch" ? "Reveal when switching workspaces" : field == "shadow" ? "Show shadow" : "Thickness";
      result.push_back({{"id", id}, {"page", "desktop-dock"}, {"title", title}, {"group", "Bar · " + bar.name},
        {"type", field == "position" ? "position" : field == "thickness" ? "integer" : "boolean"}, {"value", value},
        {"options", field == "position" ? Json({"top", "bottom", "left", "right"}) : Json::array()},
        {"minimum", field == "thickness" ? 10 : 0}, {"maximum", field == "thickness" ? 300 : 0}, {"overridden", config.hasOverride(settingPath(id))}});
    }
  }
  return result;
}
}

namespace helix {
void validateSettingValue(ConfigService& config, std::string_view id, const Json& value, bool desktop) {
  for (const auto& entry : settingsCatalog(config, desktop)) {
    if (entry.at("id") != id) continue;
    const auto type = entry.at("type").get<std::string>();
    if (type == "shortcut-list") {
      const auto shortcuts = shortcutValues(value);
      for (const auto& shortcut : shortcuts) if (std::ranges::find(entry.at("options"), Json(shortcut.type)) == entry.at("options").end())
        throw std::invalid_argument("Unknown or disabled shortcut");
    } else if (type == "notification-rules") { (void)notificationRules(value);
    } else if (type == "application-list" || type == "widget-list" || type == "display-list" || type == "tab-list") {
      if (!value.is_array() || value.size() > 256) throw std::invalid_argument("Expected at most 256 application IDs");
      std::set<std::string> seen;
      for (const auto& item : value) {
        if (!item.is_string()) throw std::invalid_argument("Application IDs must be text");
        const auto& text = item.get_ref<const std::string&>();
        if (type == "tab-list" && std::ranges::find(entry.at("options"), item) == entry.at("options").end())
          throw std::invalid_argument("Unknown Control Center page");
        if (text.empty() || text.size() > 255 || (type != "display-list" && text.find('/') != std::string::npos) ||
            std::ranges::any_of(text, [](unsigned char c) { return c < 32 || c == 127; }) || !seen.insert(text).second)
          throw std::invalid_argument("Invalid or duplicate list entry");
        if (type == "widget-list" && std::ranges::find(entry.at("options"), item) == entry.at("options").end() &&
            std::ranges::find(entry.at("value"), item) == entry.at("value").end())
          throw std::invalid_argument("Unknown widget instance");
      }
    } else if (type == "boolean") {
      if (!value.is_boolean()) throw std::invalid_argument("This setting requires a boolean");
    } else if (type == "number" || type == "integer") {
      if (!value.is_number() || (type == "integer" && !value.is_number_integer())) throw std::invalid_argument("This setting requires a number");
      const double number = value.get<double>();
      if (!std::isfinite(number) || number < entry.at("minimum").get<double>() || number > entry.at("maximum").get<double>()) throw std::invalid_argument("Setting is outside its supported range");
    } else {
      if (!value.is_string()) throw std::invalid_argument("This setting requires text");
      if (type == "directory" && !value.get_ref<const std::string&>().empty()) {
        const auto& path = value.get_ref<const std::string&>();
        std::error_code error;
        if (!std::filesystem::path(path).is_absolute() || !std::filesystem::is_directory(path, error))
          throw std::invalid_argument("Choose an existing folder");
      }
      if (!entry.at("options").empty() && std::ranges::find(entry.at("options"), value) == entry.at("options").end()) throw std::invalid_argument("Unknown setting option");
    }
    return;
  }
  throw std::invalid_argument("Setting is unavailable on this device");
}
}
