#pragma once
#include <gtk/gtk.h>

namespace helix::apps {
inline bool savePreview(GtkWidget* window, const char* path) {
  const int width = gtk_widget_get_width(window), height = gtk_widget_get_height(window);
  if (!gtk_widget_get_realized(window) || width <= 0 || height <= 0) return false;
  auto* paintable = gtk_widget_paintable_new(window); auto* snapshot = gtk_snapshot_new();
  gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height); g_object_unref(paintable);
  auto* node = gtk_snapshot_free_to_node(snapshot); if (!node) return false;
  auto* renderer = gtk_native_get_renderer(GTK_NATIVE(window));
  if (!renderer) { gsk_render_node_unref(node); return false; }
  const graphene_rect_t viewport = GRAPHENE_RECT_INIT(0, 0, static_cast<float>(width), static_cast<float>(height));
  auto* texture = gsk_renderer_render_texture(renderer, node, &viewport); gsk_render_node_unref(node);
  if (!texture) return false;
  const bool saved = gdk_texture_save_to_png(texture, path); g_object_unref(texture); return saved;
}
} // namespace helix::apps
