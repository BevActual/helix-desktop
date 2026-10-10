#include "desktop/settings_catalog.h"
#include "desktop_api/settings_routes.h"
#include "desktop/shortcuts.h"
#include "desktop/notification_rules.h"
#include "core/input/key_chord.h"
#include "config/config_export.h"
#include "config/color_spec.h"
#include "config/config_types.h"
#include "theme/builtin_palettes.h"
#include "theme/builtin_templates.h"
#include "theme/community_palettes.h"
#include "theme/community_templates.h"
#include "theme/custom_palettes.h"
#include "i18n/i18n_service.h"
#include "inference/routing.h"
#include "theme/color_roles.h"
#include <sstream>
#include <array>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <filesystem>
#include <map>
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
  {"nightlight.enabled", "displays", "Night Light", "Night Light", "boolean", true},
  {"nightlight.force", "displays", "Keep Night Light on", "Night Light", "boolean", true},
  {"nightlight.temperature_day", "displays", "Day temperature (Kelvin)", "Night Light", "integer", true, 1100, 25000},
  {"nightlight.temperature_night", "displays", "Night temperature (Kelvin)", "Night Light", "integer", true, 1000, 24900},
  {"keybinds.validate", "keyboard", "Confirm or activate", "Helix Shell Navigation", "key-chords", true},
  {"keybinds.cancel", "keyboard", "Cancel or close", "Helix Shell Navigation", "key-chords", true},
  {"keybinds.left", "keyboard", "Move left", "Helix Shell Navigation", "key-chords", true},
  {"keybinds.right", "keyboard", "Move right", "Helix Shell Navigation", "key-chords", true},
  {"keybinds.up", "keyboard", "Move up", "Helix Shell Navigation", "key-chords", true},
  {"keybinds.down", "keyboard", "Move down", "Helix Shell Navigation", "key-chords", true},
  {"keybinds.tab_next", "keyboard", "Next tab", "Helix Shell Navigation", "key-chords", true},
  {"keybinds.tab_previous", "keyboard", "Previous tab", "Helix Shell Navigation", "key-chords", true},
  {"keybinds.copy", "keyboard", "Copy selected item", "Helix Shell Navigation", "key-chords", true},
  {"keybinds.save", "keyboard", "Save selected item", "Helix Shell Navigation", "key-chords", true},
  {"keybinds.delete", "keyboard", "Delete selected item", "Helix Shell Navigation", "key-chords", true},
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
  {"shell.clipboard_enabled", "privacy-security", "Keep clipboard history", "Clipboard", "boolean", true},
  {"shell.clipboard_keep_from_closed_apps", "privacy-security", "Keep copied items after apps close", "Clipboard", "boolean", true},
  {"shell.clipboard_history_max_entries", "privacy-security", "History size", "Clipboard", "integer", true,
    double(helix::config::kClipboardHistoryMinEntries), double(helix::config::kClipboardHistoryMaxEntries)},
  {"shell.clipboard_confirm_clear_history", "privacy-security", "Confirm history deletion", "Clipboard", "boolean", true},
  {"shell.clipboard_auto_paste", "privacy-security", "Paste selected items automatically", "Clipboard", "clipboard-paste", true},
  {"shell.clipboard_image_action_command", "privacy-security", "Image editor command", "Clipboard", "string", true},
  {"shell.password_style", "privacy-security", "Password masking", "Authentication", "password-mask", true},
  {"shell.polkit_agent", "privacy-security", "Handle authentication dialogs", "Authentication", "boolean", true},
  {"lockscreen.enabled", "privacy-security", "Show lock screen", "Lock Screen", "boolean", true},
  {"lockscreen.fingerprint", "privacy-security", "Use fingerprint authentication", "Authentication", "boolean", true},
  {"lockscreen.allow_empty_password", "privacy-security", "Allow unlocking without a password", "Lock Screen", "boolean", true},
  {"lockscreen.wallpaper", "privacy-security", "Lock-screen wallpaper", "Lock Screen", "string", true},
  {"lockscreen.blurred_desktop", "privacy-security", "Use blurred desktop background", "Lock Screen", "boolean", true},
  {"lockscreen.blur_intensity", "privacy-security", "Background blur", "Lock Screen", "number", true, 0, 1},
  {"lockscreen.tint_intensity", "privacy-security", "Background tint", "Lock Screen", "number", true, 0, 1},
  {"lockscreen.monitors", "privacy-security", "Lock-screen displays", "Lock Screen", "display-list", true},
  // Mirror keys for search; values live in ~/.local/state/helix/inference-routing.toml (Mobius).
  {"ai.act_model", "privacy-security", "On-device act model (GGUF id)", "Models & routing", "string", false},
  {"ai.chat_model", "privacy-security", "On-device chat model (GGUF id)", "Models & routing", "string", false},
  {"ai.lan_endpoint", "privacy-security", "LAN OpenAI-compatible endpoint", "Models & routing", "string", false},
  {"ai.allow_remote_on_battery_saver", "privacy-security", "Allow LAN/cloud on battery saver", "Models & routing", "boolean", false},
  {"battery.warning_threshold", "power", "Low battery warning (%)", "Power", "integer", true, 0, 100},
  {"lockscreen.lock_before_suspend", "power", "Lock before system suspend", "Session", "boolean", true},
  {"lockscreen.lid_close_keep_external_displays", "power", "Keep external displays active when closing the lid", "Session", "boolean", true},
  {"idle.pre_action_fade_seconds", "power", "Fade before idle actions (seconds)", "Idle", "number", true, 0, 120},
  {"notification.filter", "notifications", "Application Rules", "Application Rules", "notification-rules", true},
  {"notification.monitors", "notifications", "Notification displays", "Displays", "display-list", true},
  {"notification.layer", "notifications", "Popup layer", "Popups", "shell-layer", true},
  {"notification.background_opacity", "notifications", "Popup opacity", "Popups", "number", true, 0, 1},
  {"notification.border", "notifications", "Show popup outline", "Popups", "boolean", true},
  {"osd.enabled", "desktop-dock", "Show on-screen indicators", "On-Screen Indicators", "boolean", true},
  {"osd.position", "desktop-dock", "Indicator position", "On-Screen Indicators", "screen-position", true},
  {"osd.position_vertical", "desktop-dock", "Vertical indicator position", "On-Screen Indicators", "screen-position", true},
  {"osd.orientation", "desktop-dock", "Indicator orientation", "On-Screen Indicators", "orientation", true},
  {"osd.scale", "desktop-dock", "Indicator scale", "On-Screen Indicators", "number", true, 0.5, 2.5},
  {"osd.background_opacity", "desktop-dock", "Background opacity", "On-Screen Indicators", "number", true, 0, 1},
  {"osd.border", "desktop-dock", "Show outline", "On-Screen Indicators", "boolean", true},
  {"osd.offset_x", "desktop-dock", "Horizontal margin", "On-Screen Indicators", "integer", true, 0, 200},
  {"osd.offset_y", "desktop-dock", "Vertical margin", "On-Screen Indicators", "integer", true, 0, 200},
  {"osd.monitors", "desktop-dock", "Indicator displays", "On-Screen Indicators", "display-list", true},
  {"osd.kinds.volume", "desktop-dock", "Volume indicators", "Indicator Events", "boolean", true},
  {"osd.kinds.volume_output", "desktop-dock", "Output volume", "Indicator Events", "boolean", true},
  {"osd.kinds.volume_input", "desktop-dock", "Microphone volume", "Indicator Events", "boolean", true},
  {"osd.kinds.brightness", "desktop-dock", "Brightness", "Indicator Events", "boolean", true},
  {"osd.kinds.keyboard_backlight", "desktop-dock", "Keyboard backlight", "Indicator Events", "boolean", true},
  {"osd.kinds.wifi", "desktop-dock", "Wi-Fi", "Indicator Events", "boolean", true},
  {"osd.kinds.bluetooth", "desktop-dock", "Bluetooth", "Indicator Events", "boolean", true},
  {"osd.kinds.power_profile", "desktop-dock", "Power profile", "Indicator Events", "boolean", true},
  {"osd.kinds.caffeine", "desktop-dock", "Keep awake", "Indicator Events", "boolean", true},
  {"osd.kinds.nightlight", "desktop-dock", "Night Light", "Indicator Events", "boolean", true},
  {"osd.kinds.dnd", "desktop-dock", "Do Not Disturb", "Indicator Events", "boolean", true},
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
  {"theme.shell_mode", "appearance", "Shell chrome appearance", "Colors", "shell-mode", true},
  {"theme.wallpaper_scheme", "appearance", "Wallpaper palette scheme", "Colors", "wallpaper-scheme", true},
  {"theme.source", "appearance", "Palette source", "Colors", "source", false},
  {"backdrop.enabled", "appearance", "Dim wallpaper behind windows", "Backdrop", "boolean", true},
  {"backdrop.blur_intensity", "appearance", "Backdrop blur", "Backdrop", "number", true, 0, 1},
  {"backdrop.tint_intensity", "appearance", "Backdrop tint", "Backdrop", "number", true, 0, 1},
  {"theme.community_palette", "appearance", "Community palette", "Colors", "community", false},
  {"theme.custom_palette", "appearance", "Custom palette", "Colors", "custom", false},
  {"theme.builtin", "appearance", "Palette", "Colors", "palette", false},
  {"theme.pure_black_dark", "appearance", "Pure black dark surfaces", "Colors", "boolean", false},
  {"shell.font_family", "appearance", "Interface font", "Typography", "string", true},
  {"shell.corner_radius_scale", "appearance", "Corner roundness", "Shape", "number", true, 0, 2},
  {"accessibility.ui_scale", "accessibility", "Interface scale", "Presentation", "number", true, 0.5, 3},
  {"accessibility.high_contrast", "accessibility", "High contrast", "Presentation", "boolean", true},
  {"shell.animation.enabled", "accessibility", "Animations", "Motion", "boolean", true},
  {"shell.animation.speed", "accessibility", "Animation speed", "Motion", "number", true, 0.1, 4},
  {"shell.panel.transparency_mode", "appearance", "Shell material", "Shell chrome", "material", true},
  {"shell.panel.glass", "appearance", "Glass amount", "Shell chrome", "number", true, 0, 1},
  {"shell.panel.refraction", "appearance", "Refraction", "Shell chrome", "boolean", true},
  {"shell.panel.borders", "appearance", "Panel outlines", "Shell chrome", "boolean", true},
  {"shell.panel.shadow", "appearance", "Panel shadows", "Shell chrome", "boolean", true},
  {"shell.panel.list_item_background", "appearance", "List item backgrounds", "Shell chrome", "boolean", true},
  {"shell.panel.floating_offset", "appearance", "Floating panel distance", "Shell chrome", "integer", true, 0, 100},
  {"shell.button_borders", "appearance", "Button outlines", "Borders", "boolean", true},
  {"shell.input_borders", "appearance", "Field outlines", "Borders", "boolean", true},
  {"shell.popup_borders", "appearance", "Menu outlines", "Borders", "boolean", true},
  {"shell.card_borders", "appearance", "Card outlines", "Borders", "boolean", true},
  {"shell.shadow.direction", "appearance", "Shadow direction", "Shadows", "shadow-direction", true},
  {"shell.shadow.alpha", "appearance", "Shadow strength", "Shadows", "number", true, 0, 1},
  {"shell.popup_shadows", "appearance", "Menu shadows", "Shadows", "boolean", true},
  {"shell.app_icon_colorize", "appearance", "Tint shell application icons", "Icons", "boolean", true},
  {"shell.app_icon_color", "appearance", "Icon tint color", "Icons", "color", true},
  {"hooks.started", "desktop-dock", "On Started", "Automation · Lifecycle", "string", true},
  {"hooks.session_locked", "desktop-dock", "On Session Locked", "Automation · Lifecycle", "string", true},
  {"hooks.session_unlocked", "desktop-dock", "On Session Unlocked", "Automation · Lifecycle", "string", true},
  {"hooks.logging_out", "desktop-dock", "On Logout", "Automation · Lifecycle", "string", true},
  {"hooks.rebooting", "desktop-dock", "On Reboot", "Automation · Lifecycle", "string", true},
  {"hooks.shutting_down", "desktop-dock", "On Shutdown", "Automation · Lifecycle", "string", true},
  {"hooks.wallpaper_changed", "desktop-dock", "On Wallpaper Changed", "Automation · Theme", "string", true},
  {"hooks.colors_changed", "desktop-dock", "On Colors Changed", "Automation · Theme", "string", true},
  {"hooks.theme_mode_changed", "desktop-dock", "On Theme Mode Changed", "Automation · Theme", "string", true},
  {"hooks.wifi_enabled", "desktop-dock", "On Wi-Fi Enabled", "Automation · Network", "string", true},
  {"hooks.wifi_disabled", "desktop-dock", "On Wi-Fi Disabled", "Automation · Network", "string", true},
  {"hooks.bluetooth_enabled", "desktop-dock", "On Bluetooth Enabled", "Automation · Network", "string", true},
  {"hooks.bluetooth_disabled", "desktop-dock", "On Bluetooth Disabled", "Automation · Network", "string", true},
  {"hooks.battery_charging", "desktop-dock", "On Battery Charging", "Automation · Power", "string", true},
  {"hooks.battery_discharging", "desktop-dock", "On Battery Discharging", "Automation · Power", "string", true},
  {"hooks.battery_plugged", "desktop-dock", "On Battery Plugged", "Automation · Power", "string", true},
  {"hooks.battery_percentage_changed", "desktop-dock", "On Battery Percentage Changed", "Automation · Power", "string", true},
  {"hooks.power_profile_changed", "desktop-dock", "On Power Profile Changed", "Automation · Power", "string", true},
  {"theme.templates.enable_builtin_templates", "appearance", "Built-in templates", "Templates", "boolean", true},
  {"theme.templates.builtin_ids", "appearance", "Built-in template selection", "Templates", "template-list", true},
  {"theme.templates.enable_community_templates", "appearance", "Community templates", "Templates", "boolean", true},
  {"theme.templates.community_ids", "appearance", "Community template selection", "Templates", "template-list", true},
  {"shell.lang", "system", "Language", "Language", "language", true},
  {"wallpaper.enabled", "appearance", "Show desktop wallpaper", "Wallpaper", "boolean", true},
  {"wallpaper.fill_mode", "appearance", "Image fit", "Wallpaper", "wallpaper-fit", true},
  {"wallpaper.fill_color", "appearance", "Fill color", "Wallpaper", "color", true},
  {"wallpaper.transition", "appearance", "Transition styles", "Wallpaper", "transition-list", true},
  {"wallpaper.directory", "appearance", "Wallpaper folder", "Wallpaper", "directory", true},
  {"wallpaper.directory_light", "appearance", "Light appearance folder", "Wallpaper", "directory", true},
  {"wallpaper.directory_dark", "appearance", "Dark appearance folder", "Wallpaper", "directory", true},
  {"wallpaper.per_monitor_directories", "appearance", "Per-display wallpaper folders", "Wallpaper", "boolean", true},
  {"wallpaper.wallhaven.download_dir", "appearance", "Wallhaven download folder", "Wallhaven", "directory", true},
  {"wallpaper.transition_duration", "appearance", "Transition duration (milliseconds)", "Wallpaper", "number", true, 100, 30000},
  {"wallpaper.transition_on_startup", "appearance", "Animate wallpaper on startup", "Wallpaper", "boolean", true},
  {"wallpaper.edge_smoothness", "appearance", "Transition edge smoothness", "Wallpaper", "number", true, 0, 1},
  {"wallpaper.automation.enabled", "appearance", "Automatically change wallpaper", "Wallpaper Automation", "boolean", true},
  {"wallpaper.automation.interval_seconds", "appearance", "Change interval (seconds)", "Wallpaper Automation", "integer", true, 1, 86400},
  {"wallpaper.automation.source", "appearance", "Wallpaper source", "Wallpaper Automation", "wallpaper-source", true},
  {"wallpaper.automation.order", "appearance", "Folder order", "Wallpaper Automation", "wallpaper-order", true},
  {"wallpaper.automation.recursive", "appearance", "Include subfolders", "Wallpaper Automation", "boolean", true},
  {"wallpaper.daily.provider", "appearance", "Daily image provider", "Wallpaper Automation", "daily-provider", true},
  {"wallpaper.daily.bing_market", "appearance", "Bing market code", "Wallpaper Automation", "string", true},
  {"wallpaper.daily.download_dir", "appearance", "Daily image folder", "Wallpaper Automation", "directory", true},
  {"wallpaper.video.directory", "appearance", "Video folder", "Video Wallpaper", "directory", true},
  {"wallpaper.video.mute", "appearance", "Mute video audio", "Video Wallpaper", "boolean", true},
  {"wallpaper.video.hardware_decode", "appearance", "Hardware decoding", "Video Wallpaper", "boolean", true},
  {"wallpaper.video.auto_pause", "appearance", "Pause video behind windows", "Video Wallpaper", "video-pause", true},
  {"wallpaper.video.mpv_options", "appearance", "Extra mpv options", "Video Wallpaper", "string", true},
  {"shell.disable_mipmaps", "appearance", "Disable texture mipmaps", "Rendering", "boolean", true},
  {"shell.shared_gl_context", "appearance", "Share OpenGL context", "Rendering", "boolean", true},
  {"location.auto_locate", "privacy-security", "Locate automatically", "Location", "boolean", true},
  {"location.address", "privacy-security", "Address", "Location", "string", true},
  {"location.latitude", "privacy-security", "Latitude", "Location", "number", true, -90, 90},
  {"location.longitude", "privacy-security", "Longitude", "Location", "number", true, -180, 180},
  {"location.custom_schedule", "privacy-security", "Use custom day and night times", "Location", "boolean", true},
  {"location.sunrise", "privacy-security", "Sunrise (HH:MM)", "Location", "string", true},
  {"location.sunset", "privacy-security", "Sunset (HH:MM)", "Location", "string", true},
  {"weather.enabled", "privacy-security", "Show weather", "Weather", "boolean", true},
  {"shell.show_location", "privacy-security", "Show location name", "Weather", "boolean", true},
  {"weather.unit", "privacy-security", "Temperature unit", "Weather", "weather-unit", true},
  {"weather.effects", "privacy-security", "Weather effects", "Weather", "boolean", true},
  {"weather.refresh_minutes", "privacy-security", "Refresh interval (minutes)", "Weather", "integer", true, 5, 240},
  {"shell.privacy.mic_filter_regex", "privacy-security", "Microphone indicator filter", "Privacy Indicators", "string", true},
  {"shell.privacy.cam_filter_regex", "privacy-security", "Camera indicator filter", "Privacy Indicators", "string", true},
  {"shell.privacy.screen_filter_regex", "privacy-security", "Screen-share indicator filter", "Privacy Indicators", "string", true},
  {"shell.avatar_path", "privacy-security", "Profile picture", "Profile", "string", true},
  {"storage.key_source", "privacy-security", "Credential storage", "Storage", "storage-key", true},
  {"storage.key_file", "privacy-security", "Credential key file", "Storage", "string", true},
  {"shell.greeter_sync.auto_sync", "privacy-security", "Sync greeter theme automatically", "Greeter", "boolean", true},
  {"shell.greeter_sync.privilege_command", "privacy-security", "Greeter sync privilege command", "Greeter", "string", true},
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
  {"dock.launcher_icon", "desktop-dock", "Launcher icon name", "Dock Launcher", "string", true},
  {"dock.launcher_custom_image", "desktop-dock", "Custom launcher image", "Dock Launcher", "string", true},
  {"dock.launcher_custom_image_colorize", "desktop-dock", "Tint custom launcher image", "Dock Launcher", "boolean", true},
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
  {"shell.launcher.auto_paste", "desktop-dock", "Paste after copy actions", "Launcher", "clipboard-paste", true},
  {"shell.launcher.provider_prefix", "desktop-dock", "Provider prefix", "Launcher", "string", true},
  {"shell.launcher.fetch_exchange_rates", "desktop-dock", "Fetch currency exchange rates", "Launcher", "boolean", true},
  {"shell.panel.launcher_placement", "desktop-dock", "Panel placement", "Launcher", "panel-placement", true},
  {"shell.panel.launcher_position", "desktop-dock", "Floating position", "Launcher", "panel-position", true},
  {"shell.panel.open_near_click_launcher", "desktop-dock", "Open near the pointer", "Launcher", "boolean", true},
  {"shell.window_switcher.mru", "desktop-dock", "Most recently used order", "Window Switcher", "boolean", true},
  {"shell.panel.clipboard_placement", "desktop-dock", "Panel placement", "Panels · Clipboard", "panel-placement", true},
  {"shell.panel.clipboard_position", "desktop-dock", "Floating position", "Panels · Clipboard", "panel-position", true},
  {"shell.panel.open_near_click_clipboard", "desktop-dock", "Open near the pointer", "Panels · Clipboard", "boolean", true},
  {"shell.panel.session_placement", "desktop-dock", "Panel placement", "Panels · Session", "panel-placement", true},
  {"shell.panel.session_position", "desktop-dock", "Floating position", "Panels · Session", "panel-position", true},
  {"shell.panel.open_near_click_session", "desktop-dock", "Open near the pointer", "Panels · Session", "boolean", true},
  {"shell.session.grid", "desktop-dock", "Use a grid layout", "Panels · Session", "boolean", true},
  {"shell.session.grid_columns", "desktop-dock", "Columns", "Panels · Session", "integer", true, 1, 5},
  {"shell.session.show_shortcuts", "desktop-dock", "Show keyboard shortcuts", "Panels · Session", "boolean", true},
  {"shell.panel.wallpaper_placement", "desktop-dock", "Panel placement", "Panels · Wallpaper", "panel-placement", true},
  {"shell.panel.wallpaper_position", "desktop-dock", "Floating position", "Panels · Wallpaper", "panel-position", true},
  {"shell.panel.open_near_click_wallpaper", "desktop-dock", "Open near the pointer", "Panels · Wallpaper", "boolean", true},
  {"shell.panel.polkit_placement", "desktop-dock", "Panel placement", "Panels · Authentication", "panel-placement", true},
  {"shell.panel.polkit_position", "desktop-dock", "Floating position", "Panels · Authentication", "panel-position", true},
  {"shell.panel.quick_insert_placement", "desktop-dock", "Panel placement", "Panels · Quick Insert", "panel-placement", true},
  {"shell.panel.quick_insert_position", "desktop-dock", "Floating position", "Panels · Quick Insert", "panel-position", true},
  {"shell.panel.open_near_click_quick_insert", "desktop-dock", "Open near the pointer", "Panels · Quick Insert", "boolean", true},
  {"shell.panel_anchor_bar", "desktop-dock", "Anchor bar", "Panels · General", "bar-anchor", true},
  {"shell.panel.floating_layer", "desktop-dock", "Floating panel layer", "Panels · General", "shell-layer", true},
  {"desktop_widgets.enabled", "desktop-dock", "Show desktop widgets", "Desktop", "boolean", true},
  {"lockscreen_widgets.enabled", "privacy-security", "Show lock-screen widgets", "Lock Screen", "boolean", true},
  {"shell.screen_corners.enabled", "desktop-dock", "Round screen corners", "Screen Corners", "boolean", true},
  {"shell.screen_corners.size", "desktop-dock", "Corner size", "Screen Corners", "integer", true, 1, 100},
  {"dock.monitors", "desktop-dock", "Dock displays", "Dock", "display-list", true},
  {"dock.layer", "desktop-dock", "Layer", "Dock", "shell-layer", true},
  {"dock.shadow", "desktop-dock", "Show shadow", "Dock", "boolean", true},
  {"dock.background_opacity", "desktop-dock", "Background opacity", "Dock Effects", "number", true, 0, 1},
  {"dock.radius", "desktop-dock", "Corner radius", "Dock Shape", "integer", true, 0, 80},
  {"dock.border", "desktop-dock", "Border color", "Dock Shape", "color", true},
  {"dock.border_width", "desktop-dock", "Border width", "Dock Shape", "number", true, 0, 20},
  {"dock.radius_top_left", "desktop-dock", "Top-left radius", "Dock Shape", "integer", true, 0, 80},
  {"dock.radius_top_right", "desktop-dock", "Top-right radius", "Dock Shape", "integer", true, 0, 80},
  {"dock.radius_bottom_left", "desktop-dock", "Bottom-left radius", "Dock Shape", "integer", true, 0, 80},
  {"dock.radius_bottom_right", "desktop-dock", "Bottom-right radius", "Dock Shape", "integer", true, 0, 80},
  {"dock.concave_edge_corners", "desktop-dock", "Concave edge corners", "Dock Shape", "boolean", true},
  {"shell.launch_apps_as_systemd_services", "desktop-dock", "Launch apps as systemd services", "Launcher", "boolean", true},
  {"shell.launch_apps_custom_command", "desktop-dock", "Custom app launch command", "Launcher", "string", true},
  {"shell.mpris.blacklist", "desktop-dock", "Hidden media players", "Media", "display-list", true},
  {"dock.active_scale", "desktop-dock", "Active icon scale", "Dock Focus", "number", true, 0.1, 1.75},
  {"dock.inactive_scale", "desktop-dock", "Inactive icon scale", "Dock Focus", "number", true, 0.1, 1},
  {"dock.active_opacity", "desktop-dock", "Active icon opacity", "Dock Focus", "number", true, 0, 1},
  {"dock.inactive_opacity", "desktop-dock", "Inactive icon opacity", "Dock Focus", "number", true, 0, 1},
  {"shell.screenshot.save_to_file", "desktop-dock", "Save screenshots to a file", "Screenshot · Output", "boolean", true},
  {"shell.screenshot.copy_to_clipboard", "desktop-dock", "Copy screenshots to the clipboard", "Screenshot · Output", "boolean", true},
  {"shell.screenshot.directory", "desktop-dock", "Screenshot folder", "Screenshot · Output", "directory", true},
  {"shell.screenshot.filename_pattern", "desktop-dock", "Filename pattern", "Screenshot · Output", "string", true},
  {"shell.screenshot.pipe_to_command", "desktop-dock", "Pipe to a command", "Screenshot · Output", "boolean", true},
  {"shell.screenshot.pipe_command", "desktop-dock", "Pipe command", "Screenshot · Output", "string", true},
  {"shell.screenshot.freeze_screen", "desktop-dock", "Freeze the screen while selecting", "Screenshot · Capture", "boolean", true},
  {"shell.screenshot.confirm_region", "desktop-dock", "Confirm the selected region", "Screenshot · Capture", "boolean", true},
  {"shell.screenshot.remember_last_region", "desktop-dock", "Remember the last region", "Screenshot · Capture", "boolean", true},
  {"shell.screenshot.show_cursor", "desktop-dock", "Include the pointer", "Screenshot · Capture", "boolean", true},
  {"shell.screenshot.annotate", "desktop-dock", "Open annotation editor", "Screenshot · Annotation", "boolean", true},
  {"shell.screenshot.skip_annotate_on_copy_save", "desktop-dock", "Skip annotation for copy and save", "Screenshot · Annotation", "boolean", true},
  {"shell.screenshot.close_on_copy", "desktop-dock", "Close after copying", "Screenshot · Annotation", "boolean", true},
  {"shell.screenshot.close_on_save", "desktop-dock", "Close after saving", "Screenshot · Annotation", "boolean", true},
  {"shell.screen_record.audio", "desktop-dock", "Record audio", "Screen Record", "boolean", true},
  {"shell.screen_record.show_cursor", "desktop-dock", "Include the pointer", "Screen Record", "boolean", true},
  {"shell.screen_record.directory", "desktop-dock", "Recording folder", "Screen Record", "directory", true},
  {"shell.screen_record.filename_pattern", "desktop-dock", "Filename pattern", "Screen Record", "string", true},
  {"shell.screen_record.replay_duration", "desktop-dock", "Replay buffer (seconds)", "Screen Record", "integer", true, 2, 600},
  {"theme.niri_layout.gaps", "desktop-dock", "Window gaps", "Niri Layout", "integer", true, 0, 64},
  {"theme.niri_layout.struts", "desktop-dock", "Outer padding", "Niri Layout", "integer", true, 0, 64},
  {"theme.niri_layout.default_column_width", "desktop-dock", "Default column width", "Niri Layout", "number", true, 0.15, 1},
  {"theme.niri_layout.overview", "desktop-dock", "Overview style", "Niri Layout", "niri-overview", true},
  {"theme.niri_layout.focus_ring", "desktop-dock", "Focus ring", "Niri Chrome", "boolean", true},
  {"theme.niri_layout.border", "desktop-dock", "Window border", "Niri Chrome", "boolean", true},
  {"theme.niri_layout.draw_border_with_background", "desktop-dock", "Draw border with background", "Niri Chrome", "boolean", true},
  {"theme.niri_layout.blur", "desktop-dock", "Window blur", "Niri Chrome", "boolean", true},
  {"theme.niri_layout.blur_passes", "desktop-dock", "Blur passes", "Niri Chrome", "integer", true, 1, 8},
  {"theme.niri_layout.blur_offset", "desktop-dock", "Blur offset", "Niri Chrome", "number", true, 0.5, 8},
  {"theme.niri_layout.blur_noise", "desktop-dock", "Blur noise", "Niri Chrome", "number", true, 0, 0.2},
  {"shell.niri_overview_type_to_launch_enabled", "desktop-dock", "Type to launch in overview", "Niri Layout", "boolean", true},
  {"audio.enable_sounds", "sound", "Interface sounds", "Sounds", "boolean", false},
  {"audio.sound_volume", "sound", "Sound volume", "Sounds", "number", false, 0, 1},
  {"audio.enable_overdrive", "sound", "Allow volume above 100%", "Sounds", "boolean", false},
  {"audio.sound_theme", "sound", "Sound theme", "Sounds", "string", false},
  {"brightness.enable_ddcutil", "displays", "Use DDC/CI brightness", "Brightness", "boolean", true},
  {"brightness.sync_all_monitors", "displays", "Sync brightness across displays", "Brightness", "boolean", true},
  {"brightness.minimum_brightness", "displays", "Minimum brightness", "Brightness", "number", true, 0, 1},
  {"brightness.ignore_mmids", "displays", "Ignored DDC monitor IDs", "Brightness", "display-list", true},
  {"control_center.network.wifi_band", "network", "Wi-Fi band filter", "Wi-Fi Preferences", "wifi-band", true},
  {"control_center.network.hidden_ssids", "network", "Hidden networks", "Wi-Fi Preferences", "display-list", true},
  {"control_center.network.custom_dns", "network", "Custom DNS servers", "DNS", "string", true},
  {"control_center.network.nextdns_id", "network", "NextDNS configuration ID", "DNS", "string", true},
  {"control_center.bluetooth.hide_unnamed", "bluetooth", "Hide unnamed devices", "Discovery", "boolean", true},
  {"control_center.bluetooth.discovery_seconds", "bluetooth", "Discovery duration (seconds)", "Discovery", "integer", true, 5, 60},
  {"shell.time_format", "system", "Time format", "Clock", "string", true},
  {"shell.date_format", "system", "Date format", "Clock", "string", true},
  {"calendar.enabled", "system", "Show calendar events", "Calendar", "boolean", true},
  {"calendar.refresh_minutes", "system", "Calendar refresh (minutes)", "Calendar", "integer", true, 5, 240},
  {"calendar.event_date_format", "system", "Event date format", "Calendar", "string", true},
  {"calendar.event_time_format", "system", "Event time format", "Calendar", "string", true},
  {"shell.setup_wizard_enabled", "system", "Show setup wizard", "Setup", "boolean", true},
  {"shell.settings_show_advanced", "system", "Show advanced settings", "Settings", "boolean", true},
  {"notification.filter_replace", "notifications", "Replace default notification rules", "Application Rules", "boolean", true},
  {"tailscale.refresh_interval_seconds", "network", "Tailscale refresh (seconds)", "Tailscale", "integer", true, 3, 120},
  {"tailscale.notify_on_peer_change", "network", "Notify when Tailscale peers change", "Tailscale", "boolean", true},
  {"tailscale.admin_url", "network", "Tailscale admin URL", "Tailscale", "string", true},
  {"shell.session.power.suspend", "power", "Suspend command", "Session Commands", "string", true},
  {"shell.session.power.reboot", "power", "Restart command", "Session Commands", "string", true},
  {"shell.session.power.shutdown", "power", "Shut down command", "Session Commands", "string", true},
};
constexpr std::pair<std::string_view, std::string_view> controlCenterTabs[] = {
  {"network", "Network"}, {"bluetooth", "Bluetooth"}, {"tailscale", "Tailscale"},
  {"notifications", "Notifications"}, {"audio", "Sound"}, {"screen-time", "Screen Time"}, {"theme", "Appearance"},
  {"control-center", "Control Center"}, {"launcher", "Launcher"}, {"privacy-security", "Privacy & Security"},
  {"system", "System Monitor"}, {"packages", "Software"}, {"clock", "Clock"}, {"desktop", "Desktop & Dock"},
  {"monitor", "Displays"}, {"wallpaper", "Wallpaper"}, {"power", "Power"}, {"keybinds", "Keyboard"},
  {"shell", "Shell"}, {"printers", "Printers"}, {"camera", "Camera"}, {"calendar", "Calendar"}, {"weather", "Weather"},
  {"media", "Media"}, {"capture", "Capture"}, {"clipboard", "Clipboard"}, {"ai", "AI Usage"},
  {"templates", "Templates"}, {"hooks", "Automation"}, {"niri", "Niri"},
};
const Setting* find(std::string_view id) {
  for (const auto& setting : settings) if (setting.id == id) return &setting;
  return nullptr;
}

