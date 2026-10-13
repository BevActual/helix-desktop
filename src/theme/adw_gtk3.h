#pragma once

#include "config/config_types.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace helix::theme {

  inline constexpr std::string_view kGtk3TemplateId = "gtk3";
  inline constexpr std::string_view kGtk4TemplateId = "gtk4";

  // Same search roots as assets/templates/gtk/apply.sh: ~/.themes, XDG data
  // themes directories, then /usr/share/themes and /usr/local/share/themes.
  [[nodiscard]] std::vector<std::filesystem::path> adwGtk3ThemeRoots();

  // True when adw-gtk3 or adw-gtk3-dark exists under the given theme roots.
  [[nodiscard]] bool adwGtk3ThemePresent(const std::vector<std::filesystem::path>& themeRoots);

  [[nodiscard]] bool adwGtk3ThemePresent();

  // Built-in GTK 3/4 templates are opted in and the built-in catalog is enabled.
  [[nodiscard]] bool gtkTemplatesEnabled(const ThemeConfig::TemplatesConfig& templates);

  // Appends gtk3 and gtk4 to `ids` when missing. Returns true if ids changed.
  bool appendGtkTemplateIds(std::vector<std::string>& ids);

} // namespace helix::theme
