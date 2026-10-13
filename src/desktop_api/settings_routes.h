#pragma once

#include <string_view>

namespace settings {
  // Compatibility contexts keep working after retiring the shell's settings editors.
  inline std::string_view nativePageForSection(std::string_view section) {
    if (section.starts_with("settings:"))
      section.remove_prefix(9);
    section = section.substr(0, section.find_first_of(" :/."));
    if (section == "theme"
        || section == "wallpaper"
        || section == "appearance"
        || section == "glass"
        || section == "type"
        || section == "templates"
        || section == "icons"
        || section == "colors"
        || section == "effects"
        || section == "backdrop"
        || section == "video-wallpaper"
        || section == "roundness")
      return "appearance";
    if (section == "audio" || section == "sound" || section == "sounds")
      return "sound";
    if (section == "network"
        || section == "wifi"
        || section == "wifi-prefs"
        || section == "tailscale"
        || section == "vpn"
        || section == "dns")
      return "network";
    if (section == "bluetooth")
      return "bluetooth";
    if (section == "notifications" || section == "notification")
      return "notifications";
    if (section == "accessibility" || section == "scale")
      return "accessibility";
    if (section == "clipboard"
        || section == "privacy"
        || section == "security"
        || section == "privacy-security"
        || section == "firewall"
        || section == "storage"
        || section == "location"
        || section == "weather"
        || section == "lock-screen"
        || section == "lockscreen"
        || section == "authentication")
      return "privacy-security";
    if (section == "power" || section == "idle" || section == "session")
      return "power";
    if (section == "system"
        || section == "clock"
        || section == "calendar"
        || section == "language"
        || section == "setup"
        || section == "advanced")
      return "system";
    if (section == "keybinds" || section == "keyboard")
      return "keyboard";
    if (section == "monitor" || section == "displays")
      return "displays";
    if (section == "online-accounts" || section == "accounts")
      return "online-accounts";
    if (section == "about")
      return "about";
    if (section.empty() || section == "settings")
      return "appearance";
    return "desktop-dock";
  }

  // Finite section IDs are safe to export through desktop launch commands.
  inline std::string_view nativeGroupForSection(std::string_view id) {
    if (id == "clipboard")
      return "Clipboard";
    if (id == "control-center")
      return "Control Center";
    if (id == "dock")
      return "Dock";
    if (id == "bar")
      return "Bar";
    if (id == "launcher")
      return "Launcher";
    if (id == "osd")
      return "On-Screen Indicators";
    if (id == "hot-corners")
      return "Hot Corners";
    if (id == "hooks")
      return "Automation";
    if (id == "panels")
      return "Panels";
    if (id == "niri")
      return "Niri";
    if (id == "window-switcher")
      return "Window Switcher";
    if (id == "capture" || id == "screenshot" || id == "screen-record")
      return "Capture";
    if (id == "desktop" || id == "widgets")
      return "Desktop";
    if (id == "media")
      return "Media";
    if (id == "type" || id == "roundness")
      return "Typography";
    if (id == "glass")
      return "Shell chrome";
    if (id == "icons")
      return "Icons";
    if (id == "effects")
      return "Borders";
    if (id == "templates")
      return "Templates";
    if (id == "colors")
      return "Colors";
    if (id == "wallpaper" || id == "theme")
      return "Wallpaper";
    if (id == "video-wallpaper")
      return "Video Wallpaper";
    if (id == "backdrop")
      return "Backdrop";
    if (id == "scale")
      return "Presentation";
    if (id == "language")
      return "Language";
    if (id == "privacy" || id == "firewall" || id == "security")
      return "Network Privacy";
    if (id == "lock-screen" || id == "lockscreen")
      return "Lock Screen";
    if (id == "location" || id == "weather")
      return "Location";
    if (id == "authentication")
      return "Authentication";
    if (id == "storage")
      return "Storage";
    if (id == "wifi-prefs" || id == "wifi")
      return "Wi-Fi Preferences";
    if (id == "dns")
      return "DNS";
    if (id == "tailscale")
      return "Tailscale";
    if (id == "clock")
      return "Clock";
    if (id == "calendar")
      return "Calendar";
    if (id == "setup")
      return "Setup";
    if (id == "advanced")
      return "Settings";
    if (id == "sounds" || id == "audio")
      return "Sounds";
    return {};
  }
  inline std::string_view nativePageForTargetSection(std::string_view id) {
    if (id == "clipboard"
        || id == "privacy"
        || id == "lock-screen"
        || id == "lockscreen"
        || id == "location"
        || id == "weather"
        || id == "authentication"
        || id == "storage"
        || id == "firewall"
        || id == "security")
      return "privacy-security";
    if (id == "scale")
      return "accessibility";
    if (id == "language" || id == "clock" || id == "calendar" || id == "setup" || id == "advanced")
      return "system";
    if (id == "type"
        || id == "roundness"
        || id == "glass"
        || id == "icons"
        || id == "effects"
        || id == "templates"
        || id == "colors"
        || id == "wallpaper"
        || id == "theme"
        || id == "backdrop"
        || id == "video-wallpaper")
      return "appearance";
    if (id == "wifi-prefs" || id == "wifi" || id == "dns" || id == "tailscale")
      return "network";
    if (id == "sounds" || id == "audio")
      return "sound";
    if (id == "desktop" || id == "widgets" || id == "media")
      return "desktop-dock";
    return "desktop-dock";
  }
  inline std::string_view nativeSectionForContext(std::string_view context) {
    if (context.starts_with("settings:"))
      context.remove_prefix(9);
    context = context.substr(0, context.find_first_of(" :/."));
    return nativeGroupForSection(context).empty() ? std::string_view{} : context;
  }

  inline bool isSoftwareContext(std::string_view context) {
    if (context.starts_with("settings:"))
      context.remove_prefix(9);
    context = context.substr(0, context.find_first_of(" :/."));
    return context == "packages" || context == "software";
  }

  inline bool isRetiredSettingsTab(std::string_view id) {
    return isSoftwareContext(id)
        || id == "theme"
        || id == "wallpaper"
        || id == "control-center"
        || id == "launcher"
        || id == "desktop"
        || id == "shell"
        || id == "keybinds"
        || id == "templates"
        || id == "hooks"
        || id == "niri"
        || id == "phone"
        || id == "localsend"
        || id == "syncthing"
        || id == "cast";
  }
} // namespace settings
