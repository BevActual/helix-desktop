#pragma once

#include <string_view>

namespace settings {
// Compatibility contexts keep working after retiring the shell's settings editors.
inline std::string_view nativePageForSection(std::string_view section) {
  if (section.starts_with("settings:")) section.remove_prefix(9);
  section = section.substr(0, section.find_first_of(" :/."));
  if (section == "theme" || section == "wallpaper" || section == "appearance" || section == "glass" || section == "type" || section == "templates" || section == "icons") return "appearance";
  if (section == "audio" || section == "sound") return "sound";
  if (section == "network" || section == "wifi" || section == "tailscale" || section == "vpn") return "network";
  if (section == "bluetooth") return "bluetooth";
  if (section == "notifications" || section == "notification") return "notifications";
  if (section == "accessibility" || section == "scale") return "accessibility";
  if (section == "clipboard" || section == "privacy" || section == "security" || section == "privacy-security" || section == "firewall" || section == "storage") return "privacy-security";
  if (section == "power" || section == "idle" || section == "session") return "power";
  if (section == "system") return "system";
  if (section == "keybinds" || section == "keyboard") return "keyboard";
  if (section == "monitor" || section == "displays") return "displays";
  if (section == "online-accounts" || section == "accounts") return "online-accounts";
  if (section == "about") return "about";
  if (section.empty() || section == "settings") return "appearance";
  return "desktop-dock";
}

// Finite section IDs are safe to export through desktop launch commands.
inline std::string_view nativeGroupForSection(std::string_view id) {
  if (id == "clipboard") return "Clipboard";
  if (id == "control-center") return "Control Center";
  if (id == "dock") return "Dock";
  if (id == "bar") return "Bar";
  if (id == "launcher") return "Launcher";
  if (id == "osd") return "On-Screen Indicators";
  if (id == "hot-corners") return "Hot Corners";
  return {};
}
inline std::string_view nativePageForTargetSection(std::string_view id) {
  return id == "clipboard" ? "privacy-security" : "desktop-dock";
}
inline std::string_view nativeSectionForContext(std::string_view context) {
  if (context.starts_with("settings:")) context.remove_prefix(9);
  context = context.substr(0, context.find_first_of(" :/."));
  return nativeGroupForSection(context).empty() ? std::string_view{} : context;
}

inline bool isSoftwareContext(std::string_view context) {
  if (context.starts_with("settings:")) context.remove_prefix(9);
  context = context.substr(0, context.find_first_of(" :/."));
  return context == "packages" || context == "software";
}

inline bool isRetiredSettingsTab(std::string_view id) {
  return isSoftwareContext(id) || id == "theme" || id == "wallpaper" || id == "control-center" || id == "launcher" ||
      id == "desktop" || id == "shell" || id == "keybinds" || id == "templates" || id == "plugins" ||
      id == "hooks" || id == "niri";
}
}
