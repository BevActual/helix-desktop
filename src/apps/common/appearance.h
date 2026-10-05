#pragma once
#include <adwaita.h>
#include <nlohmann/json.hpp>
#include <string>

namespace helix::apps {
// Export semantic toolkit roles only. Geometry and interaction stay native.
inline void applyAppearance(GtkCssProvider* css, const nlohmann::json& appearance = {}) {
  auto* manager = adw_style_manager_get_default();
  if (!appearance.is_object() || !appearance.value("available", false)) {
    gtk_settings_reset_property(gtk_settings_get_default(), "gtk-enable-animations");
    adw_style_manager_set_color_scheme(manager, ADW_COLOR_SCHEME_DEFAULT);
    gtk_css_provider_load_from_string(css, ""); return;
  }
  if (appearance.value("helix-desktop", false))
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", appearance.value("animations", true), nullptr);
  else gtk_settings_reset_property(gtk_settings_get_default(), "gtk-enable-animations");
  const bool light = appearance.value("mode", "dark") == "light";
  adw_style_manager_set_color_scheme(manager, light ? ADW_COLOR_SCHEME_FORCE_LIGHT : ADW_COLOR_SCHEME_FORCE_DARK);
  if (adw_style_manager_get_high_contrast(manager) || appearance.value("high-contrast", false)) {
    gtk_css_provider_load_from_string(css, ""); return;
  }
  const auto tokens = appearance.value(light ? "light" : "dark", nlohmann::json::object());
  std::string style = ":root {";
  for (const auto& [token, variable] : {
    std::pair{"primary", "accent-bg-color"}, {"on_primary", "accent-fg-color"},
    {"surface", "window-bg-color"}, {"on_surface", "window-fg-color"},
    {"surface_variant", "card-bg-color"}, {"on_surface", "card-fg-color"}}) {
    if (!tokens.contains(token) || !tokens.at(token).is_number_unsigned()) continue;
    auto* color = g_strdup_printf("#%06x", tokens.at(token).get<unsigned>() & 0xffffff);
    style += "--" + std::string(variable) + ": " + color + ";"; g_free(color);
  }
  gtk_css_provider_load_from_string(css, (style + "}").c_str());
}
} // namespace helix::apps
