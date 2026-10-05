#pragma once
#include <adwaita.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <tuple>

namespace helix::settings {
// Illustrations are content, not replacement controls. Every depicted setting
// remains editable through the native preference rows below the illustration.
class LayoutPreview {
  using Json = nlohmann::json;
  GtkWidget* area;
  GtkLabel* caption;
  bool notifications;
  Json values = Json::object();
  std::string summary;
  template<class T> T value(const std::string& id, T fallback) const {
    const auto found = values.find(id); return found == values.end() ? fallback : found->get<T>();
  }
  std::string text(const std::string& id, const char* fallback) const { return value(id, std::string(fallback)); }
  static void color(cairo_t* cr, const GdkRGBA& color, double alpha) { cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha * alpha); }
  static void rounded(cairo_t* cr, double x, double y, double w, double h, double radius) {
    radius = std::min({radius, w / 2, h / 2});
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - radius, y + radius, radius, -G_PI_2, 0);
    cairo_arc(cr, x + w - radius, y + h - radius, radius, 0, G_PI_2);
    cairo_arc(cr, x + radius, y + h - radius, radius, G_PI_2, G_PI);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, G_PI * 1.5); cairo_close_path(cr);
  }
  void draw(cairo_t* cr, int width, int height) const {
    GdkRGBA ink{}, accent{}; gtk_widget_get_color(gtk_widget_get_parent(area), &ink); gtk_widget_get_color(area, &accent);
    const bool contrast = adw_style_manager_get_high_contrast(adw_style_manager_get_default()) || value("accessibility.high_contrast", false);
    // Fixed proportions describe a display, while the illustration itself scales
    // to fit its allocation at every window width and font size.
    const double scale = std::min((width - 16.0) / 520, (height - 16.0) / 300);
    if (scale <= 0) return;
    cairo_translate(cr, (width - 520 * scale) / 2, (height - 300 * scale) / 2); cairo_scale(cr, scale, scale);
    rounded(cr, 1, 1, 518, 298, 14); color(cr, ink, .04); cairo_fill_preserve(cr);
    color(cr, ink, contrast ? 1 : .45); cairo_set_line_width(cr, contrast ? 2 : 1.5); cairo_stroke(cr);
    // A neutral sample application gives the bars and popup positions context.
    rounded(cr, 102, 65, 310, 180, 10); color(cr, ink, .07); cairo_fill_preserve(cr); color(cr, ink, .35); cairo_stroke(cr);
    cairo_move_to(cr, 102, 93); cairo_line_to(cr, 412, 93); cairo_move_to(cr, 177, 93); cairo_line_to(cr, 177, 245); color(cr, ink, .2); cairo_stroke(cr);
    rounded(cr, 113, 108, 53, 12, 4); color(cr, accent, .6); cairo_fill(cr);
    for (int i = 0; i < 3; ++i) { rounded(cr, 113, 134 + i * 24, 44, 5, 2); color(cr, ink, .25); cairo_fill(cr); }
    for (int i = 0; i < 3; ++i) { rounded(cr, 193, 111 + i * 36, 198, 24, 5); color(cr, ink, .09); cairo_fill(cr); }
    if (notifications) {
      if (!value("notification.enable_daemon", false)) return;
      const auto position = text("notification.position", "top_right");
      const double x = position.ends_with("left") ? 14 : position.ends_with("center") ? 167 : 320;
      const bool bottom = position.starts_with("bottom");
      const auto maximum = value("notification.max_visible", 0);
      for (int i = (maximum == 0 ? 3 : std::min(maximum, 3)) - 1; i >= 0; --i) {
        const double y = bottom ? 218 - i * 77 : 14 + i * 77;
        rounded(cr, x, y, 186, 67, 8); color(cr, ink, .15); cairo_fill_preserve(cr); color(cr, ink, contrast ? 1 : .65); cairo_stroke(cr);
        rounded(cr, x + 10, y + 11, 21, 21, 5); color(cr, accent, 1); cairo_fill(cr);
        if (value("notification.show_app_name", true)) { rounded(cr, x + 40, y + 12, 84, 5, 2); color(cr, ink, .75); cairo_fill(cr); }
        rounded(cr, x + 40, y + 26, 128, 4, 2); color(cr, ink, .5); cairo_fill(cr);
        if (value("notification.show_actions", true)) { rounded(cr, x + 40, y + 44, 47, 12, 4); color(cr, accent, .65); cairo_fill(cr); }
      }
      return;
    }
    auto edge = [&](const std::string& position, bool dock, bool hiding, double thickness, unsigned items) {
      const bool vertical = position == "left" || position == "right";
      const double length = dock ? 164 : (vertical ? 268 : 488);
      const double x = vertical ? (position == "left" ? 12 : 508 - thickness) : (520 - length) / 2;
      const double y = vertical ? (300 - length) / 2 : (position == "top" ? 12 : 288 - thickness);
      rounded(cr, x, y, vertical ? thickness : length, vertical ? length : thickness, 6);
      color(cr, ink, .15); cairo_fill_preserve(cr); color(cr, ink, contrast ? 1 : .7);
      const double dash[] = {5, 4}; if (hiding) cairo_set_dash(cr, dash, 2, 0);
      cairo_stroke(cr); cairo_set_dash(cr, nullptr, 0, 0);
      const double size = std::min(thickness - 8, 16.0);
      for (unsigned i = 0; i < std::min(items, 8u); ++i) {
        const double offset = dock ? 10 + i * 24 : 10 + i * ((length - 35) / std::max(1u, items));
        rounded(cr, x + (vertical ? (thickness - size) / 2 : offset), y + (vertical ? offset : (thickness - size) / 2), size, size, 3);
        color(cr, dock ? accent : ink, dock ? .9 : .55); cairo_fill(cr);
      }
    };
    for (auto it = values.begin(); it != values.end(); ++it) if (it.key().starts_with("bar:") && it.key().ends_with(":enabled") && it.value() == true) {
      const auto prefix = it.key().substr(0, it.key().size() - 7);
      unsigned count = 0; for (const auto* section : {"start", "center", "end"}) if (values.contains(prefix + section)) count += values.at(prefix + section).size();
      edge(text(prefix + "position", "top"), false, value(prefix + "auto_hide", false) || value(prefix + "smart_auto_hide", false), std::clamp(value(prefix + "thickness", 30.0) / 2, 14.0, 30.0), count);
    }
    if (value("dock.enabled", false)) edge(text("dock.position", "bottom"), true, value("dock.auto_hide", false) || value("dock.smart_auto_hide", false), 30, 6);
    if (value("hot_corners.enabled", false)) {
      for (const auto& [name, x, y] : {std::tuple{"top_left", 8., 8.}, {"top_right", 512., 8.}, {"bottom_left", 8., 292.}, {"bottom_right", 512., 292.}})
        if (text("hot_corners." + std::string(name) + ".action", "none") != "none") { cairo_arc(cr, x, y, 5, 0, 2 * G_PI); color(cr, accent, 1); cairo_fill(cr); }
    }
  }