Json inferenceRoutingMirrorValue(std::string_view id, const inference::RoutingTable& routing) {
  if (id == "ai.act_model") return routing.route("act").model;
  if (id == "ai.chat_model") return routing.route("chat").model;
  if (id == "ai.allow_remote_on_battery_saver") return routing.allowRemoteOnBatterySaver;
  if (id == "ai.lan_endpoint") {
    for (const auto& backend : routing.backends)
      if (backend.kind == "openai" && !backend.baseUrl.empty()) return backend.baseUrl;
    return "";
  }
  return Json{};
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
      if (field == "enabled" || field == "position" || field == "auto_hide" || field == "smart_auto_hide" || field == "reserve_space" || field == "show_on_workspace_switch" || field == "shadow" || field == "contact_shadow" || field == "thickness" || field == "layer" || field == "scale" || field == "font_scale" || field == "font_weight" || field == "font_family" || field == "panel_overlap" || field == "margin_ends" || field == "margin_edge" || field == "margin_opposite_edge" || field == "padding" || field == "background_opacity" || field == "radius" || field == "border_width" || field == "concave_edge_corners" || field == "widget_spacing" || field == "start" || field == "center" || field == "end")
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
Json settingsCatalog(ConfigService& config, bool helixDesktop, bool gammaAvailable) {
  auto table = config_export::serialize(config.config());
  const inference::RoutingTable inferenceRouting = inference::loadRouting();
  Json result = Json::array();
  for (const auto& setting : settings) {
    if (setting.desktop && !helixDesktop) continue;
    if (std::string_view(setting.id).starts_with("nightlight.") && !gammaAvailable) continue;
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
    if (std::string_view(setting.id).starts_with("ai.")) {
      Json value = inferenceRoutingMirrorValue(setting.id, inferenceRouting);
      if (value.is_null()) continue;
      result.push_back({{"id", setting.id}, {"page", setting.page}, {"title", setting.title}, {"group", setting.group},
        {"type", setting.type}, {"value", value}, {"options", Json::array()}, {"minimum", setting.minimum},
        {"maximum", setting.maximum}, {"overridden", false}});
      continue;
    }
    const toml::node* node = &table;
    for (const auto& part : path) { auto* parent = node ? node->as_table() : nullptr; node = parent ? parent->get(part) : nullptr; }
    Json value;
    if (!node && std::string_view(setting.id) == "shell.panel.glass") value = static_cast<double>(resolvedPanelGlass(config.config().shell.panel));
    else if (!node && (std::string_view(setting.id) == "shell.lang" || std::string_view(setting.id) == "shell.app_icon_color" || std::string_view(setting.id) == "shell.panel_anchor_bar" || std::string_view(setting.id) == "dock.launcher_custom_image" || std::string_view(setting.id) == "lockscreen.wallpaper" || std::string_view(setting.id) == "shell.screenshot.directory" || std::string_view(setting.id) == "shell.screenshot.filename_pattern" || std::string_view(setting.id) == "shell.screenshot.pipe_command" || std::string_view(setting.id) == "shell.screen_record.directory" || std::string_view(setting.id) == "shell.screen_record.filename_pattern" || std::string_view(setting.id) == "wallpaper.daily.bing_market" || std::string_view(setting.id) == "wallpaper.video.mpv_options" || std::string_view(setting.id) == "wallpaper.fill_color" || std::string_view(setting.id) == "location.address" || std::string_view(setting.id) == "location.sunrise" || std::string_view(setting.id) == "location.sunset" || std::string_view(setting.id) == "shell.privacy.mic_filter_regex" || std::string_view(setting.id) == "shell.privacy.cam_filter_regex" || std::string_view(setting.id) == "shell.privacy.screen_filter_regex" || std::string_view(setting.id) == "shell.avatar_path" || std::string_view(setting.id) == "storage.key_file" || std::string_view(setting.id) == "shell.greeter_sync.privilege_command" || std::string_view(setting.id) == "shell.launch_apps_custom_command" || std::string_view(setting.id) == "control_center.network.custom_dns" || std::string_view(setting.id) == "control_center.network.nextdns_id" || std::string_view(setting.id) == "tailscale.admin_url" || std::string_view(setting.id) == "shell.session.power.suspend" || std::string_view(setting.id) == "shell.session.power.reboot" || std::string_view(setting.id) == "shell.session.power.shutdown" || std::string_view(setting.id).starts_with("hooks."))) value = "";
    else if (!node && std::string_view(setting.id) == "location.latitude") value = config.config().location.latitude.value_or(0.0);
    else if (!node && std::string_view(setting.id) == "location.longitude") value = config.config().location.longitude.value_or(0.0);
    else if (!node && std::string_view(setting.id) == "wallpaper.transition") value = Json::array({"fade", "wipe", "disc", "stripes", "zoom", "honeycomb"});
    else if (!node && (std::string_view(setting.id) == "brightness.ignore_mmids" || std::string_view(setting.id) == "control_center.network.hidden_ssids" || std::string_view(setting.id) == "shell.mpris.blacklist")) value = Json::array();
    else if (!node && std::string_view(setting.id) == "theme.niri_layout.gaps") value = 16;
    else if (!node && std::string_view(setting.id) == "theme.niri_layout.struts") value = 0;
    else if (!node && std::string_view(setting.id) == "theme.niri_layout.default_column_width") value = 0.5;
    else if (!node && std::string_view(setting.id) == "theme.niri_layout.draw_border_with_background") value = true;
    else if (!node) continue;
    else if (auto* b = node->as_boolean()) value = b->get();
    else if (auto* i = node->as_integer()) value = i->get();
    else if (auto* d = node->as_floating_point()) value = d->get();
    else if (auto* text = node->as_string()) value = text->get();
    else if (auto* array = node->as_array()) {
      if (std::string_view(setting.type) == "string" && std::string_view(setting.id).starts_with("hooks.")) {
        value = "";
        for (const auto& item : *array) if (item.is_string() && !item.as_string()->get().empty()) { value = item.as_string()->get(); break; }
      } else {
        value = Json::array(); for (const auto& item : *array) if (item.is_string()) value.push_back(item.as_string()->get());
      }
    } else continue;
    Json options = Json::array();
    std::string_view type = setting.type;
    if (type == "mode") options = {"dark", "light", "auto"};
    if (type == "source") {
      options = {"builtin"};
      if (helixDesktop) options.push_back("wallpaper");
      if (!helix::theme::availableCommunityPalettes().empty()) options.push_back("community");
      if (!helix::theme::availableCustomPalettes().empty()) options.push_back("custom");
    }
    if (type == "community") for (const auto& palette : helix::theme::availableCommunityPalettes()) options.push_back(palette.name);
    if (type == "custom") for (const auto& palette : helix::theme::availableCustomPalettes()) options.push_back(palette.name);
    if ((type == "community" || type == "custom") && options.empty()) continue;
    if (type == "shell-layer") options = {"top", "overlay"};
    if (type == "shell-mode") options = {"follow", "dark", "light", "auto"};
    if (type == "wallpaper-scheme") options = {"m3-content", "m3-tonal-spot", "m3-fruit-salad", "m3-rainbow", "m3-monochrome"};
    if (type == "weather-unit") options = {"metric", "imperial"};
    if (type == "bar-anchor") {
      options = Json::array(); options.push_back("");
      for (const auto& bar : config.config().bars) options.push_back(bar.name);
      if (value.is_string() && !value.get_ref<const std::string&>().empty() && std::ranges::find(options, value) == options.end()) options.push_back(value);
    }
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
    if (type == "wifi-band") options = {"all", "2.4", "5", "6"};
    if (type == "storage-key") options = {"secret-service", "file"};
    if (type == "transition-list") options = {"fade", "wipe", "disc", "stripes", "zoom", "honeycomb"};
    if (type == "hot-corner-action") options = {"none", "launcher", "control_center", "window_switcher", "command"};
    if (type == "clipboard-paste") options = {"off", "auto", "ctrl_v", "ctrl_shift_v", "shift_insert"};
    if (type == "password-mask") options = {"default", "random"};
    if (type == "material") options = {"solid", "soft", "glass"};
    if (type == "niri-overview") options = {"none", "backdrop", "stationary", "flat"};
    if (type == "shadow-direction") options = {"center", "down", "up", "left", "right", "down_left", "down_right", "up_left", "up_right"};
    if (type == "language") {
      options = Json::array(); options.push_back("");
      for (const auto& language : i18n::kSupportedLanguages) options.push_back(std::string(language.code));
      if (value.is_string() && !value.get_ref<const std::string&>().empty() && std::ranges::find(options, value) == options.end()) options.push_back(value);
    }
    if (type == "template-list") {
      const auto templates = std::string_view(setting.id) == "theme.templates.community_ids"
          ? helix::theme::CommunityTemplateService::availableTemplates() : helix::theme::availableTemplates();
      for (const auto& item : templates) options.push_back(item.id);
      if (value.is_array()) for (const auto& item : value) if (item.is_string() && std::ranges::find(options, item) == options.end()) options.push_back(item);
    }
    if (type == "position") options = {"bottom", "left", "right", "top"};
    if (type == "color") {
      options = Json::array(); options.push_back("");
      for (const auto& token : kColorRoleTokens) options.push_back(std::string(token.token));
      if (value.is_string() && !value.get_ref<const std::string&>().empty() && std::ranges::find(options, value) == options.end()) options.push_back(value);
    }
    if (type == "palette") for (const auto& palette : helix::theme::builtinPalettes()) options.push_back(palette.name);
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
    if (type == "niri-overview") labels = {"None", "Backdrop", "Stationary wallpaper", "Flat color"};
    if (type == "shadow-direction") labels = {"Center", "Down", "Up", "Left", "Right", "Down left", "Down right", "Up left", "Up right"};
    if (type == "language") {
      labels = Json::array(); labels.push_back("Automatic");
      for (const auto& language : i18n::kSupportedLanguages) labels.push_back(std::string(language.displayName));
      if (labels.size() < options.size()) labels.push_back(options.back());
    }
    if (type == "template-list") {
      labels = Json::array();
      const auto templates = std::string_view(setting.id) == "theme.templates.community_ids"
          ? helix::theme::CommunityTemplateService::availableTemplates() : helix::theme::availableTemplates();
      std::map<std::string, std::string> names;
      for (const auto& item : templates) names.emplace(item.id, item.displayName.empty() ? item.id : item.displayName);
      for (const auto& option : options) {
        const auto id = option.get<std::string>();
        labels.push_back(names.contains(id) ? names.at(id) : id);
      }
    }
    if (type == "position") labels = {"Bottom", "Left", "Right", "Top"};
    if (type == "color") {
      labels = Json::array(); labels.push_back("Default");
      for (const auto& token : kColorRoleTokens) {
        std::string label(token.token);
        for (char& c : label) if (c == '_') c = ' ';
        if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
        labels.push_back(label);
      }
      if (labels.size() < options.size()) labels.push_back(options.back());
    }
    if (type == "wallpaper-source") labels = {"Folder", "Daily image"};
    if (type == "wallpaper-order") labels = {"Random", "Alphabetical"};
    if (type == "daily-provider") labels = {"Bing", "NASA"};
    if (type == "video-pause") labels = {"Never", "Maximized windows", "Fullscreen windows"};
    if (type == "wifi-band") labels = {"All bands", "2.4 GHz", "5 GHz", "6 GHz"};
    if (type == "storage-key") labels = {"Secret Service", "Key file"};
    if (type == "transition-list") labels = {"Fade", "Wipe", "Disc", "Stripes", "Zoom", "Honeycomb"};
    if (type == "hot-corner-action") labels = {"None", "Launcher", "Control Center", "Window switcher", "Custom command"};
    if (type == "password-mask") labels = {"Filled circles", "Random icons"};
    if (type == "shell-layer") labels = {"Normal", "Above fullscreen windows"};
    if (type == "shell-mode") labels = {"Follow appearance", "Dark", "Light", "Automatic"};
    if (type == "wallpaper-scheme") labels = {"Content", "Tonal spot", "Fruit salad", "Rainbow", "Monochrome"};
    if (type == "weather-unit") labels = {"Celsius", "Fahrenheit"};
    if (type == "bar-anchor") {
      labels = Json::array(); labels.push_back("Automatic");
      for (std::size_t i = 1; i < options.size(); ++i) labels.push_back(options.at(i));
    }
    if (type == "sidebar-mode") labels = {"Full sidebar", "Compact sidebar", "No sidebar"};
    if (type == "panel-placement") labels = {"Attached to bar", "Floating"};
    if (type == "panel-position") labels = {"Automatic", "Center", "Top left", "Top center", "Top right", "Center left", "Center right", "Bottom left", "Bottom center", "Bottom right"};
    if (type == "clipboard-paste") labels = {"Off", "Automatic", "Ctrl+V", "Ctrl+Shift+V", "Shift+Insert"};
    if (type == "orientation") labels = {"Horizontal", "Vertical"};
    if (type == "screen-position") labels = {"Top left", "Top center", "Top right", "Bottom left", "Bottom center", "Bottom right"};
    if (type == "dock-launcher") labels = {"Hidden", "Start", "End"};
    if (type == "wallpaper-fit") labels = {"Center", "Fill", "Fit", "Stretch", "Tile", "Span displays"};
    result.push_back({{"id", setting.id}, {"page", setting.page}, {"title", setting.title},
      {"group", setting.group}, {"type", setting.type}, {"value", value}, {"options", options}, {"option-labels", labels},
      {"minimum", setting.minimum}, {"maximum", setting.maximum}, {"overridden", config.hasOverride(path)}});
  }
  for (auto& entry : result) {
    const auto id = entry.at("id").get<std::string>();
    if (id == "nightlight.temperature_day" || id == "nightlight.temperature_night") entry["step"] = 100;
    if (id == "shell.clipboard_history_max_entries") entry["step"] = helix::config::kClipboardHistoryStepEntries;
    if (id == "nightlight.enabled") entry["description"] = "Use warmer display colors through the Helix compositor's gamma-control capability.";
    if (id == "nightlight.force") entry["description"] = "Use the night temperature all day. Otherwise Night Light follows the existing location-based schedule.";
    if (id == "nightlight.temperature_day" || id == "nightlight.temperature_night") entry["description"] = "Lower values are warmer. Temperatures stay at least 100 K apart; changing one adjusts the other when necessary. Reset restores both configured temperatures.";
    if (id == "control_center.sidebar") entry["description"] = "Controls the navigation sidebar in the full Control Center. The quick Home panel stays compact.";
    if (id == "control_center.sidebar_section") entry["description"] = "Controls the sidebar when opening a detail view directly from a bar control or command.";
    if (id == "control_center.width") entry["description"] = "Maximum width of the full Control Center. Compact and sidebar-free views scale from this value.";
    if (id == "control_center.height") entry["description"] = "Maximum height of the Control Center. Scrollable content remains accessible when it does not fit.";
    if (id == "control_center.show_session_button") entry["description"] = "Shows Lock, Suspend and Power actions on Home.";
    if (id == "control_center.shortcuts") entry["description"] = "Order of the Quick Settings tile grid (everyday toggles and pins). Hold-drag in the dropdown also reorders this list.";
    if (id == "shell.panel.control_center_placement") entry["description"] = "Attached follows the bar; Floating uses a screen position.";
    if (id == "shell.panel.control_center_position") entry["description"] = "Used only when Control Center is floating. Automatic follows the bar.";
    if (id == "shell.panel.open_near_click_control_center") entry["description"] = "When floating, opens Control Center or Notification Center near the click instead of the chosen screen anchor where supported.";
    if (id == "hot_corners.delay_ms") entry["description"] = "Keep the pointer in a corner for this long before triggering its action. Zero triggers immediately.";
    if (id.starts_with("hot_corners.") && id.ends_with(".command")) entry["description"] = "Runs when hot corners are enabled and this corner uses Custom command. Saving does not run the command.";

    if (entry.at("id").get<std::string>().starts_with("ai."))
      entry["description"] = "Managed in Mobius (Models & routing). Helix Settings lists this for search; edit inference-routing.toml via Mobius, not Configuration1.";
    if (entry.at("id") == "shell.offline_mode") entry["description"] = "Blocks Helix Shell HTTP requests. Does not disconnect the network or restrict other applications.";
    if (entry.at("id") == "shell.external_ip_enabled") entry["description"] = "Uses an external service to resolve your public address. Unavailable while shell network requests are restricted.";
    if (entry.at("id") == "shell.telemetry_enabled") entry["description"] = "Sends an anonymous ping when Helix Shell starts. Unavailable while shell network requests are restricted.";
    if (entry.at("id") == "shell.clipboard_enabled") entry["description"] = "Retain copied items in Helix clipboard history. Turning this off stops history retention; ordinary copy and paste remain available.";
    if (entry.at("id") == "shell.clipboard_keep_from_closed_apps") entry["description"] = "Keep the current copied item available after its source app exits, even when history retention is off.";
    if (entry.at("id") == "shell.clipboard_history_max_entries") entry["description"] = "Maximum unpinned history entries. Pinned items are exempt. Used while history retention is enabled.";
    if (entry.at("id") == "shell.clipboard_confirm_clear_history") entry["description"] = "Ask before clearing history or deleting an unpinned item. Pinned items always require confirmation.";
    if (entry.at("id") == "shell.clipboard_auto_paste") entry["description"] = "Paste an item selected from Helix clipboard history into the active app. Off copies it without simulating a paste shortcut.";
    if (entry.at("id") == "shell.clipboard_image_action_command") entry["description"] = "Command offered for image history entries. Use {path} for the image file, {stdin} for standard input, or leave blank to disable the action.";
    if (entry.at("id") == "shell.screen_time_enabled") entry["description"] = "Records focused-application usage locally while Helix is running. Saved reports are available in Helix System Monitor. Disabling recording preserves existing history.";
    if (entry.at("id") == "lockscreen.fingerprint") entry["description"] = "Allows fingerprint unlock when a configured reader and fprintd are available. Password unlock remains available.";
    if (entry.at("id") == "shell.polkit_agent") entry["description"] = "Shows Helix authentication dialogs for privileged actions. Turning this off leaves other agents to handle prompts.";
    if (entry.at("id") == "lockscreen.enabled") entry["description"] = "Shows the Helix lock screen when the session locks. Session locking itself remains available.";
    if (entry.at("id") == "lockscreen.allow_empty_password") entry["description"] = "Allows unlocking accounts that have no password set. Prefer leaving this off on shared devices.";
    if (entry.at("id") == "lockscreen.wallpaper") entry["description"] = "Absolute path to a lock-screen image. Leave blank to use the desktop wallpaper or blurred desktop.";
    if (entry.at("id") == "lockscreen.blurred_desktop") entry["description"] = "Uses compositor screen capture where supported by helix-niri.";
    if (entry.at("id") == "lockscreen.monitors") entry["description"] = "Selects lock-screen presentation displays. This does not disable session locking. An empty list uses all displays.";
    if (entry.at("id") == "dock.border_width") entry["description"] = "Width of the inside outline around the dock.";
    if (entry.at("id") == "dock.border") entry["description"] = "Color of the inside outline around the dock.";
    if (entry.at("id") == "dock.radius_top_left" || entry.at("id") == "dock.radius_top_right" || entry.at("id") == "dock.radius_bottom_left" || entry.at("id") == "dock.radius_bottom_right") entry["description"] = "Corner radius for this dock corner. Combined with the shared corner radius when shaping the dock.";
    if (entry.at("id") == "dock.concave_edge_corners") entry["description"] = "Carves concave corners on the side that touches the screen edge.";
    if (entry.at("id") == "shell.screenshot.directory") entry["description"] = "Folder for saved screenshots. Leave blank to use the Pictures folder.";
    if (entry.at("id") == "shell.screenshot.filename_pattern") entry["description"] = "strftime pattern for screenshot filenames. Leave blank for the default timestamped name.";
    if (entry.at("id") == "shell.screenshot.pipe_command") entry["description"] = "Command that receives screenshot image data when Pipe to a command is enabled.";
    if (entry.at("id") == "shell.screen_record.directory") entry["description"] = "Folder for saved recordings. Leave blank to use Videos/Recordings.";
    if (entry.at("id") == "shell.screen_record.filename_pattern") entry["description"] = "strftime pattern for recording filenames. Leave blank for the default timestamped name.";
    if (entry.at("id") == "shell.screen_record.replay_duration") entry["description"] = "Length of the gpu-screen-recorder replay buffer in seconds.";
    if (entry.at("id") == "theme.shell_mode") entry["description"] = "Appearance of Helix Shell chrome. Follow appearance uses the shared theme mode.";
    if (entry.at("id") == "theme.wallpaper_scheme") entry["description"] = "Material palette algorithm used when colors are generated from wallpaper.";
    if (entry.at("id") == "backdrop.enabled") entry["description"] = "Dims and blurs the wallpaper behind floating windows where helix-niri supports it.";
    if (entry.at("id") == "backdrop.blur_intensity") entry["description"] = "Blur strength for the wallpaper backdrop.";
    if (entry.at("id") == "backdrop.tint_intensity") entry["description"] = "Tint strength for the wallpaper backdrop.";
    if (entry.at("id") == "wallpaper.fill_color") entry["description"] = "Background color shown around wallpapers that do not fill the display. Leave blank for transparent.";
    if (entry.at("id") == "wallpaper.transition") entry["description"] = "Transition styles used when the wallpaper changes. At least one style is required.";
    if (entry.at("id") == "wallpaper.per_monitor_directories") entry["description"] = "Uses per-display wallpaper folders when configured. Advanced per-display pickers are not in Settings yet.";
    if (entry.at("id") == "wallpaper.edge_smoothness") entry["description"] = "Softens transition edges between wallpapers.";
    if (entry.at("id") == "wallpaper.daily.provider") entry["description"] = "Source used when wallpaper automation is set to Daily image.";
    if (entry.at("id") == "wallpaper.daily.bing_market") entry["description"] = "Bing market code such as en-US. Leave blank for the default market.";
    if (entry.at("id") == "wallpaper.video.mpv_options") entry["description"] = "Extra mpv options for video wallpapers. Leave blank for defaults.";
    if (entry.at("id") == "wallpaper.video.auto_pause") entry["description"] = "Pauses video wallpapers when windows cover the desktop.";
    if (entry.at("id") == "shell.disable_mipmaps") entry["description"] = "Disables texture mipmaps in Helix Shell. Leave off unless diagnosing rendering issues.";
    if (entry.at("id") == "shell.shared_gl_context") entry["description"] = "Shares one OpenGL context across Helix Shell surfaces. Leave on unless diagnosing graphics issues.";
    if (entry.at("id") == "location.auto_locate") entry["description"] = "Resolves coordinates from public IP geolocation when shell network requests are allowed.";
    if (entry.at("id") == "location.address") entry["description"] = "Address geocoded when automatic location is off. Leave blank to use latitude and longitude.";
    if (entry.at("id") == "location.latitude" || entry.at("id") == "location.longitude") entry["description"] = "Manual coordinates used when automatic location and address lookup are unavailable.";
    if (entry.at("id") == "location.custom_schedule") entry["description"] = "Uses the sunrise and sunset times below instead of coordinates for day and night.";
    if (entry.at("id") == "location.sunrise" || entry.at("id") == "location.sunset") entry["description"] = "Local time in HH:MM form. Used only when custom day and night times are enabled.";
    if (entry.at("id") == "weather.enabled") entry["description"] = "Fetches weather for the configured location when shell network requests are allowed.";
    if (entry.at("id") == "shell.show_location") entry["description"] = "Shows the resolved location name with weather surfaces.";
    if (entry.at("id") == "weather.unit") entry["description"] = "Temperature unit for Helix weather surfaces.";
    if (entry.at("id") == "weather.effects") entry["description"] = "Shows weather motion effects where supported.";
    if (entry.at("id") == "weather.refresh_minutes") entry["description"] = "How often weather data is refreshed.";
    if (entry.at("id") == "shell.privacy.mic_filter_regex" || entry.at("id") == "shell.privacy.cam_filter_regex" || entry.at("id") == "shell.privacy.screen_filter_regex") entry["description"] = "Regular expression matching process names to hide from this privacy indicator. Leave blank to show all.";
    if (entry.at("id") == "shell.avatar_path") entry["description"] = "Absolute path to a profile image. Leave blank to use the account avatar.";
    if (entry.at("id") == "storage.key_source") entry["description"] = "Where Helix stores encrypted credentials. Key file requires a path below.";
    if (entry.at("id") == "storage.key_file") entry["description"] = "Absolute path to the credential key file when Credential storage is Key file.";
    if (entry.at("id") == "shell.greeter_sync.auto_sync") entry["description"] = "Copies Helix appearance into the greeter when supported.";
    if (entry.at("id") == "shell.greeter_sync.privilege_command") entry["description"] = "Privilege helper used for greeter sync. Leave blank for the default escalator.";
    if (entry.at("id") == "shell.launch_apps_as_systemd_services") entry["description"] = "Starts launched applications as transient systemd user services when possible.";
    if (entry.at("id") == "shell.launch_apps_custom_command") entry["description"] = "Optional command wrapper for launched applications. Use {command} for the application command. Leave blank for the default launcher.";
    if (entry.at("id") == "shell.mpris.blacklist") entry["description"] = "Media player identities hidden from Helix media controls.";
    if (entry.at("id") == "audio.enable_overdrive") entry["description"] = "Allows PipeWire volume above 100% when the audio backend supports it.";
    if (entry.at("id") == "audio.sound_theme") entry["description"] = "XDG sound theme name for interface sounds.";
    if (entry.at("id") == "brightness.enable_ddcutil") entry["description"] = "Uses ddcutil for external display brightness when available.";
    if (entry.at("id") == "brightness.sync_all_monitors") entry["description"] = "Applies brightness changes to every connected display at once.";
    if (entry.at("id") == "brightness.minimum_brightness") entry["description"] = "Lowest brightness Settings and shell controls will set.";
    if (entry.at("id") == "brightness.ignore_mmids") entry["description"] = "DDC monitor model identifiers skipped by ddcutil brightness control.";
    if (entry.at("id") == "control_center.network.wifi_band") entry["description"] = "Limits which Wi-Fi bands appear while scanning.";
    if (entry.at("id") == "control_center.network.hidden_ssids") entry["description"] = "SSIDs to probe even when they are not broadcasting.";
    if (entry.at("id") == "control_center.network.custom_dns") entry["description"] = "Comma-separated DNS servers applied through Helix network controls. Leave blank for automatic DNS.";
    if (entry.at("id") == "control_center.network.nextdns_id") entry["description"] = "NextDNS configuration ID used when NextDNS DNS is selected.";
    if (entry.at("id") == "control_center.bluetooth.hide_unnamed") entry["description"] = "Hides Bluetooth devices that advertise no name.";
    if (entry.at("id") == "control_center.bluetooth.discovery_seconds") entry["description"] = "How long Bluetooth discovery runs after Find Devices.";
    if (entry.at("id") == "shell.time_format") entry["description"] = "strftime or chrono format string for Helix Shell clocks.";
    if (entry.at("id") == "shell.date_format") entry["description"] = "strftime format string for Helix Shell dates.";
    if (entry.at("id") == "calendar.enabled") entry["description"] = "Shows connected calendar events in Helix calendar surfaces.";
    if (entry.at("id") == "calendar.refresh_minutes") entry["description"] = "How often calendar accounts are refreshed.";
    if (entry.at("id") == "calendar.event_date_format") entry["description"] = "strftime format for calendar event dates.";
    if (entry.at("id") == "calendar.event_time_format") entry["description"] = "strftime format for calendar event times.";
    if (entry.at("id") == "shell.setup_wizard_enabled") entry["description"] = "Offers the first-run setup wizard when Helix detects a new session.";
    if (entry.at("id") == "shell.settings_show_advanced") entry["description"] = "Shows advanced configuration groups in Helix Settings.";
    if (entry.at("id") == "notification.filter_replace") entry["description"] = "Replaces the built-in notification rules with only the rules listed below.";
    if (entry.at("id") == "tailscale.refresh_interval_seconds") entry["description"] = "How often Tailscale peer status is refreshed.";
    if (entry.at("id") == "tailscale.notify_on_peer_change") entry["description"] = "Shows a notification when Tailscale peers connect or disconnect.";
    if (entry.at("id") == "tailscale.admin_url") entry["description"] = "URL opened by Tailscale admin actions. Leave blank for the default admin console.";
    if (entry.at("id") == "shell.session.power.suspend" || entry.at("id") == "shell.session.power.reboot" || entry.at("id") == "shell.session.power.shutdown") entry["description"] = "Optional shell command override. Leave blank to auto-detect the session backend.";
    if (id == "shell.shadow.alpha" || id == "shell.panel.glass" || id == "dock.background_opacity" || id == "dock.active_opacity" || id == "dock.inactive_opacity" || id == "osd.background_opacity" || id == "backdrop.blur_intensity" || id == "backdrop.tint_intensity" || id == "wallpaper.edge_smoothness") entry["step"] = 0.01;
    if (id == "dock.border_width") entry["step"] = 0.5;
    if (id == "location.latitude" || id == "location.longitude") entry["step"] = 0.0001;
    if (id == "shell.corner_radius_scale" || id == "shell.animation.speed") entry["step"] = 0.05;
    if (id == "theme.niri_layout.default_column_width" || id == "dock.active_scale" || id == "dock.inactive_scale") entry["step"] = 0.05;
    if (id == "theme.niri_layout.blur_offset") entry["step"] = 0.1;
    if (id == "theme.niri_layout.blur_noise") entry["step"] = 0.01;
    if (entry.at("id") == "shell.font_family") entry["description"] = "Changes Helix Shell text. Application typography follows this device’s native defaults.";
    if (entry.at("id") == "shell.corner_radius_scale") entry["description"] = "Scales corner radius across Helix Shell chrome. Zero is square; two is twice the standard radius.";
    if (entry.at("id") == "shell.animation.speed") entry["description"] = "Higher values make Helix Shell motion faster. This applies while animations are enabled.";
    if (entry.at("id") == "shell.panel.glass") entry["description"] = "Translucency of shell chrome. Zero is solid. When unset, the amount follows the selected material.";
    if (entry.at("id") == "shell.panel.refraction") entry["description"] = "Refracts whatever is under glass chrome (windows and workspace) when helix-niri supports it. Not a wallpaper-only xray.";
    if (entry.at("id") == "shell.panel.borders") entry["description"] = "Draws an outline around shell panels.";
    if (entry.at("id") == "shell.panel.shadow") entry["description"] = "Draws a shadow under shell panels.";
    if (entry.at("id") == "shell.panel.list_item_background") entry["description"] = "Fills list rows in the launcher, clipboard and similar panels.";
    if (entry.at("id") == "shell.panel.floating_offset") entry["description"] = "Distance, in pixels, between a floating panel and the screen edge or bar.";
    if (entry.at("id") == "shell.button_borders") entry["description"] = "Draws outlines on Helix Shell buttons.";
    if (entry.at("id") == "shell.input_borders") entry["description"] = "Draws outlines on Helix Shell text fields.";
    if (entry.at("id") == "shell.popup_borders") entry["description"] = "Draws outlines on Helix Shell menus.";
    if (entry.at("id") == "shell.card_borders") entry["description"] = "Draws outlines on Helix Shell cards.";
    if (entry.at("id") == "shell.shadow.direction") entry["description"] = "Direction of Helix Shell drop shadows.";
    if (entry.at("id") == "shell.shadow.alpha") entry["description"] = "Opacity of Helix Shell drop shadows.";
    if (entry.at("id") == "shell.popup_shadows") entry["description"] = "Draws drop shadows on Helix Shell menus.";
    if (entry.at("id") == "shell.app_icon_colorize") entry["description"] = "Desaturate and tint application icons in the shell. Turn this on to use the tint color below.";
    if (entry.at("id") == "shell.app_icon_color") entry["description"] = "Palette role or hex color used while icon tinting is enabled. Default follows the current theme.";
    if (entry.at("id") == "hooks.started") entry["description"] = "Runs after Helix Shell starts.";
    if (entry.at("id") == "hooks.session_locked") entry["description"] = "Runs when the session is locked.";
    if (entry.at("id") == "hooks.session_unlocked") entry["description"] = "Runs after the session is unlocked.";
    if (entry.at("id") == "hooks.logging_out") entry["description"] = "Runs before logging out.";
    if (entry.at("id") == "hooks.rebooting") entry["description"] = "Runs before rebooting.";
    if (entry.at("id") == "hooks.shutting_down") entry["description"] = "Runs before shutdown.";
    if (entry.at("id") == "hooks.wallpaper_changed") entry["description"] = "Runs when the wallpaper changes. Provides HELIX_WALLPAPER_PATH and HELIX_WALLPAPER_CONNECTOR.";
    if (entry.at("id") == "hooks.colors_changed") entry["description"] = "Runs when the active color palette changes.";
    if (entry.at("id") == "hooks.theme_mode_changed") entry["description"] = "Runs when the resolved theme mode changes between light and dark.";
    if (entry.at("id") == "hooks.wifi_enabled") entry["description"] = "Runs when Wi-Fi is enabled.";
    if (entry.at("id") == "hooks.wifi_disabled") entry["description"] = "Runs when Wi-Fi is disabled.";
    if (entry.at("id") == "hooks.bluetooth_enabled") entry["description"] = "Runs when Bluetooth is enabled.";
    if (entry.at("id") == "hooks.bluetooth_disabled") entry["description"] = "Runs when Bluetooth is disabled.";
    if (entry.at("id") == "hooks.battery_charging") entry["description"] = "Runs when the battery starts charging.";
    if (entry.at("id") == "hooks.battery_discharging") entry["description"] = "Runs when the battery starts discharging.";
    if (entry.at("id") == "hooks.battery_plugged") entry["description"] = "Runs when the battery becomes fully charged or pending charge.";
    if (entry.at("id") == "hooks.battery_percentage_changed") entry["description"] = "Runs when the battery percentage changes.";
    if (entry.at("id") == "hooks.power_profile_changed") entry["description"] = "Runs when the active power profile changes.";
    if (entry.at("id") == "shell.panel.launcher_placement" || entry.at("id") == "shell.panel.clipboard_placement" || entry.at("id") == "shell.panel.session_placement" || entry.at("id") == "shell.panel.wallpaper_placement" || entry.at("id") == "shell.panel.polkit_placement" || entry.at("id") == "shell.panel.quick_insert_placement") entry["description"] = "Attached follows the bar; Floating uses a screen position.";
    if (entry.at("id") == "shell.panel.launcher_position" || entry.at("id") == "shell.panel.clipboard_position" || entry.at("id") == "shell.panel.session_position" || entry.at("id") == "shell.panel.wallpaper_position" || entry.at("id") == "shell.panel.polkit_position" || entry.at("id") == "shell.panel.quick_insert_position") entry["description"] = "Used only when this panel is floating. Automatic follows the bar.";
    if (entry.at("id") == "shell.panel.open_near_click_launcher" || entry.at("id") == "shell.panel.open_near_click_clipboard" || entry.at("id") == "shell.panel.open_near_click_session" || entry.at("id") == "shell.panel.open_near_click_wallpaper" || entry.at("id") == "shell.panel.open_near_click_quick_insert") entry["description"] = "When attached or floating with Automatic position, opens near the click instead of a fixed screen anchor where supported.";
    if (entry.at("id") == "shell.panel_anchor_bar") entry["description"] = "Bar that attached panels follow. Automatic uses the bar under the pointer, or the first enabled bar.";
    if (entry.at("id") == "shell.panel.floating_layer") entry["description"] = "Layer used by floating panels. Above fullscreen can cover games and videos.";
    if (entry.at("id") == "shell.session.grid") entry["description"] = "Arranges session actions in a grid instead of a list.";
    if (entry.at("id") == "shell.session.show_shortcuts") entry["description"] = "Shows the keyboard shortcut for each session action when one is configured.";
    if (entry.at("id") == "shell.launcher.auto_paste") entry["description"] = "After calculator, emoji and similar copy actions, paste into the active app. Off only copies.";
    if (entry.at("id") == "shell.launcher.provider_prefix") entry["description"] = "Character that starts a provider command in the launcher search field, such as /calc.";
    if (entry.at("id") == "shell.launcher.fetch_exchange_rates") entry["description"] = "Lets the calculator refresh currency conversion rates online when network requests are allowed.";
    if (entry.at("id") == "shell.window_switcher.mru") entry["description"] = "Orders windows by most recent use instead of spatial layout.";
    if (entry.at("id") == "osd.background_opacity") entry["description"] = "Opacity of on-screen indicator backgrounds.";
    if (entry.at("id") == "osd.border") entry["description"] = "Draws an outline around on-screen indicator cards.";
    if (entry.at("id") == "osd.offset_x") entry["description"] = "Horizontal margin from the screen edge.";
    if (entry.at("id") == "osd.offset_y") entry["description"] = "Vertical margin from the screen edge.";
    if (entry.at("id") == "dock.monitors") entry["description"] = "Displays that show the dock. An empty list uses all displays.";
    if (entry.at("id") == "dock.layer") entry["description"] = "Layer used by the dock. Above fullscreen can cover games and videos.";
    if (entry.at("id") == "dock.shadow") entry["description"] = "Draws the global shell shadow under the dock.";
    if (entry.at("id") == "dock.background_opacity") entry["description"] = "Opacity of the dock background fill.";
    if (entry.at("id") == "dock.radius") entry["description"] = "Corner radius of the dock background.";
    if (entry.at("id") == "dock.active_scale") entry["description"] = "Scale of the focused application icon.";
    if (entry.at("id") == "dock.inactive_scale") entry["description"] = "Scale of unfocused application icons.";
    if (entry.at("id") == "dock.active_opacity") entry["description"] = "Opacity of the focused application icon.";
    if (entry.at("id") == "dock.inactive_opacity") entry["description"] = "Opacity of unfocused application icons.";
    if (entry.at("id") == "shell.session.grid_columns") entry["description"] = "Number of columns when the session panel uses a grid layout.";
    if (entry.at("id") == "desktop_widgets.enabled") entry["description"] = "Shows desktop widgets on the wallpaper. Existing widget layouts remain saved when this is off.";
    if (entry.at("id") == "lockscreen_widgets.enabled") entry["description"] = "Shows widgets on the lock screen. Existing layouts remain saved when this is off.";
    if (entry.at("id") == "dock.launcher_icon") entry["description"] = "Tabler icon name for the dock launcher button when no custom image is set.";
    if (entry.at("id") == "dock.launcher_custom_image") entry["description"] = "Absolute path to an image used instead of the launcher icon. Leave blank to use the icon name.";
    if (entry.at("id") == "dock.launcher_custom_image_colorize") entry["description"] = "Tints the custom launcher image with the shell icon color.";
    if (entry.at("id") == "shell.screen_corners.enabled") entry["description"] = "Draws rounded overlays at the corners of each display.";
    if (entry.at("id") == "shell.screen_corners.size") entry["description"] = "Radius of the rounded screen-corner overlays.";
    if (entry.at("id") == "theme.niri_layout.gaps") entry["description"] = "Spacing between tiled windows in helix-niri. Saving writes this into the Helix niri layout include.";
    if (entry.at("id") == "theme.niri_layout.struts") entry["description"] = "Outer padding around the tiling area in helix-niri.";
    if (entry.at("id") == "theme.niri_layout.default_column_width") entry["description"] = "Default column width as a fraction of the screen. 0.5 is half width.";
    if (entry.at("id") == "theme.niri_layout.overview") entry["description"] = "Overview wallpaper treatment written for helix-niri.";
    if (entry.at("id") == "theme.niri_layout.focus_ring") entry["description"] = "Shows a focus ring around the active window through the Helix niri layout include.";
    if (entry.at("id") == "theme.niri_layout.border") entry["description"] = "Shows window borders through the Helix niri layout include.";
    if (entry.at("id") == "theme.niri_layout.draw_border_with_background") entry["description"] = "Fills the window border area with the surface background when borders are enabled.";
    if (entry.at("id") == "theme.niri_layout.blur") entry["description"] = "Enables helix-niri window blur where the compositor supports it.";
    if (entry.at("id") == "theme.niri_layout.blur_passes") entry["description"] = "Quality of the blur effect. Higher values use more GPU work.";
    if (entry.at("id") == "theme.niri_layout.blur_offset") entry["description"] = "Sample distance for the blur effect.";
    if (entry.at("id") == "theme.niri_layout.blur_noise") entry["description"] = "Noise mixed into the blur to reduce banding.";
    if (entry.at("id") == "shell.niri_overview_type_to_launch_enabled") entry["description"] = "Focuses the launcher search field while the niri overview is open, where supported.";
    if (entry.at("id") == "theme.templates.enable_builtin_templates") entry["description"] = "Applies the built-in templates selected below. Existing generated files stay in place when this is off.";
    if (entry.at("id") == "theme.templates.builtin_ids") entry["description"] = "Choose which built-in templates write application theme files.";
    if (entry.at("id") == "theme.templates.enable_community_templates") entry["description"] = "Applies selected community templates from the local catalog.";
    if (entry.at("id") == "theme.templates.community_ids") entry["description"] = "Choose community templates that are already available on this device.";
    if (entry.at("id") == "shell.lang") entry["description"] = "Language for Helix Shell. Automatic follows the session locale. Applications keep their own language settings.";
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
      Json entry = {{"id", id}, {"page", "power"}, {"group", "Idle · " + behavior.name},
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
    struct BarField {
      const char* id; const char* title; const char* groupSuffix; const char* type;
      double minimum; double maximum; double step;
    };
    const BarField fields[] = {
      {"enabled", "Show bar", "", "boolean", 0, 0, 0},
      {"position", "Position", "", "position", 0, 0, 0},
      {"layer", "Layer", "", "shell-layer", 0, 0, 0},
      {"auto_hide", "Automatically hide", "", "boolean", 0, 0, 0},
      {"smart_auto_hide", "Hide behind windows", "", "boolean", 0, 0, 0},
      {"reserve_space", "Reserve space for the bar", "", "boolean", 0, 0, 0},
      {"show_on_workspace_switch", "Reveal when switching workspaces", "", "boolean", 0, 0, 0},
      {"shadow", "Show shadow", "", "boolean", 0, 0, 0},
      {"contact_shadow", "Contact shadow", "", "boolean", 0, 0, 0},
      {"thickness", "Thickness", " · Size", "integer", 10, 300, 1},
      {"scale", "Content scale", " · Size", "number", 0.5, 4, 0.05},
      {"font_scale", "Font scale", " · Size", "number", 0.2, 2.5, 0.01},
      {"padding", "End padding", " · Size", "integer", 0, 200, 1},
      {"widget_spacing", "Widget spacing", " · Size", "integer", 0, 100, 1},
      {"panel_overlap", "Panel overlap", " · Layout", "integer", -2, 3, 1},
      {"margin_ends", "Inset from display ends", " · Layout", "integer", 0, 500, 1},
      {"margin_edge", "Distance from display edge", " · Layout", "integer", 0, 100, 1},
      {"margin_opposite_edge", "Inward reserved space", " · Layout", "integer", 0, 100, 1},
      {"background_opacity", "Background opacity", " · Effects", "number", 0, 1, 0.01},
      {"radius", "Corner radius", " · Shape", "integer", 0, 500, 1},
      {"border_width", "Border width", " · Shape", "number", 0, 20, 0.5},
      {"concave_edge_corners", "Concave edge corners", " · Shape", "boolean", 0, 0, 0},
      {"font_weight", "Font weight", " · Typography", "integer", 100, 900, 100},
      {"font_family", "Font family", " · Typography", "string", 0, 0, 0},
    };
    for (const auto& field : fields) {
      auto id = "bar:" + bar.name + ":" + field.id;
      Json value;
      if (std::string_view(field.id) == "enabled") value = bar.enabled;
      else if (std::string_view(field.id) == "position") value = bar.position;
      else if (std::string_view(field.id) == "layer") value = bar.layer;
      else if (std::string_view(field.id) == "auto_hide") value = bar.autoHide;
      else if (std::string_view(field.id) == "smart_auto_hide") value = bar.smartAutoHide;
      else if (std::string_view(field.id) == "reserve_space") value = bar.reserveSpace;
      else if (std::string_view(field.id) == "show_on_workspace_switch") value = bar.showOnWorkspaceSwitch;
      else if (std::string_view(field.id) == "shadow") value = bar.shadow;
      else if (std::string_view(field.id) == "contact_shadow") value = bar.contactShadow;
      else if (std::string_view(field.id) == "thickness") value = bar.thickness;
      else if (std::string_view(field.id) == "scale") value = bar.scale;
      else if (std::string_view(field.id) == "font_scale") value = bar.fontScale;
      else if (std::string_view(field.id) == "font_weight") value = bar.fontWeight;
      else if (std::string_view(field.id) == "font_family") value = bar.fontFamily.value_or("");
      else if (std::string_view(field.id) == "padding") value = bar.padding;
      else if (std::string_view(field.id) == "widget_spacing") value = bar.widgetSpacing;
      else if (std::string_view(field.id) == "panel_overlap") value = bar.panelOverlap;
      else if (std::string_view(field.id) == "margin_ends") value = bar.marginEnds;
      else if (std::string_view(field.id) == "margin_edge") value = bar.marginEdge;
      else if (std::string_view(field.id) == "margin_opposite_edge") value = bar.marginOppositeEdge;
      else if (std::string_view(field.id) == "background_opacity") value = bar.backgroundOpacity;
      else if (std::string_view(field.id) == "border_width") value = bar.borderWidth;
      else if (std::string_view(field.id) == "concave_edge_corners") value = bar.concaveEdgeCorners;
      else value = bar.radius;
      Json options = Json::array(), labels = Json::array();
      if (std::string_view(field.type) == "position") {
        options = {"top", "bottom", "left", "right"};
        labels = {"Top", "Bottom", "Left", "Right"};
      } else if (std::string_view(field.type) == "shell-layer") {
        options = {"top", "overlay"};
        labels = {"Normal", "Above fullscreen windows"};
      }
      Json entry = {{"id", id}, {"page", "desktop-dock"}, {"title", field.title},
        {"group", "Bar · " + bar.name + field.groupSuffix}, {"type", field.type}, {"value", value},
        {"options", options}, {"option-labels", labels.empty() ? options : labels},
        {"minimum", field.minimum}, {"maximum", field.maximum},
        {"overridden", config.hasOverride(settingPath(id))}};
      if (field.step > 0 && std::string_view(field.type) == "number") entry["step"] = field.step;
      if (std::string_view(field.id) == "contact_shadow") entry["description"] = "Draws a dark gradient between an attached panel and this bar.";
      if (std::string_view(field.id) == "layer") entry["description"] = "Layer used by this bar. Attached panels follow it. Above fullscreen can cover games and videos.";
      if (std::string_view(field.id) == "scale") entry["description"] = "Scales glyphs and text on this bar independently of thickness.";
      if (std::string_view(field.id) == "font_scale") entry["description"] = "Scales bar fonts independently of content scale.";
      if (std::string_view(field.id) == "font_family") entry["description"] = "Typeface for this bar. Leave blank to use the shell font.";
      if (std::string_view(field.id) == "font_weight") entry["description"] = "Primary label weight for widgets on this bar.";
      if (std::string_view(field.id) == "border_width") entry["description"] = "Width of the inside outline around this bar.";
      if (std::string_view(field.id) == "concave_edge_corners") entry["description"] = "Carves concave corners on the side that touches the screen edge.";
      if (std::string_view(field.id) == "panel_overlap") entry["description"] = "How far attached panels overlap this bar edge, in pixels. Negative values pull the panel away.";
      if (std::string_view(field.id) == "margin_opposite_edge") entry["description"] = "Extra reserved space on the inward side of the bar.";
      if (std::string_view(field.type) == "string") {
        entry["options"] = Json::array();
        entry.erase("minimum");
        entry.erase("maximum");
      }
      result.push_back(std::move(entry));
    }
  }
  return result;
}
}

namespace helix {
void validateSettingValue(ConfigService& config, std::string_view id, const Json& value, bool desktop, bool gammaAvailable) {
  for (const auto& entry : settingsCatalog(config, desktop, gammaAvailable)) {
    if (entry.at("id") != id) continue;
    const auto type = entry.at("type").get<std::string>();
    if (type == "key-chords") {
      if (!value.is_array() || value.empty() || value.size() > 16) throw std::invalid_argument("Provide between one and sixteen key shortcuts");
      std::vector<KeyChord> seen;
      for (const auto& item : value) {
        if (!item.is_string() || item.get_ref<const std::string&>().size() > 128) throw std::invalid_argument("Key shortcuts must be short text values");
        const auto chord = parseKeyChordSpec(item.get_ref<const std::string&>());
        if (!chord) throw std::invalid_argument("Invalid key shortcut");
        if (std::ranges::find(seen, *chord) != seen.end()) throw std::invalid_argument("Duplicate key shortcut");
        seen.push_back(*chord);
      }
    } else if (type == "shortcut-list") {
      const auto shortcuts = shortcutValues(value);
      for (const auto& shortcut : shortcuts) if (std::ranges::find(entry.at("options"), Json(shortcut.type)) == entry.at("options").end())
        throw std::invalid_argument("Unknown or disabled shortcut");
    } else if (type == "notification-rules") { (void)notificationRules(value);
    } else if (type == "application-list" || type == "widget-list" || type == "display-list" || type == "tab-list" || type == "template-list" || type == "transition-list") {
      if (!value.is_array() || value.size() > 256) throw std::invalid_argument("Expected at most 256 application IDs");
      if (type == "transition-list" && value.empty()) throw std::invalid_argument("Choose at least one wallpaper transition");
      std::set<std::string> seen;
      for (const auto& item : value) {
        if (!item.is_string()) throw std::invalid_argument("Application IDs must be text");
        const auto& text = item.get_ref<const std::string&>();
        if (type == "tab-list" && std::ranges::find(entry.at("options"), item) == entry.at("options").end())
          throw std::invalid_argument("Unknown Control Center page");
        if (text.empty() || text.size() > 255 || (type != "display-list" && text.find('/') != std::string::npos) ||
            std::ranges::any_of(text, [](unsigned char c) { return c < 32 || c == 127; }) || !seen.insert(text).second)
          throw std::invalid_argument("Invalid or duplicate list entry");
        if ((type == "widget-list" || type == "template-list" || type == "transition-list") && std::ranges::find(entry.at("options"), item) == entry.at("options").end() &&
            std::ranges::find(entry.at("value"), item) == entry.at("value").end())
          throw std::invalid_argument(type == "template-list" ? "Unknown template" : type == "transition-list" ? "Unknown wallpaper transition" : "Unknown widget instance");
      }
    } else if (type == "boolean") {
      if (!value.is_boolean()) throw std::invalid_argument("This setting requires a boolean");
    } else if (type == "number" || type == "integer") {
      if (!value.is_number() || (type == "integer" && !value.is_number_integer())) throw std::invalid_argument("This setting requires a number");
      const double number = value.get<double>();
      if (!std::isfinite(number) || number < entry.at("minimum").get<double>() || number > entry.at("maximum").get<double>()) throw std::invalid_argument("Setting is outside its supported range");
    } else {
      if (!value.is_string()) throw std::invalid_argument("This setting requires text");
      const auto& text = value.get_ref<const std::string&>();
      if (type == "color") {
        if (text.size() > 64) throw std::invalid_argument("Color values must be short");
        if (!text.empty()) {
          try { (void)colorSpecFromConfigString(text); }
          catch (const std::exception&) { throw std::invalid_argument("Choose a palette role or hex color"); }
        }
      } else if (id.starts_with("hooks.")) {
        if (text.size() > 4096) throw std::invalid_argument("Commands must be at most 4096 characters");
        if (std::ranges::any_of(text, [](unsigned char c) { return c == 0; })) throw std::invalid_argument("Commands cannot contain null bytes");
      } else if (id == "shell.launcher.provider_prefix") {
        if (text.size() > 16) throw std::invalid_argument("Provider prefixes must be at most 16 characters");
        if (std::ranges::any_of(text, [](unsigned char c) { return c < 32 || c == 127; })) throw std::invalid_argument("Provider prefixes cannot contain control characters");
      } else if (id == "dock.launcher_icon") {
        if (text.empty() || text.size() > 64) throw std::invalid_argument("Choose a short icon name");
        if (std::ranges::any_of(text, [](unsigned char c) { return c < 32 || c == 127 || c == '/' || c == '\\'; })) throw std::invalid_argument("Icon names cannot contain path separators");
      } else if (id == "location.sunrise" || id == "location.sunset") {
        if (text.size() > 5 || (text.size() != 5 && !text.empty())) throw std::invalid_argument("Use HH:MM time");
        if (!text.empty() && (text[2] != ':' || !std::isdigit(static_cast<unsigned char>(text[0])) || !std::isdigit(static_cast<unsigned char>(text[1])) || !std::isdigit(static_cast<unsigned char>(text[3])) || !std::isdigit(static_cast<unsigned char>(text[4]))))
          throw std::invalid_argument("Use HH:MM time");
      } else if (id == "dock.launcher_custom_image" || id == "lockscreen.wallpaper") {
        if (text.size() > 4096) throw std::invalid_argument(id == "lockscreen.wallpaper" ? "Wallpaper paths must be at most 4096 characters" : "Image paths must be at most 4096 characters");
        if (!text.empty()) {
          std::error_code error;
          if (!std::filesystem::path(text).is_absolute() || !std::filesystem::is_regular_file(text, error))
            throw std::invalid_argument(id == "lockscreen.wallpaper" ? "Choose an existing wallpaper image" : "Choose an existing image file");
        }
      } else {
        if (type == "directory" && !text.empty()) {
          std::error_code error;
          if (!std::filesystem::path(text).is_absolute() || !std::filesystem::is_directory(text, error))
            throw std::invalid_argument("Choose an existing folder");
        }
        if (!entry.at("options").empty() && std::ranges::find(entry.at("options"), value) == entry.at("options").end()) throw std::invalid_argument("Unknown setting option");
      }
    }
    return;
  }
  throw std::invalid_argument("Setting is unavailable on this device");
}
}
