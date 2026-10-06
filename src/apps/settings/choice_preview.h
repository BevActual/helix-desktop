#pragma once

#include <adwaita.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <string>

namespace helix::settings {
// Small choice illustrations. Depicted settings remain editable as native rows.
class ChoicePreview {
  using Json = nlohmann::json;
 public:
  enum class Kind { DockShape, WallpaperFill, VolumeOverdrive, BrightnessSync };
 private:
  GtkWidget* area = nullptr;
  GtkLabel* caption = nullptr;
  Kind kind = Kind::DockShape;
  Json values = Json::object();
  std::string summary;
  template <class T> T value(const std::string& id, T fallback) const {
    const auto found = values.find(id);
    return found == values.end() ? fallback : found->get<T>();
  }
  std::string text(const std::string& id, const char* fallback) const { return value(id, std::string(fallback)); }
  static void color(cairo_t* cr, const GdkRGBA& ink, double alpha) {
    cairo_set_source_rgba(cr, ink.red, ink.green, ink.blue, ink.alpha * alpha);
  }
  static void rounded(cairo_t* cr, double x, double y, double w, double h, double radius) {
    radius = std::min({radius, w / 2.0, h / 2.0});
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - radius, y + radius, radius, -G_PI_2, 0);
    cairo_arc(cr, x + w - radius, y + h - radius, radius, 0, G_PI_2);
    cairo_arc(cr, x + radius, y + h - radius, radius, G_PI_2, G_PI);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, G_PI * 1.5);
    cairo_close_path(cr);
  }
  void draw(cairo_t* cr, int width, int height) const {
    GdkRGBA ink{}, accent{};
    gtk_widget_get_color(gtk_widget_get_parent(area), &ink);
    gtk_widget_get_color(area, &accent);
    const bool contrast = adw_style_manager_get_high_contrast(adw_style_manager_get_default());
    if (kind == Kind::DockShape) {
      const double scale = std::min((width - 16.0) / 360, (height - 16.0) / 120);
      if (scale <= 0) return;
      cairo_translate(cr, (width - 360 * scale) / 2, (height - 120 * scale) / 2);
      cairo_scale(cr, scale, scale);
      const double radius = value("dock.radius", 16);
      const double border = value("dock.border_width", 0.0);
      const bool concave = value("dock.concave_edge_corners", true);
      rounded(cr, 40, 35, 280, 50, radius);
      color(cr, ink, .12); cairo_fill_preserve(cr);
      color(cr, accent, contrast ? 1 : .85); cairo_set_line_width(cr, std::max(1.0, border)); cairo_stroke(cr);
      if (concave) {
        cairo_arc(cr, 40, 35, 10, G_PI, G_PI * 1.5); color(cr, ink, .35); cairo_set_line_width(cr, 2); cairo_stroke(cr);
        cairo_arc(cr, 320, 35, 10, -G_PI_2, 0); cairo_stroke(cr);
      }
      for (int i = 0; i < 5; ++i) {
        rounded(cr, 70 + i * 44, 46, 24, 24, 5); color(cr, accent, i == 2 ? .95 : .45); cairo_fill(cr);
      }
      return;
    }
    if (kind == Kind::WallpaperFill) {
      const double scale = std::min((width - 16.0) / 360, (height - 16.0) / 200);
      if (scale <= 0) return;
      cairo_translate(cr, (width - 360 * scale) / 2, (height - 200 * scale) / 2);
      cairo_scale(cr, scale, scale);
      rounded(cr, 20, 20, 320, 160, 12); color(cr, ink, .08); cairo_fill_preserve(cr); color(cr, ink, .4); cairo_stroke(cr);
      const auto fit = text("wallpaper.fill_mode", "crop");
      double ix = 40, iy = 40, iw = 280, ih = 120;
      if (fit == "fit") { ix = 70; iy = 50; iw = 220; ih = 100; }
      else if (fit == "center") { ix = 110; iy = 60; iw = 140; ih = 80; }
      else if (fit == "stretch") { ix = 20; iy = 20; iw = 320; ih = 160; }
      rounded(cr, ix, iy, iw, ih, 8); color(cr, accent, .55); cairo_fill(cr);
      if (fit == "fit" || fit == "center") {
        color(cr, ink, .18);
        cairo_rectangle(cr, 20, 20, 320, iy - 20); cairo_fill(cr);
        cairo_rectangle(cr, 20, iy + ih, 320, 180 - (iy + ih)); cairo_fill(cr);
      }
      return;
    }
    if (kind == Kind::VolumeOverdrive) {
      const double scale = std::min((width - 16.0) / 360, (height - 16.0) / 100);
      if (scale <= 0) return;
      cairo_translate(cr, (width - 360 * scale) / 2, (height - 100 * scale) / 2);
      cairo_scale(cr, scale, scale);
      const bool overdrive = value("audio.enable_overdrive", false);
      const double volume = std::clamp(value("audio.sound_volume", 0.5), 0.0, 1.0);
      rounded(cr, 30, 40, 240, 20, 8); color(cr, ink, .12); cairo_fill(cr);
      rounded(cr, 30, 40, 240 * volume, 20, 8); color(cr, accent, .9); cairo_fill(cr);
      if (overdrive) {
        rounded(cr, 270, 40, 60, 20, 8); color(cr, accent, .35); cairo_fill_preserve(cr);
        color(cr, accent, .9); cairo_set_line_width(cr, 1.5); cairo_stroke(cr);
        cairo_move_to(cr, 270, 35); cairo_line_to(cr, 270, 65); color(cr, ink, .5); cairo_stroke(cr);
      } else {
        cairo_move_to(cr, 270, 35); cairo_line_to(cr, 270, 65); color(cr, ink, .35); cairo_stroke(cr);
      }
      return;
    }
    if (kind == Kind::BrightnessSync) {
      const double scale = std::min((width - 16.0) / 360, (height - 16.0) / 140);
      if (scale <= 0) return;
      cairo_translate(cr, (width - 360 * scale) / 2, (height - 140 * scale) / 2);
      cairo_scale(cr, scale, scale);
      const bool sync = value("brightness.sync_all_monitors", false);
      const double level = std::clamp(value("brightness.minimum_brightness", 0.0) + 0.55, 0.15, 1.0);
      for (int i = 0; i < 2; ++i) {
        const double x = 40 + i * 150;
        rounded(cr, x, 25, 120, 70, 8); color(cr, ink, .1); cairo_fill_preserve(cr); color(cr, ink, .4); cairo_stroke(cr);
        rounded(cr, x + 15, 85, 90, 10, 4); color(cr, ink, .12); cairo_fill(cr);
        const double fill = sync ? level : (i == 0 ? level : std::max(0.2, level - 0.25));
        rounded(cr, x + 15, 85, 90 * fill, 10, 4); color(cr, accent, .85); cairo_fill(cr);
      }
      if (sync) {
        cairo_move_to(cr, 160, 55); cairo_line_to(cr, 190, 55); color(cr, accent, .9); cairo_set_line_width(cr, 2); cairo_stroke(cr);
      }
    }
  }
  explicit ChoicePreview(GtkWidget* drawing, GtkLabel* label, Kind kindIn) : area(drawing), caption(label), kind(kindIn) {}
 public:
  static GtkWidget* create(Kind kind) {
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    auto* drawing = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(drawing), 360);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(drawing), kind == Kind::WallpaperFill ? 200 : kind == Kind::BrightnessSync ? 140 : kind == Kind::DockShape ? 120 : 100);
    gtk_widget_set_hexpand(drawing, true);
    auto* label = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(label), true);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_add_css_class(label, "dim-label");
    auto* self = new ChoicePreview(drawing, GTK_LABEL(label), kind);
    g_object_set_data_full(G_OBJECT(box), "choice-preview", self, +[](gpointer data) { delete static_cast<ChoicePreview*>(data); });
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing), +[](GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer data) {
      static_cast<ChoicePreview*>(data)->draw(cr, w, h);
    }, self, nullptr);
    gtk_box_append(GTK_BOX(box), drawing);
    gtk_box_append(GTK_BOX(box), label);
    return box;
  }
  static void update(GtkWidget* widget, const Json& catalog) {
    auto* self = static_cast<ChoicePreview*>(g_object_get_data(G_OBJECT(widget), "choice-preview"));
    if (!self) return;
    self->values = Json::object();
    for (const auto& setting : catalog) {
      const auto id = setting.value("id", "");
      if (id.empty() || !setting.contains("value")) continue;
      self->values[id] = setting.at("value");
    }
    if (self->kind == Kind::DockShape) {
      self->summary = self->value("dock.concave_edge_corners", true) ?
        "Corner radius, outline and concave edges follow the controls below." :
        "Corner radius and outline follow the controls below.";
    } else if (self->kind == Kind::WallpaperFill) {
      const auto fit = self->text("wallpaper.fill_mode", "crop");
      const char* label = fit == "fit" ? "Fit" : fit == "center" ? "Center" : fit == "stretch" ? "Stretch" : "Crop";
      self->summary = std::string(label) + " places the sample on the display.";
    } else if (self->kind == Kind::VolumeOverdrive) {
      self->summary = self->value("audio.enable_overdrive", false) ?
        "Overdrive extends volume past 100%." :
        "Volume tops out at 100%.";
    } else {
      self->summary = self->value("brightness.sync_all_monitors", false) ?
        "Brightness changes apply to every display." :
        "Each display keeps its own brightness.";
    }
    gtk_label_set_text(self->caption, self->summary.c_str());
    gtk_widget_queue_draw(self->area);
  }
};
}