public:
  LayoutPreview(GtkWidget* drawing, GtkLabel* label, bool notifications) : area(drawing), caption(label), notifications(notifications) {}
  static GtkWidget* create(bool notifications) {
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); auto* drawing = gtk_drawing_area_new();
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(drawing), 240); gtk_widget_set_hexpand(drawing, true); gtk_widget_add_css_class(drawing, "accent");
    auto* label = gtk_label_new(""); gtk_label_set_wrap(GTK_LABEL(label), true); gtk_label_set_xalign(GTK_LABEL(label), 0);
    auto* self = new LayoutPreview(drawing, GTK_LABEL(label), notifications);
    g_object_set_data_full(G_OBJECT(box), "layout-preview", self, +[](gpointer data) { delete static_cast<LayoutPreview*>(data); });
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing), +[](GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer data) { static_cast<LayoutPreview*>(data)->draw(cr, w, h); }, self, nullptr);
    gtk_box_append(GTK_BOX(box), drawing); gtk_box_append(GTK_BOX(box), label); return box;
  }
  static void update(GtkWidget* widget, const Json& catalog) {
    auto& self = *static_cast<LayoutPreview*>(g_object_get_data(G_OBJECT(widget), "layout-preview"));
    Json values = Json::object(); for (const auto& setting : catalog) values[setting.at("id").get<std::string>()] = setting.at("value");
    if (self.values == values) return;
    self.values = std::move(values);
    if (self.notifications) {
      const auto maximum = self.value("notification.max_visible", 0);
      self.summary = self.value("notification.enable_daemon", false) ? "Popups: " + self.text("notification.position", "top_right") +
        (maximum == 0 ? ". Number limited by available space." : ". Up to " + std::to_string(maximum) + " visible.") : "Notification popups are disabled.";
      std::ranges::replace(self.summary, '_', ' ');
      self.summary += " Illustration shows up to three sample notifications.";
    } else {
      self.summary = self.value("dock.enabled", false) ? "Dock: " + self.text("dock.position", "bottom") + ". " : "Dock is hidden. ";
      for (auto it = self.values.begin(); it != self.values.end(); ++it) if (it.key().starts_with("bar:") && it.key().ends_with(":enabled") && it.value() == true) {
        const auto prefix = it.key().substr(0, it.key().size() - 7);
        self.summary += "Bar " + prefix.substr(4, prefix.size() - 5) + ": " + self.text(prefix + "position", "top") + ". ";
      }
      self.summary += "Dashed outlines indicate automatic hiding. Highlighted corners have actions.";
    }
    gtk_label_set_text(self.caption, self.summary.c_str()); gtk_accessible_update_property(GTK_ACCESSIBLE(self.area), GTK_ACCESSIBLE_PROPERTY_LABEL, self.summary.c_str(), -1);
    gtk_widget_queue_draw(self.area);
  }
  static std::string description(GtkWidget* widget) { return static_cast<LayoutPreview*>(g_object_get_data(G_OBJECT(widget), "layout-preview"))->summary; }
};
} // namespace helix::settings
