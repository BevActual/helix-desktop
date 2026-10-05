#pragma once
#include <adwaita.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace helix::settings {
class PaletteBrowser : public std::enable_shared_from_this<PaletteBrowser> {
public:
  using Json = nlohmann::json;
  using Apply = std::function<void(std::string, std::string, std::function<void(bool)>)>;
private:
  AdwDialog* dialog = nullptr;
  GtkWindow* parent;
  GtkDropDown *sources = nullptr, *scheme = nullptr;
  GtkSearchEntry* search = nullptr;
  GtkFlowBox* grid = nullptr;
  GtkLabel *status = nullptr, *choice = nullptr;
  GtkWidget* apply = nullptr;
  Json palettes = Json::array();
  std::vector<std::string> available;
  std::map<std::string, GtkToggleButton*> tiles;
  std::string selected, source, currentSource, currentName;
  bool closed = false, busy = false, connected = true;
  Apply onApply;
  using Callback = std::function<void()>;
  static void signal(gpointer widget, const char* name, Callback callback, bool notify = false) {
    auto* action = new Callback(std::move(callback));
    auto destroy = +[](gpointer p, GClosure*) { delete static_cast<Callback*>(p); };
    if (notify) g_signal_connect_data(widget, name, G_CALLBACK(+[](GObject*, GParamSpec*, gpointer p) { (*static_cast<Callback*>(p))(); }), action, destroy, G_CONNECT_DEFAULT);
    else g_signal_connect_data(widget, name, G_CALLBACK(+[](gpointer, gpointer p) { (*static_cast<Callback*>(p))(); }), action, destroy, G_CONNECT_DEFAULT);
  }
  static GtkWidget* label(const char* text) { auto* widget = gtk_label_new(text); gtk_label_set_wrap(GTK_LABEL(widget), true); gtk_label_set_xalign(GTK_LABEL(widget), 0); return widget; }
  void feedback(const char* text) { gtk_label_set_text(status, text); }
  void state() {
    gtk_widget_set_sensitive(apply, connected && !busy && !selected.empty() && !(source == currentSource && selected == currentName));
    gtk_widget_set_sensitive(GTK_WIDGET(sources), !busy); gtk_widget_set_sensitive(GTK_WIDGET(search), !busy);
    gtk_label_set_text(choice, selected.empty() ? "Choose a palette to compare its colors." :
      (source == currentSource && selected == currentName ? selected + " is active." : "Selected: " + selected).c_str());
  }
  void populate() {
    source = available.at(gtk_drop_down_get_selected(sources));
    selected.clear(); tiles.clear();
    while (auto* child = gtk_widget_get_first_child(GTK_WIDGET(grid))) gtk_flow_box_remove(grid, child);
    const auto* input = gtk_editable_get_text(GTK_EDITABLE(search));
    auto* needle = g_utf8_casefold(input, -1);
    unsigned visible = 0;
    for (const auto& item : palettes) {
      if (item.value("source", "") != source) continue;
      const auto name = item.value("name", "");
      auto* folded = g_utf8_casefold(name.c_str(), -1);
      const bool matches = std::string(folded).find(needle) != std::string::npos; g_free(folded);
      if (!matches) continue;
      auto* tile = gtk_toggle_button_new(); auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
      auto* swatch = gtk_drawing_area_new(); gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(swatch), 150); gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(swatch), 72);
      auto* preview = new Json(item);
      gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(swatch), +[](GtkDrawingArea* widget, cairo_t* cr, int width, int height, gpointer data) {
        const auto& palette = *static_cast<Json*>(data);
        const auto& mode = palette.at(gtk_drop_down_get_selected(GTK_DROP_DOWN(g_object_get_data(G_OBJECT(widget), "scheme"))) ? "light" : "dark");
        if (!mode.value("accents", Json::array()).size()) {
          GdkRGBA ink{}; gtk_widget_get_color(GTK_WIDGET(widget), &ink); cairo_set_source_rgba(cr, ink.red, ink.green, ink.blue, .15);
          cairo_rectangle(cr, 0, 0, width, height); cairo_fill(cr); return;
        }
        auto paint = [&](unsigned color) { cairo_set_source_rgb(cr, double((color >> 16) & 255) / 255, double((color >> 8) & 255) / 255, double(color & 255) / 255); };
        paint(mode.value("surface", 0u)); cairo_rectangle(cr, 0, 0, width, height); cairo_fill(cr);
        const auto& accents = mode.at("accents");
        paint(accents.at(0).get<unsigned>()); cairo_rectangle(cr, 8, 8, width - 16, 21); cairo_fill(cr);
        for (std::size_t i = 0; i < std::min<std::size_t>(3, accents.size()); ++i) {
          paint(accents.at(i).get<unsigned>()); cairo_arc(cr, 17 + i * 25, 53, 9, 0, G_PI * 2); cairo_fill(cr);
        }
      }, preview, +[](gpointer p) { delete static_cast<Json*>(p); });
      g_object_set_data(G_OBJECT(swatch), "scheme", scheme);
      gtk_box_append(GTK_BOX(content), swatch);
      auto* title = gtk_label_new(name.c_str()); gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars(GTK_LABEL(title), 18); gtk_box_append(GTK_BOX(content), title);
      gtk_button_set_child(GTK_BUTTON(tile), content); gtk_widget_set_tooltip_text(tile, name.c_str());
      gtk_accessible_update_property(GTK_ACCESSIBLE(tile), GTK_ACCESSIBLE_PROPERTY_LABEL, name.c_str(), -1);
      if (!tiles.empty()) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(tile), tiles.begin()->second);
      tiles[name] = GTK_TOGGLE_BUTTON(tile);
      auto self = shared_from_this(); signal(tile, "toggled", [self, tile, name] {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(tile))) { self->selected = name; self->state(); }
      });
      gtk_flow_box_append(grid, tile); ++visible;
      if (source == currentSource && name == currentName) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tile), true);
    }
    g_free(needle);
    if (!visible) feedback(input && *input ? "No palette matches this search." : "No palettes are available from this source. Refresh the catalog in Appearance.");
    else feedback("Select a palette, then apply it. Sample colors show the chosen light or dark variant.");
    state();
  }
  void applySelection() {
    if (busy || selected.empty()) return;
    busy = true; feedback("Applying palette…"); state();
    auto self = shared_from_this(); onApply(source, selected, [self](bool ok) {
      if (self->closed) return;
      self->busy = false;
      if (ok) adw_dialog_close(self->dialog);
      else { self->feedback("Palette could not be applied. Refresh the catalog or reconnect, then retry."); self->state(); }
    });
  }
  void build(const Json& initial, const std::string& preferredSource) {
    palettes = initial.value("palette-previews", Json::array());
    for (const auto& item : palettes) if (item.is_object() && item.contains("source") && item.at("source").is_string()) {
      const auto kind = item.at("source").get<std::string>();
      if ((kind == "builtin" || kind == "community" || kind == "custom") && std::ranges::find(available, kind) == available.end()) available.push_back(kind);
    }
    if (available.empty()) available.push_back("builtin");
    auto self = shared_from_this(); dialog = adw_dialog_new(); adw_dialog_set_title(dialog, "Palettes");
    adw_dialog_set_content_width(dialog, 720); adw_dialog_set_content_height(dialog, 650);
    g_object_set_data_full(G_OBJECT(dialog), "palette-browser", new std::shared_ptr<PaletteBrowser>(self), +[](gpointer p) { delete static_cast<std::shared_ptr<PaletteBrowser>*>(p); });
    signal(dialog, "closed", [self] { self->closed = true; });
    auto* toolbar = adw_toolbar_view_new(); auto* header = adw_header_bar_new(); adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(root, 12); gtk_widget_set_margin_end(root, 12); gtk_widget_set_margin_top(root, 12); gtk_widget_set_margin_bottom(root, 12);
    auto* selectors = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto* sourceNames = gtk_string_list_new(nullptr);
    for (const auto& kind : available) gtk_string_list_append(sourceNames, kind == "builtin" ? "Built-in" : kind == "community" ? "Community" : "Custom");
    sources = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(sourceNames), nullptr)); g_object_unref(sourceNames);
    gtk_widget_set_hexpand(GTK_WIDGET(sources), true); gtk_accessible_update_property(GTK_ACCESSIBLE(sources), GTK_ACCESSIBLE_PROPERTY_LABEL, "Palette source", -1);
    const char* schemeLabels[] = {"Dark", "Light", nullptr};
    auto* schemeNames = gtk_string_list_new(schemeLabels);
    scheme = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(schemeNames), nullptr)); g_object_unref(schemeNames);
    gtk_accessible_update_property(GTK_ACCESSIBLE(scheme), GTK_ACCESSIBLE_PROPERTY_LABEL, "Preview variant", -1);
    gtk_drop_down_set_selected(scheme, initial.value("mode", "dark") == "light" ? 1 : 0);
    gtk_box_append(GTK_BOX(selectors), GTK_WIDGET(sources)); gtk_box_append(GTK_BOX(selectors), GTK_WIDGET(scheme)); gtk_box_append(GTK_BOX(root), selectors);
    search = GTK_SEARCH_ENTRY(gtk_search_entry_new()); gtk_widget_set_hexpand(GTK_WIDGET(search), true);
    gtk_accessible_update_property(GTK_ACCESSIBLE(search), GTK_ACCESSIBLE_PROPERTY_LABEL, "Search palettes", -1);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(search)); status = GTK_LABEL(label("")); gtk_box_append(GTK_BOX(root), GTK_WIDGET(status));
    grid = GTK_FLOW_BOX(gtk_flow_box_new()); gtk_flow_box_set_selection_mode(grid, GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(grid, 1); gtk_flow_box_set_max_children_per_line(grid, 4);
    gtk_flow_box_set_homogeneous(grid, true); gtk_flow_box_set_row_spacing(grid, 8); gtk_flow_box_set_column_spacing(grid, 8);
    auto* scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(grid)); gtk_widget_set_vexpand(scroll, true); gtk_box_append(GTK_BOX(root), scroll);
    auto* footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_widget_set_margin_start(footer, 12); gtk_widget_set_margin_end(footer, 12);
    gtk_widget_set_margin_top(footer, 8); gtk_widget_set_margin_bottom(footer, 12);
    choice = GTK_LABEL(label("")); gtk_box_append(GTK_BOX(footer), GTK_WIDGET(choice));
    apply = gtk_button_new_with_label("Apply Palette"); gtk_widget_add_css_class(apply, "suggested-action"); gtk_widget_set_hexpand(apply, true);
    gtk_box_append(GTK_BOX(footer), apply); signal(apply, "clicked", [self] { self->applySelection(); });
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root); adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar), footer); adw_dialog_set_child(dialog, toolbar);
    signal(sources, "notify::selected", [self] { self->populate(); }, true);
    signal(scheme, "notify::selected", [self] { for (auto& [name, button] : self->tiles) {
      auto* content = gtk_button_get_child(GTK_BUTTON(button)); auto* drawing = gtk_widget_get_first_child(content); gtk_widget_queue_draw(drawing);
    } }, true);
    signal(search, "search-changed", [self] { self->populate(); });
    auto requested = std::ranges::find(available, preferredSource);
    gtk_drop_down_set_selected(sources, requested == available.end() ? 0 : std::distance(available.begin(), requested));
    populate(); adw_dialog_present(dialog, GTK_WIDGET(parent));
  }
