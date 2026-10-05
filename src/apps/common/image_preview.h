#pragma once
#include <gtk/gtk.h>
#include <filesystem>
#include <cstdint>
#include <string>

namespace helix::apps {
// Decode a bounded thumbnail off the UI thread. The task retains its widget, so
// navigation or a service refresh can safely replace the containing page.
inline GtkWidget* imagePreview(const std::string& path, const char* description) {
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  auto* picture = gtk_picture_new(); gtk_picture_set_can_shrink(GTK_PICTURE(picture), true);
  gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
  gtk_picture_set_alternative_text(GTK_PICTURE(picture), description);
  gtk_widget_set_size_request(picture, -1, 180); gtk_widget_set_hexpand(picture, true);
  gtk_box_append(GTK_BOX(box), picture);
  auto* status = gtk_label_new("Loading preview…"); gtk_label_set_wrap(GTK_LABEL(status), true);
  gtk_widget_add_css_class(status, "dim-label"); gtk_box_append(GTK_BOX(box), status);
  g_object_set_data_full(G_OBJECT(picture), "preview-status", g_object_ref(status), g_object_unref);
  auto* task = g_task_new(picture, nullptr, +[](GObject* source, GAsyncResult* result, gpointer) {
    auto* pixbuf = static_cast<GdkPixbuf*>(g_task_propagate_pointer(G_TASK(result), nullptr));
    auto* label = GTK_LABEL(g_object_get_data(source, "preview-status"));
    if (pixbuf) {
      auto* pixels = gdk_pixbuf_read_pixel_bytes(pixbuf);
      auto* texture = gdk_memory_texture_new(gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf),
        gdk_pixbuf_get_has_alpha(pixbuf) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
        pixels, gdk_pixbuf_get_rowstride(pixbuf));
      g_bytes_unref(pixels);
      gtk_picture_set_paintable(GTK_PICTURE(source), GDK_PAINTABLE(texture));
      g_object_unref(texture); g_object_unref(pixbuf); gtk_widget_set_visible(GTK_WIDGET(label), false);
    } else {
      gtk_label_set_text(label, "Preview unavailable for this file."); gtk_widget_set_visible(GTK_WIDGET(source), false);
    }
  }, nullptr);
  g_task_set_task_data(task, new std::string(path), +[](gpointer data) { delete static_cast<std::string*>(data); });
  g_task_run_in_thread(task, +[](GTask* task, gpointer, gpointer data, GCancellable*) {
    const auto& path = *static_cast<std::string*>(data); std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || std::filesystem::file_size(path, error) > 64 * 1024 * 1024 || error) {
      g_task_return_pointer(task, nullptr, nullptr); return;
    }
    int width = 0, height = 0; gdk_pixbuf_get_file_info(path.c_str(), &width, &height);
    if (width <= 0 || height <= 0 || static_cast<std::uint64_t>(width) * height > 32 * 1024 * 1024) {
      g_task_return_pointer(task, nullptr, nullptr); return;
    }
    auto* pixbuf = gdk_pixbuf_new_from_file_at_scale(path.c_str(), 640, 360, true, nullptr);
    if (pixbuf) {
      auto* oriented = gdk_pixbuf_apply_embedded_orientation(pixbuf);
      if (oriented) { g_object_unref(pixbuf); pixbuf = oriented; }
    }
    g_task_return_pointer(task, pixbuf, g_object_unref);
  });
  g_object_unref(task); return box;
}
} // namespace helix::apps
