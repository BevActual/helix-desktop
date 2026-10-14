// One-shot custom palette writer for Rust `helix-desktop` (ThemeService parity, no session D-Bus).
#include "config/config_service.h"
#include "core/color.h"
#include "core/deferred_call.h"
#include "net/http_client.h"
#include "theme/color_roles.h"
#include "theme/theme_service.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

  void drainDeferredCalls() {
    for (auto callback : DeferredCall::takePending()) {
      if (callback) {
        callback();
      }
    }
  }

  int usage() {
    std::cerr << "Usage: helix-theme-palette save-palette <current|wallpaper>\n"
                 "       helix-theme-palette set-role <role-index> <#rrggbb>\n"
                 "       helix-theme-palette set-accent <#rrggbb>\n";
    return 1;
  }

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return usage();
  }

  const std::string_view command(argv[1]);
  ConfigService config;
  HttpClient http;
  helix::theme::ThemeService theme(config, http);

  std::string paletteName;
  std::string error;
  bool ok = false;

  if (command == "save-palette") {
    if (argc != 3) {
      return usage();
    }
    const std::string_view source(argv[2]);
    if (source == "wallpaper") {
      ok = theme.saveWallpaperPaletteAsCustom(&paletteName, &error);
    } else if (source == "current") {
      ok = theme.saveCurrentPaletteAsCustom(&paletteName, &error);
    } else {
      std::cerr << "error: unknown palette source (expected current or wallpaper)\n";
      return 1;
    }
  } else if (command == "set-role") {
    if (argc != 4) {
      return usage();
    }
    const auto roleIndex = std::strtoul(argv[2], nullptr, 10);
    if (roleIndex >= kColorRoleTokens.size()) {
      std::cerr << "error: unknown color role\n";
      return 1;
    }
    ok = theme.saveRoleColorAsCustom(ColorRole(roleIndex), hex(argv[3]), &paletteName, &error);
  } else if (command == "set-accent") {
    if (argc != 3) {
      return usage();
    }
    ok = theme.saveRoleColorAsCustom(ColorRole::Primary, hex(argv[2]), &paletteName, &error);
  } else if (command == "--help" || command == "-h") {
    return usage();
  } else {
    std::cerr << "error: unknown command: " << command << '\n';
    return usage();
  }

  if (!ok) {
    std::cerr << (error.empty() ? "Palette could not be saved" : error) << '\n';
    return 2;
  }

  theme.apply();
  drainDeferredCalls();

  if (!paletteName.empty()) {
    std::cout << paletteName << '\n';
  }
  return 0;
}