public:
  PaletteBrowser(GtkWindow* parent, std::string currentSource, std::string currentName, Apply apply)
    : parent(parent), currentSource(std::move(currentSource)), currentName(std::move(currentName)), onApply(std::move(apply)) {}
  static std::shared_ptr<PaletteBrowser> open(GtkWindow* parent, const Json& appearance, std::string currentSource, std::string currentName, Apply apply) {
    auto self = std::make_shared<PaletteBrowser>(parent, currentSource, currentName, std::move(apply)); self->build(appearance, currentSource); return self;
  }
  bool isClosed() const { return closed; }
  void present() { if (!closed) adw_dialog_present(dialog, GTK_WIDGET(parent)); }
  void update(const Json& appearance, bool connected) {
    if (closed) return;
    this->connected = connected;
    if (appearance.contains("palette-previews") && appearance.at("palette-previews") != palettes) {
      palettes = appearance.at("palette-previews"); populate();
    }
    state();
  }
  Json inspect() const { return {{"source", source}, {"count", tiles.size()}, {"selected", selected}, {"apply-enabled", bool(gtk_widget_get_sensitive(apply))},
    {"width", gtk_widget_get_width(GTK_WIDGET(dialog))}, {"height", gtk_widget_get_height(GTK_WIDGET(dialog))}}; }
};
} // namespace helix::settings
