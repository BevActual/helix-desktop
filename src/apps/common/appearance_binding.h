#pragma once

#include "apps/common/appearance.h"
#include "desktop_api/client.h"
#include <memory>

namespace helix::apps {
class AppearanceBinding {
  DesktopClient client;
  GtkCssProvider* css = gtk_css_provider_new();
  GdkDisplay* display;
  std::shared_ptr<int> lifetime = std::make_shared<int>(0);
  unsigned observer = 0;
  guint retry = 0;
  gulong contrast = 0;
  bool pending = false;
  Json current = Json::object();
  std::string fingerprint;
  void refresh() {
    if (pending) return;
    pending = true; std::weak_ptr<int> alive = lifetime;
    client.callAsync("appearance", "GetSnapshot", nullptr, [this, alive](Json value, std::string error) {
      if (alive.expired()) return;
      pending = false;
      Json appearance = error.empty() ? Json{{"available", value.value("available", false)},
        {"helix-desktop", value.value("helix-desktop", false)}, {"mode", value.value("mode", "dark")},
        {"animations", value.value("animations", true)}, {"high-contrast", value.value("high-contrast", false)},
        {"light", value.value("light", Json::object())}, {"dark", value.value("dark", Json::object())}} : Json::object();
      const auto next = appearance.dump();
      if (next != fingerprint) { fingerprint = next; current = std::move(appearance); applyAppearance(css, current); }
    });
  }
public:
  explicit AppearanceBinding(GtkWidget* window) : display(GDK_DISPLAY(g_object_ref(gtk_widget_get_display(window)))) {
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    observer = client.observe([this](std::string_view interface, std::string_view, const Json&) { if (interface == appearanceInterface) refresh(); });
    contrast = g_signal_connect(adw_style_manager_get_default(), "notify::high-contrast", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
      auto& self = *static_cast<AppearanceBinding*>(data); applyAppearance(self.css, self.current);
    }), this);
    retry = g_timeout_add_seconds(5, +[](gpointer data) -> gboolean { static_cast<AppearanceBinding*>(data)->refresh(); return G_SOURCE_CONTINUE; }, this);
    refresh();
  }
  ~AppearanceBinding() {
    lifetime.reset(); client.unobserve(observer);
    if (retry) g_source_remove(retry);
    if (contrast) g_signal_handler_disconnect(adw_style_manager_get_default(), contrast);
    gtk_style_context_remove_provider_for_display(display, GTK_STYLE_PROVIDER(css)); g_object_unref(css); g_object_unref(display);
  }
};
}
