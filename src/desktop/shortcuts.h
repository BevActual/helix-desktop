#pragma once
#include "desktop_api/protocol.h"
#include "config/config_types.h"
#include <algorithm>
#include <stdexcept>

namespace helix {
inline constexpr std::pair<std::string_view, std::string_view> builtInShortcuts[] = {
  {"wifi", "Wi-Fi"}, {"bluetooth", "Bluetooth"}, {"nightlight", "Night Light"},
  {"notification", "Do Not Disturb"}, {"dark_mode", "Appearance"}, {"caffeine", "Keep Awake"},
  {"audio", "Sound"}, {"mic_mute", "Microphone"}, {"power_profile", "Power Profile"},
  {"media", "Media"}, {"system", "System Monitor"}, {"keyboard_layout", "Keyboard Layout"},
  {"wallpaper", "Wallpaper"}, {"session", "Session"}, {"clipboard", "Clipboard"},
  {"screenshot", "Screenshot"}, {"pick_color", "Color Picker"}, {"ocr", "Extract Text"},
  {"scan_qr", "Scan QR Code"}, {"screen_recorder", "Screen Recording"}, {"screen_replay", "Screen Replay"},
  {"privacy", "Privacy"},
};
inline bool shortcutAvailable(std::string_view id, const Config& config) {
  if (id == "system") return config.system.monitor.enabled;
  if (id == "clipboard") return config.shell.clipboardEnabled;
  return true;
}
inline Json shortcutValue(const ShortcutConfig& shortcut) {
  return {{"type", shortcut.type}, {"size", enumToKey(kShortcutSizes, shortcut.size)}};
}
inline std::vector<ShortcutConfig> shortcutValues(const Json& values) {
  if (!values.is_array() || values.size() > 256) throw std::invalid_argument("Expected at most 256 shortcuts");
  std::vector<ShortcutConfig> result;
  for (const auto& value : values) {
    if (!value.is_object() || value.size() != 2 || !value.contains("type") || !value.at("type").is_string() ||
        !value.contains("size") || !value.at("size").is_string()) throw std::invalid_argument("Shortcut entries require a type and size");
    auto type = value.at("type").get<std::string>();
    const auto size = enumFromKey(kShortcutSizes, value.at("size").get<std::string>());
    if (type.empty() || type.size() > 255 || std::ranges::any_of(type, [](unsigned char c) { return c < 32 || c == 127; }) || !size)
      throw std::invalid_argument("Invalid shortcut type or size");
    result.push_back({std::move(type), *size});
  }
  return result;
}
}
