#include "apps/common/appearance.h"
#include "apps/common/image_preview.h"
#include "apps/common/preview_export.h"
#include "apps/settings/wallpaper_browser.h"
#include "apps/settings/layout_preview.h"
#include "apps/settings/palette_browser.h"
#include "util/file_utils.h"
#include "desktop/dns_update.h"
#include "desktop/notification_rules.h"
#include "desktop/client.h"
#include "desktop/settings_routes.h"
#include <adwaita.h>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

namespace {
using helix::Json;
struct Page { const char* id; const char* title; const char* icon; bool desktop = false; };
constexpr Page pages[] = {
  {"appearance", "Appearance", "applications-graphics-symbolic"},
  {"desktop-dock", "Desktop & Dock", "preferences-desktop-display-symbolic", true},
  {"notifications", "Notifications", "preferences-system-notifications-symbolic", true},
  {"accessibility", "Accessibility", "preferences-desktop-accessibility-symbolic", true},
  {"sound", "Sound", "audio-volume-high-symbolic"},
  {"network", "Network", "network-wireless-symbolic"},
  {"bluetooth", "Bluetooth", "bluetooth-symbolic"},
  {"privacy-security", "Privacy & Security", "security-high-symbolic", true},
  {"system", "System", "preferences-system-symbolic", true},
  {"about", "About", "help-about-symbolic"},
};
using Callback = std::function<void()>;
void onSignal(gpointer object, const char* signal, Callback callback, bool notify = false) {
  auto* function = new Callback(std::move(callback));
  auto destroy = [](gpointer data, GClosure*) { delete static_cast<Callback*>(data); };
  if (notify) g_signal_connect_data(object, signal, G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
    (*static_cast<Callback*>(data))();
  }), function, destroy, G_CONNECT_DEFAULT);
  else g_signal_connect_data(object, signal, G_CALLBACK(+[](gpointer, gpointer data) {
    (*static_cast<Callback*>(data))();
  }), function, destroy, G_CONNECT_DEFAULT);
}
GtkWidget* button(const char* title, Callback callback) {
  auto* widget = gtk_button_new_with_label(title);
  gtk_widget_set_valign(widget, GTK_ALIGN_CENTER);
  onSignal(widget, "clicked", std::move(callback));
  return widget;
}
void title(AdwPreferencesRow* row, const std::string& value) {
  adw_preferences_row_set_use_markup(row, false);
  adw_preferences_row_set_title(row, value.c_str());
}
struct Settings {
  helix::DesktopClient client;
  AdwApplication* app;
  AdwApplicationWindow* window = nullptr;
  AdwNavigationSplitView* split = nullptr;
  AdwNavigationPage* contentPage = nullptr;
  AdwToastOverlay* toasts = nullptr;
  AdwBanner* banner = nullptr;
  GtkListBox* sidebar = nullptr;
  GtkSearchEntry* search = nullptr;
  GtkBox* body = nullptr;
  GtkWidget* scroller = nullptr;
  GtkCssProvider* css = nullptr;
  GDBusConnection* bus = nullptr;
  guint subscription = 0;
  guint ownerSubscription = 0;
  guint poll = 0;
  guint refreshIdle = 0;
  bool refreshing = false;
  bool connected = false;
  bool refreshFailed = false;
  bool syncing = false;
  bool ready = false;
  unsigned pending = 0;
  std::string requestedPage;
  std::string requestedSection;
  std::string section;
  std::map<std::string, GtkWidget*> sectionGroups;
  guint sectionTick = 0;
  std::string page = "appearance";
  std::string rendered;
  std::string query;
  unsigned paletteSwatches = 0;
  GtkPicture* wallpaperPreview = nullptr;
  GtkWidget* layoutPreview = nullptr;
  std::map<std::string, GtkToggleButton*> appearanceModes;
  std::weak_ptr<helix::settings::WallpaperBrowser> wallpaperBrowser;
  std::weak_ptr<helix::settings::PaletteBrowser> paletteBrowser;
  std::string requestedBrowser;
  bool requestedPalette = false;
  AdwDialog* details = nullptr;
  std::map<std::string, AdwActionRow*> detailRows;
  std::uint32_t meterSource = 0;
  GtkLevelBar* meterBar = nullptr;
  AdwActionRow* meterRow = nullptr;
  void stopMeter() {
    if (!meterSource) return;
    meterSource = 0; meterBar = nullptr; meterRow = nullptr;
    client.callAsync("audio", "StopInputMeter", nullptr, [](Json, std::string) {});
  }
  void startMeter(std::uint32_t source) {
    meterSource = source;
    client.callAsync("audio", "StartInputMeter", g_variant_new("(u)", source), [this, source](Json, std::string error) {
      if (!error.empty() && meterSource == source) { stopMeter(); toast("Microphone test could not start"); }
      scheduleRefresh();
    });
    rendered.clear(); render();
  }
  struct PendingOperation { std::string domain, action; bool cancellable; };
  std::map<std::uint64_t, PendingOperation> operations;
  std::set<std::uint64_t> earlyCompletions;
  Json catalog = Json::array();
  std::map<std::string, Json> snapshots;
  std::map<std::string, std::function<void(const Json&)>> setters;
  std::map<std::string, GtkListBoxRow*> pageRows;

  explicit Settings(AdwApplication* application) : app(application) {}
  ~Settings() {
    stopMeter();
    if (sectionTick && scroller) gtk_widget_remove_tick_callback(scroller, sectionTick);
    if (poll) g_source_remove(poll);
    if (refreshIdle) g_source_remove(refreshIdle);
    if (bus && ownerSubscription) g_dbus_connection_signal_unsubscribe(bus, ownerSubscription);
    if (bus && subscription) g_dbus_connection_signal_unsubscribe(bus, subscription);
    if (css && window) gtk_style_context_remove_provider_for_display(gtk_widget_get_display(GTK_WIDGET(window)), GTK_STYLE_PROVIDER(css));
    if (css) g_object_unref(css);
    if (bus) g_object_unref(bus);
    if (window) g_object_unref(window);
  }
  void toast(const std::string& text) { adw_toast_overlay_add_toast(toasts, adw_toast_new(text.c_str())); }
  void scheduleRefresh() {
    if (refreshIdle) return;
    refreshIdle = g_idle_add([](gpointer data) -> gboolean {
      auto& self = *static_cast<Settings*>(data); self.refreshIdle = 0; self.refresh(); return G_SOURCE_REMOVE;
    }, this);
  }
  void refresh() {
    if (refreshing || !window) return;
    refreshing = true; refreshFailed = false; pending = 6;
    client.callAsync("configuration", "GetSettings", nullptr, [this](Json value, std::string error) {
      if (error.empty()) catalog = std::move(value);
      finish(error);
    });
    for (const std::string domain : {"configuration", "appearance", "audio", "network", "bluetooth"}) {
      client.callAsync(domain, "GetSnapshot", nullptr, [this, domain](Json value, std::string error) {
        if (error.empty()) snapshots[domain] = std::move(value);
        finish(error);
      });
    }
  }
  void finish(const std::string& error) {
    if (!error.empty()) { stopMeter(); operations.clear(); earlyCompletions.clear(); refreshFailed = true; connected = false; adw_banner_set_title(banner, "Helix services are unavailable. You can retry the connection."); adw_banner_set_revealed(banner, true); }
    if (--pending) return;
    refreshing = false;
    // A complete refresh requires all five domains, including unavailable backend snapshots.
    connected = !refreshFailed && snapshots.contains("configuration") && snapshots["configuration"].value("available", false) &&
      client.available();
    adw_banner_set_revealed(banner, !connected);
    const bool desktop = connected && snapshots["configuration"].value("helix-desktop", false);
    for (const auto& item : pages) if (item.desktop) {
      // An outage does not establish that desktop capabilities have disappeared.
      // Keep cached navigation until a successful snapshot says otherwise.
      if (connected) {
        gtk_widget_set_visible(GTK_WIDGET(pageRows.at(item.id)), desktop);
        if (!desktop && page == item.id) select("appearance");
      }
    }
    if (!requestedPage.empty()) {
      const auto target = requestedPage; requestedPage.clear(); select(target);
    }
    if (!requestedSection.empty() && page == "desktop-dock") {
      section = std::move(requestedSection); requestedSection.clear(); navigateSection();
    }
    applyAppearance();
    if (auto browser = wallpaperBrowser.lock(); browser && !browser->isClosed()) browser->update(snapshots["appearance"], connected);
    if (auto browser = paletteBrowser.lock(); browser && !browser->isClosed()) browser->update(snapshots["appearance"], connected);
    if (!requestedBrowser.empty()) {
      const auto source = requestedBrowser; requestedBrowser.clear(); chooseWallpaper(source == "wallhaven");
    }
    if (requestedPalette) { requestedPalette = false; choosePalette(); }
    render();
    updateDetails();
  }
  void applyAppearance() {
    helix::apps::applyAppearance(css, connected && snapshots.contains("appearance") ? snapshots.at("appearance") : Json::object());
  }
  void appearancePreview() {
    if (!query.empty() || !snapshots.contains("appearance")) return;
    const auto& appearance = snapshots.at("appearance");
    if (!appearance.value("available", false)) return;
    auto* preview = group("Current Appearance");
    adw_preferences_group_set_description(preview, appearance.value("high-contrast", false) ?
      "High contrast is active. The selected palette is shown below; controls use native contrast." :
      "Preview the current palette and desktop background.");
    // Native toggle buttons contain illustrative samples of the actual palette.
    // Geometry and keyboard/selected behavior remain entirely toolkit-owned.
    auto* modes = gtk_flow_box_new(); gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(modes), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(modes), true); gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(modes), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(modes), 3); gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(modes), 8); gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(modes), 8);
    std::string selectedMode = "auto";
    for (const auto& setting : catalog) if (setting.value("id", "") == "theme.mode") selectedMode = setting.at("value").get<std::string>();
    GtkToggleButton* firstMode = nullptr;
    for (const auto& [id, name] : std::vector<std::pair<std::string, const char*>>{{"light", "Light"}, {"dark", "Dark"}, {"auto", "Automatic"}}) {
      auto* mode = gtk_toggle_button_new(); auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
      auto* sample = gtk_drawing_area_new(); gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(sample), 112); gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(sample), 72);
      struct Sample { Json light, dark; std::string mode; };
      auto* colors = new Sample{appearance.value("light", Json::object()), appearance.value("dark", Json::object()), id};
      gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(sample), +[](GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer data) {
        const auto& colors = *static_cast<Sample*>(data);
        auto paint = [&](const Json& palette, bool right) {
          cairo_save(cr); if (colors.mode == "auto") { cairo_rectangle(cr, right ? w / 2.0 : 0, 0, w / 2.0, h); cairo_clip(cr); }
          auto color = [&](const char* token) {
            const auto value = palette.value(token, 0u); cairo_set_source_rgb(cr, double((value >> 16) & 255) / 255, double((value >> 8) & 255) / 255, double(value & 255) / 255);
          };
          color("surface"); cairo_rectangle(cr, 0, 0, w, h); cairo_fill(cr);
          color("surface_variant"); cairo_rectangle(cr, 8, 8, w - 16, h - 16); cairo_fill(cr);
          color("primary"); cairo_rectangle(cr, 8, 8, 25, h - 16); cairo_fill(cr);
          color("on_surface"); for (int i = 0; i < 3; ++i) { cairo_rectangle(cr, 43, 19 + i * 14, std::max(w - 60, 10), 3); cairo_fill(cr); }
          cairo_restore(cr);
        };
        if (colors.mode != "dark") paint(colors.light, false);
        if (colors.mode != "light") paint(colors.dark, true);
      }, colors, +[](gpointer data) { delete static_cast<Sample*>(data); });
      gtk_box_append(GTK_BOX(content), sample); gtk_box_append(GTK_BOX(content), gtk_label_new(name)); gtk_button_set_child(GTK_BUTTON(mode), content);
      gtk_accessible_update_property(GTK_ACCESSIBLE(mode), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
      if (firstMode) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(mode), firstMode); else firstMode = GTK_TOGGLE_BUTTON(mode);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mode), selectedMode == id);
      appearanceModes[id] = GTK_TOGGLE_BUTTON(mode);
      onSignal(mode, "toggled", [this, mode, id] { if (!syncing && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(mode))) update("theme.mode", id); });
      gtk_flow_box_append(GTK_FLOW_BOX(modes), mode);
    }
    adw_preferences_group_add(preview, modes);
    const auto tokens = appearance.value(appearance.value("mode", "dark") == "light" ? "light" : "dark", Json::object());
    auto* colors = gtk_flow_box_new(); gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(colors), GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(colors), 1); gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(colors), 5);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(colors), true); gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(colors), 12);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(colors), 12);
    for (const auto& [token, label] : std::vector<std::pair<const char*, const char*>>{
      {"primary", "Accent"}, {"surface", "Background"}, {"surface_variant", "Card"}, {"on_surface", "Text"}, {"outline", "Outline"}}) {
      if (!tokens.contains(token) || !tokens.at(token).is_number_unsigned()) continue;
      const auto color = tokens.at(token).get<unsigned>() & 0xffffff;
      auto* item = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); auto* swatch = gtk_drawing_area_new();
      gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(swatch), 48); gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(swatch), 48);
      gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(swatch), +[](GtkDrawingArea* area, cairo_t* context, int width, int height, gpointer data) {
        const auto color = GPOINTER_TO_UINT(data);
        const double radius = std::min(width, height) / 2.0 - 2;
        cairo_arc(context, width / 2.0, height / 2.0, radius, 0, 6.283185307);
        cairo_set_source_rgb(context, double((color >> 16) & 255) / 255, double((color >> 8) & 255) / 255, double(color & 255) / 255);
        cairo_fill_preserve(context); GdkRGBA foreground; gtk_widget_get_color(GTK_WIDGET(area), &foreground);
        cairo_set_source_rgba(context, foreground.red, foreground.green, foreground.blue, .35); cairo_set_line_width(context, 1); cairo_stroke(context);
      }, GUINT_TO_POINTER(color), nullptr);
      auto* hex = g_strdup_printf("%s #%06x", label, color);
      gtk_accessible_update_property(GTK_ACCESSIBLE(swatch), GTK_ACCESSIBLE_PROPERTY_LABEL, hex, -1); g_free(hex);
      gtk_box_append(GTK_BOX(item), swatch); gtk_box_append(GTK_BOX(item), gtk_label_new(label));
      auto* child = gtk_flow_box_child_new(); gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), item);
      gtk_widget_set_focusable(child, false); gtk_flow_box_append(GTK_FLOW_BOX(colors), child);
      ++paletteSwatches;
    }
    adw_preferences_group_add(preview, colors);
    const auto wallpaper = appearance.value("wallpaper", "");
    if (appearance.value("helix-desktop", false) && !wallpaper.empty()) {
      auto* image = helix::apps::imagePreview(wallpaper, "Desktop wallpaper preview");
      wallpaperPreview = GTK_PICTURE(gtk_widget_get_first_child(image));
      adw_preferences_group_add(preview, image);
    }
    auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(actions), button("Palettes", [this] { choosePalette(); }));
    gtk_box_append(GTK_BOX(actions), button("Accent", [this] { chooseAccent(); }));
    if (appearance.value("helix-desktop", false)) gtk_box_append(GTK_BOX(actions), button("Wallpaper", [this] { chooseWallpaper(); }));
    adw_preferences_group_add(preview, actions);
  }
  void navigateSection() {
    if (sectionTick) gtk_widget_remove_tick_callback(scroller, sectionTick);
    sectionTick = gtk_widget_add_tick_callback(scroller, +[](GtkWidget*, GdkFrameClock*, gpointer data) -> gboolean {
      auto& self = *static_cast<Settings*>(data);
      const auto target = self.sectionGroups.find(std::string(settings::nativeGroupForSection(self.section)));
      if (target == self.sectionGroups.end()) { self.sectionTick = 0; return G_SOURCE_REMOVE; }
      graphene_rect_t bounds;
      if (!gtk_widget_compute_bounds(target->second, GTK_WIDGET(self.body), &bounds) || bounds.size.height <= 0) return G_SOURCE_CONTINUE;
      auto* adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self.scroller));
      gtk_adjustment_set_value(adjustment, std::max(0.0, double(bounds.origin.y) - 12));
      self.sectionTick = 0;
      return G_SOURCE_REMOVE;
    }, this, nullptr);
  }
  void select(const std::string& id) {
    auto it = pageRows.find(id);
    if (it == pageRows.end()) { toast("Unknown Settings page"); return; }
    if (!gtk_widget_get_visible(GTK_WIDGET(it->second))) { toast("This page requires a Helix desktop session"); return; }
    if (id != "sound") stopMeter();
    section.clear();
    if (sectionTick) { gtk_widget_remove_tick_callback(scroller, sectionTick); sectionTick = 0; }
    query.clear(); gtk_editable_set_text(GTK_EDITABLE(search), "");
    page = id;
    gtk_list_box_select_row(sidebar, it->second);
    for (const auto& item : pages) if (id == item.id) adw_navigation_page_set_title(contentPage, item.title);
    rendered.clear(); render();
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller)), 0);
    adw_navigation_split_view_set_show_content(split, true);
  }
  void update(const std::string& id, Json value) {
    if (syncing) return;
    client.callAsync("configuration", "Update", g_variant_new("(sv)", id.c_str(), helix::toVariant(value)),
      [this](Json, std::string error) { if (!error.empty()) toast("This setting could not be saved. Refresh and try again."); scheduleRefresh(); });
  }
  void act(const std::string& domain, const std::string& action, Json args = Json::object()) {
    client.callAsync(domain, "Action", g_variant_new("(s@a{sv})", action.c_str(), helix::toVariant(args)),
      [this, domain, action](Json result, std::string error) {
        if (!error.empty()) toast("The operation could not start. Check the device and try again.");
        else if ((result.is_number_unsigned() || result.is_number_integer()) && !earlyCompletions.erase(result.get<std::uint64_t>()))
          operations[result.get<std::uint64_t>()] = {domain, action, (domain == "bluetooth" && action == "pair") || (domain == "network" && (action == "connect" || action == "enterprise"))};
        scheduleRefresh();
      });
  }
  AdwPreferencesGroup* group(const std::string& name) {
    auto* widget = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(widget), name.c_str());
    gtk_box_append(body, widget);
    sectionGroups[name] = widget;
    return ADW_PREFERENCES_GROUP(widget);
  }
  AdwActionRow* row(AdwPreferencesGroup* group, const std::string& name, const std::string& subtitle = {}) {
    auto* widget = adw_action_row_new();
    title(ADW_PREFERENCES_ROW(widget), name);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(widget), subtitle.c_str());
    adw_action_row_set_title_lines(ADW_ACTION_ROW(widget), 2);
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(widget), 3);
    adw_preferences_group_add(group, widget);
    return ADW_ACTION_ROW(widget);
  }
  bool matches(const std::string& text) const {
    if (query.empty()) return true;
    auto* haystack = g_utf8_casefold(text.c_str(), -1);
    auto* needle = g_utf8_casefold(query.c_str(), -1);
    bool hit = std::string(haystack).find(needle) != std::string::npos;
    g_free(haystack); g_free(needle); return hit;
  }
  void addSetting(AdwPreferencesGroup* group, const Json& setting) {
    const auto id = setting.at("id").get<std::string>();
    const auto type = setting.at("type").get<std::string>();
    const auto name = setting.at("title").get<std::string>();
    GtkWidget* widget;
    if (type == "notification-rules") { ruleList(group, setting); return; }
    if (type == "shortcut-list") { shortcutList(group, setting); return; }
    if (type == "tab-list") { tabVisibility(group, setting); return; }
    if (type == "application-list" || type == "widget-list" || type == "display-list") { applicationList(group, setting); return; }
    if (type == "directory") {
      widget = adw_action_row_new(); title(ADW_PREFERENCES_ROW(widget), name);
      adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(widget), 2);
      setters[id] = [widget](const Json& value) {
        const auto path = value.get<std::string>();
        adw_action_row_set_subtitle(ADW_ACTION_ROW(widget), path.empty() ? "Use the configured default folder" : path.c_str());
      };
      setters[id](setting.at("value"));
      adw_action_row_add_suffix(ADW_ACTION_ROW(widget), button("Choose", [this, id] { chooseFolder(id); }));
    } else if (type == "boolean") {
      widget = adw_switch_row_new(); title(ADW_PREFERENCES_ROW(widget), name);
      setters[id] = [widget](const Json& value) { adw_switch_row_set_active(ADW_SWITCH_ROW(widget), value.get<bool>()); };
      setters[id](setting.at("value"));
      onSignal(widget, "notify::active", [this, widget, id] { update(id, bool(adw_switch_row_get_active(ADW_SWITCH_ROW(widget)))); }, true);
    } else if (!setting.at("options").empty()) {
      widget = adw_combo_row_new(); title(ADW_PREFERENCES_ROW(widget), name);
      auto options = setting.at("options").get<std::vector<std::string>>();
      auto* model = gtk_string_list_new(nullptr);
      const auto labels = setting.value("option-labels", options);
      for (const auto& label : labels) gtk_string_list_append(model, label.c_str());
      adw_combo_row_set_model(ADW_COMBO_ROW(widget), G_LIST_MODEL(model)); g_object_unref(model);
      setters[id] = [widget, options](const Json& value) {
        auto it = std::ranges::find(options, value.get<std::string>());
        adw_combo_row_set_selected(ADW_COMBO_ROW(widget), it == options.end() ? GTK_INVALID_LIST_POSITION : guint(it - options.begin()));
      };
      setters[id](setting.at("value"));
      onSignal(widget, "notify::selected", [this, widget, options, id] {
        auto selected = adw_combo_row_get_selected(ADW_COMBO_ROW(widget));
        if (selected < options.size()) update(id, options[selected]);
      }, true);
    } else if (type == "number" || type == "integer") {
      widget = adw_spin_row_new_with_range(setting.at("minimum"), setting.at("maximum"), type == "integer" ? 1 : 0.05);
      title(ADW_PREFERENCES_ROW(widget), name);
      adw_spin_row_set_digits(ADW_SPIN_ROW(widget), type == "integer" ? 0 : 2);
      setters[id] = [widget](const Json& value) { adw_spin_row_set_value(ADW_SPIN_ROW(widget), value.get<double>()); };
      setters[id](setting.at("value"));
      onSignal(widget, "notify::value", [this, widget, id, type] {
        double value = adw_spin_row_get_value(ADW_SPIN_ROW(widget));
        update(id, type == "integer" ? Json(std::int64_t(value)) : Json(value));
      }, true);
    } else {
      widget = adw_entry_row_new(); title(ADW_PREFERENCES_ROW(widget), name);
      adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(widget), true);
      setters[id] = [widget](const Json& value) {
        if (!gtk_widget_has_focus(widget)) gtk_editable_set_text(GTK_EDITABLE(widget), value.get_ref<const std::string&>().c_str());
      };
      setters[id](setting.at("value"));
      onSignal(widget, "apply", [this, widget, id] { update(id, std::string(gtk_editable_get_text(GTK_EDITABLE(widget)))); });
    }
    if (setting.value("overridden", false)) {
      auto* reset = gtk_button_new_from_icon_name("edit-undo-symbolic");
      gtk_widget_set_tooltip_text(reset, "Restore configured default");
      gtk_widget_set_valign(reset, GTK_ALIGN_CENTER);
      onSignal(reset, "clicked", [this, id] {
        client.callAsync("configuration", "Reset", g_variant_new("(s)", id.c_str()), [this](Json, std::string error) {
          if (!error.empty()) toast("The default could not be restored");
          scheduleRefresh();
        });
      });
      if (ADW_IS_ENTRY_ROW(widget)) adw_entry_row_add_suffix(ADW_ENTRY_ROW(widget), reset);
      else adw_action_row_add_suffix(ADW_ACTION_ROW(widget), reset);
    }
    if (setting.contains("description") && ADW_IS_ACTION_ROW(widget)) adw_action_row_set_subtitle(ADW_ACTION_ROW(widget), setting.at("description").get_ref<const std::string&>().c_str());
    adw_preferences_group_add(group, widget);
  }
  void updateDetails() {
    if (!details || !snapshots.contains("network")) return;
    const auto& state = snapshots.at("network");
    const bool active = connected && state.value("available", false) && state.value("connected", false);
    const auto set = [this](const char* key, const std::string& value) {
      if (detailRows.contains(key)) adw_action_row_set_subtitle(detailRows.at(key), value.c_str());
    };
    const int kind = state.value("kind", 0);
    set("status", active ? "Connected" : "Disconnected or unavailable");
    set("name", !active ? "—" : kind == 3 ? state.value("ssid", "Wi-Fi") : kind == 2 ? "Wired" : kind == 4 ? "Mobile" : "Network");
    set("interface", active ? state.value("interface", "—") : "—");
    set("address", active && !state.value("address", "").empty() ? state.value("address", "") : "Not available");
    set("signal", active && kind == 3 ? std::to_string(state.value("signal", 0u)) + "%" : "Not applicable");
    set("frequency", active && kind == 3 && state.value("frequency", 0u) ? std::to_string(state.value("frequency", 0u)) + " MHz" : "Not applicable");
    set("vpn", active && state.value("vpn-connected", false) ? "Connected" : "Not connected");
  }
  void connectionDetails() {
    if (details) { adw_dialog_present(details, GTK_WIDGET(window)); return; }
    auto* dialog = adw_alert_dialog_new("Connection Details", "Current connection information updates while this window is open.");
    details = ADW_DIALOG(dialog);
    auto* fields = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    for (const auto& [key, label] : std::vector<std::pair<std::string, std::string>>{
      {"status", "Status"}, {"name", "Connection"}, {"interface", "Interface"}, {"address", "IPv4 address"},
      {"signal", "Wi-Fi signal"}, {"frequency", "Wi-Fi frequency"}, {"vpn", "VPN"}}) {
      auto* item = row(fields, label); detailRows[key] = item;
      adw_action_row_set_subtitle_selectable(item, true);
    }
    auto* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), true);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 360);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(fields));
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), scroll);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "close", "Close", nullptr);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "close");
    onSignal(dialog, "closed", [this] { details = nullptr; detailRows.clear(); });
    updateDetails(); adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
  }
  void dnsEditor(const Json& initial) {
    auto* dialog = adw_alert_dialog_new("DNS Settings", "Changes apply to the active connection profile.");
    auto* fields = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    auto* profile = ADW_COMBO_ROW(adw_combo_row_new()); title(ADW_PREFERENCES_ROW(profile), "DNS provider");
    const char* choices[] = {"Automatic", "Google", "Cloudflare", "NextDNS", "Custom", nullptr};
    auto* model = gtk_string_list_new(choices); adw_combo_row_set_model(profile, G_LIST_MODEL(model)); g_object_unref(model);
    adw_combo_row_set_selected(profile, initial.value("profile", 0u));
    adw_preferences_group_add(fields, GTK_WIDGET(profile));
    auto* servers = adw_entry_row_new(); title(ADW_PREFERENCES_ROW(servers), "DNS server addresses");
    std::string text; for (const auto& address : initial.value("servers", Json::array())) { if (!text.empty()) text += ", "; text += address.get<std::string>(); }
    gtk_editable_set_text(GTK_EDITABLE(servers), text.c_str()); adw_preferences_group_add(fields, servers);
    auto* nextdns = adw_entry_row_new(); title(ADW_PREFERENCES_ROW(nextdns), "NextDNS configuration ID");
    gtk_editable_set_text(GTK_EDITABLE(nextdns), initial.value("nextdns", "").c_str()); adw_preferences_group_add(fields, nextdns);
    adw_preferences_group_set_description(fields, "Separate custom IPv4 or IPv6 addresses with commas. An optional #server-name specifies a TLS server name.");
    auto sync = [profile, servers, nextdns] {
      gtk_widget_set_visible(servers, adw_combo_row_get_selected(profile) == 4);
      gtk_widget_set_visible(nextdns, adw_combo_row_get_selected(profile) == 3);
    };
    sync(); onSignal(profile, "notify::selected", sync, true);
    auto* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), true);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(fields));
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), scroll);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "save", "Save", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "save", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    struct Edit { Settings* self; AdwComboRow* profile; GtkWidget *servers, *nextdns; };
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window), nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Edit> edit(static_cast<Edit*>(data));
        const auto* response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
        if (std::string(response) != "save") return;
        Json args = {{"profile", adw_combo_row_get_selected(edit->profile)},
          {"servers", dns_override::splitServerList(gtk_editable_get_text(GTK_EDITABLE(edit->servers)))},
          {"nextdns", gtk_editable_get_text(GTK_EDITABLE(edit->nextdns))}};
        try { (void)helix::validatedDnsUpdate(args); }
        catch (const std::exception& error) { edit->self->toast(error.what()); edit->self->dnsEditor(args); return; }
        edit->self->client.callAsync("network", "SetDns", g_variant_new("(@a{sv})", helix::toVariant(args)),
          [self = edit->self, args](Json result, std::string error) {
            if (!error.empty() || !result.value("success", false)) {
              self->toast(error.empty() ? result.value("message", "DNS could not be changed") : "DNS could not be changed");
              if (self->connected && self->page == "network") self->dnsEditor(args);
            } else self->toast("DNS settings saved");
            self->scheduleRefresh();
          });
      }, new Edit{this, profile, servers, nextdns});
  }
  void loadDnsEditor() {
    client.callAsync("network", "GetDns", nullptr, [this](Json value, std::string error) {
      if (!error.empty()) toast("DNS settings are unavailable");
      else if (page == "network" && connected) dnsEditor(value);
    });
  }
  void saveApplications(const std::string& id, const Json& baseline, const Json& candidate) {
    client.callAsync("configuration", "UpdateOrderedList", g_variant_new("(s@av@av)", id.c_str(), helix::toVariant(baseline), helix::toVariant(candidate)),
      [this](Json, std::string error) { if (!error.empty()) toast("The list changed or could not be saved. Refresh and try again."); scheduleRefresh(); });
  }
  void ruleEditor(const Json& baseline, std::size_t index, Json initial) {
    auto* dialog = adw_alert_dialog_new(index < baseline.size() ? "Edit Notification Rule" : "Add Notification Rule", "The first matching enabled rule applies. Application matches ignore case; content expressions use regular expressions.");
    auto* fields = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    std::map<std::string, GtkWidget*> entries, flags;
    for (const auto& [key, label] : std::vector<std::pair<std::string, std::string>>{{"name", "Rule name"}, {"match", "Application, desktop ID, or category"}, {"match_content", "Content expression (optional)"}}) {
      auto* entry = adw_entry_row_new(); title(ADW_PREFERENCES_ROW(entry), label);
      gtk_editable_set_text(GTK_EDITABLE(entry), initial.value(key, "").c_str()); entries[key] = entry; adw_preferences_group_add(fields, entry);
    }
    for (const auto& [key, label] : std::vector<std::pair<std::string, std::string>>{{"enabled", "Enable rule"}, {"show_toast", "Show popup"}, {"save_history", "Save in history"}, {"play_sound", "Play notification sound"}, {"bypass_dnd", "Bypass Do Not Disturb"}, {"allow_permanent", "Allow persistent notifications"}}) {
      auto* entry = adw_switch_row_new(); title(ADW_PREFERENCES_ROW(entry), label);
      adw_switch_row_set_active(ADW_SWITCH_ROW(entry), initial.value(key, false)); flags[key] = entry; adw_preferences_group_add(fields, entry);
    }
    std::map<std::string, GtkWidget*> urgency;
    const auto allowed = initial.value("allowed_urgencies", Json::array());
    for (const auto& [key, label] : std::vector<std::pair<std::string, std::string>>{{"low", "Allow low urgency"}, {"normal", "Allow normal urgency"}, {"critical", "Allow critical urgency"}}) {
      auto* entry = adw_switch_row_new(); title(ADW_PREFERENCES_ROW(entry), label);
      adw_switch_row_set_active(ADW_SWITCH_ROW(entry), allowed.empty() || std::ranges::find(allowed, Json(key)) != allowed.end());
      urgency[key] = entry; adw_preferences_group_add(fields, entry);
    }
    auto* override = adw_switch_row_new(); title(ADW_PREFERENCES_ROW(override), "Override application duration");
    adw_switch_row_set_active(ADW_SWITCH_ROW(override), initial.contains("override_duration")); adw_preferences_group_add(fields, override);
    auto* duration = adw_spin_row_new_with_range(-1, 2147483647, 1); title(ADW_PREFERENCES_ROW(duration), "Duration (milliseconds)");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(duration), "-1 uses the default timeout; 0 requests a persistent notification.");
    adw_spin_row_set_digits(ADW_SPIN_ROW(duration), 0); adw_spin_row_set_value(ADW_SPIN_ROW(duration), initial.value("override_duration", 5000));
    gtk_widget_set_visible(duration, initial.contains("override_duration")); adw_preferences_group_add(fields, duration);
    onSignal(override, "notify::active", [override, duration] { gtk_widget_set_visible(duration, adw_switch_row_get_active(ADW_SWITCH_ROW(override))); }, true);
    auto* scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), true); gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(fields)); adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), scroll);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "save", "Save", nullptr);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel"); adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "save", ADW_RESPONSE_SUGGESTED);
    struct Edit { Settings* self; Json baseline, initial; std::size_t index; std::map<std::string, GtkWidget*> entries, flags, urgency; GtkWidget *override, *duration; };
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window), nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Edit> edit(static_cast<Edit*>(data));
        if (std::string(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result)) != "save") return;
        auto rule = edit->initial;
        for (const auto& [key, entry] : edit->entries) rule[key] = gtk_editable_get_text(GTK_EDITABLE(entry));
        for (const auto& [key, entry] : edit->flags) rule[key] = bool(adw_switch_row_get_active(ADW_SWITCH_ROW(entry)));
        rule["allowed_urgencies"] = Json::array();
        for (const std::string key : {"low", "normal", "critical"}) if (adw_switch_row_get_active(ADW_SWITCH_ROW(edit->urgency.at(key)))) rule["allowed_urgencies"].push_back(key);
        if (rule["allowed_urgencies"].empty()) { edit->self->toast("Allow at least one notification urgency"); edit->self->ruleEditor(edit->baseline, edit->index, rule); return; }
        if (rule["allowed_urgencies"].size() == 3) rule["allowed_urgencies"] = Json::array();
        if (adw_switch_row_get_active(ADW_SWITCH_ROW(edit->override))) rule["override_duration"] = std::int64_t(adw_spin_row_get_value(ADW_SPIN_ROW(edit->duration)));
        else rule.erase("override_duration");
        auto next = edit->baseline;
        if (edit->index < next.size()) next.at(edit->index) = rule; else next.push_back(rule);
        try { (void)helix::notificationRules(next); }
        catch (const std::exception& error) { edit->self->toast(error.what()); edit->self->ruleEditor(edit->baseline, edit->index, rule); return; }
        edit->self->saveApplications("notification.filter", edit->baseline, next);
      }, new Edit{this, baseline, initial, index, entries, flags, urgency, override, duration});
  }
  void ruleList(AdwPreferencesGroup* fields, const Json& setting) {
    const auto values = setting.at("value");
    auto* header = row(fields, "Application Rules", values.empty() ? "No application rules" : "The first matching enabled rule applies.");
    adw_action_row_add_suffix(header, button("Add", [this, values] {
      NotificationFilterConfig rule; rule.name = "Rule " + std::to_string(values.size() + 1);
      ruleEditor(values, values.size(), helix::notificationRule(rule));
    }));
    if (setting.value("overridden", false)) adw_action_row_add_suffix(header, button("Reset", [this] {
      client.callAsync("configuration", "Reset", g_variant_new("(s)", "notification.filter"), [this](Json, std::string error) { if (!error.empty()) toast("Rules could not be reset"); scheduleRefresh(); });
    }));
    for (std::size_t i = 0; i < values.size(); ++i) {
      const auto& rule = values.at(i);
      auto* item = row(fields, rule.at("name"), rule.value("enabled", true) ? rule.value("match", "") : "Disabled"); adw_action_row_set_title_lines(item, 1);
      auto* menu = gtk_menu_button_new(); gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "view-more-symbolic"); gtk_widget_set_tooltip_text(menu, "Edit notification rule"); gtk_widget_set_valign(menu, GTK_ALIGN_CENTER);
      gtk_accessible_update_property(GTK_ACCESSIBLE(menu), GTK_ACCESSIBLE_PROPERTY_LABEL, "Edit notification rule", -1);
      auto* popover = gtk_popover_new(); auto* actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); gtk_popover_set_child(GTK_POPOVER(popover), actions);
      gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), popover); adw_action_row_add_suffix(item, menu);
      gtk_box_append(GTK_BOX(actions), button("Edit", [this, popover, values, i] { gtk_popover_popdown(GTK_POPOVER(popover)); ruleEditor(values, i, values.at(i)); }));
      const auto control = [this, popover, actions, values, i](const char* label, int direction) {
        gtk_box_append(GTK_BOX(actions), button(label, [this, popover, values, i, direction] {
          auto next = values; if (!direction) next.erase(next.begin() + i); else std::swap(next.at(i), next.at(i + direction));
          gtk_popover_popdown(GTK_POPOVER(popover)); saveApplications("notification.filter", values, next);
        }));
      };
      if (i) control("Move earlier", -1);
      if (i + 1 < values.size()) control("Move later", 1);
      control("Remove", 0);
    }
  }
  void displayPicker(const std::string& id, const Json& values) {
    auto* dialog = adw_alert_dialog_new("Add Display", "Enter a display output name, such as eDP-1 or DP-1, or an existing display selector. An empty list uses all displays.");
    auto* entry = adw_entry_row_new(); title(ADW_PREFERENCES_ROW(entry), "Display selector");
    auto* fields = adw_preferences_group_new(); adw_preferences_group_add(ADW_PREFERENCES_GROUP(fields), entry);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), fields);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "add", "Add", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "add", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    struct Selection { Settings* self; std::string id; Json values; GtkWidget* entry; };
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window), nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Selection> selection(static_cast<Selection*>(data));
        if (std::string(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result)) != "add") return;
        auto* raw = g_strdup(gtk_editable_get_text(GTK_EDITABLE(selection->entry))); g_strstrip(raw); const std::string value(raw); g_free(raw);
        if (value.empty() || std::ranges::find(selection->values, Json(value)) != selection->values.end()) {
          selection->self->toast("Enter a new display selector"); selection->self->displayPicker(selection->id, selection->values); return;
        }
        auto next = selection->values; next.push_back(value); selection->self->saveApplications(selection->id, selection->values, next);
      }, new Selection{this, id, values, entry});
  }
  void widgetPicker(const Json& setting) {
    const auto id = setting.at("id").get<std::string>(); const auto values = setting.at("value");
    auto* dialog = adw_alert_dialog_new("Add Widget", "Choose a configured widget instance. Plugin instances remain available here.");
    auto* fields = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    for (const auto& option : setting.at("options")) {
      if (std::ranges::find(values, option) != values.end()) continue;
      const auto name = option.get<std::string>(); auto* item = row(fields, name);
      adw_action_row_add_suffix(item, button("Add", [this, id, values, name, dialog] {
        auto next = values; next.push_back(name); saveApplications(id, values, next); adw_dialog_close(ADW_DIALOG(dialog));
      }));
    }
    auto* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), true);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(fields));
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), scroll);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "close", "Close", nullptr);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "close"); adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
  }
  void applicationPicker(const std::string& id, const Json& baseline) {
    auto* dialog = adw_alert_dialog_new("Add Application", "Choose an installed application to pin.");
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    auto* search = gtk_search_entry_new(); gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search), "Search applications");
    gtk_box_append(GTK_BOX(box), search);
    auto* fields = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    auto* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 180);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), true);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(fields)); gtk_box_append(GTK_BOX(box), scroll);
    std::vector<std::pair<GtkWidget*, std::string>> rows;
    std::set<std::string> seen;
    auto* apps = g_app_info_get_all();
    apps = g_list_sort(apps, [](gconstpointer a, gconstpointer b) -> gint {
      return g_utf8_collate(g_app_info_get_display_name(G_APP_INFO(const_cast<gpointer>(a))), g_app_info_get_display_name(G_APP_INFO(const_cast<gpointer>(b))));
    });
    for (auto* node = apps; node; node = node->next) {
      auto* app = G_APP_INFO(node->data); const auto* raw = g_app_info_get_id(app);
      if (!raw || !g_app_info_should_show(app)) continue;
      std::string appId(raw); if (appId.ends_with(".desktop")) appId.resize(appId.size() - 8);
      if (!seen.insert(appId).second || std::ranges::find(baseline, Json(appId)) != baseline.end()) continue;
      const std::string name = g_app_info_get_display_name(app);
      auto* item = row(fields, name, appId); rows.emplace_back(GTK_WIDGET(item), name + " " + appId);
      adw_action_row_add_suffix(item, button("Add", [this, dialog, id, baseline, appId] {
        auto values = baseline; values.push_back(appId); saveApplications(id, baseline, values); adw_dialog_close(ADW_DIALOG(dialog));
      }));
    }
    g_list_free_full(apps, g_object_unref);
    onSignal(search, "search-changed", [search, rows] {
      auto* query = g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(search)), -1);
      for (const auto& [widget, name] : rows) {
        auto* text = g_utf8_casefold(name.c_str(), -1); gtk_widget_set_visible(widget, std::string(text).find(query) != std::string::npos); g_free(text);
      } g_free(query);
    });
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), box);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "close", "Close", nullptr);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "close");
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
  }
  void shortcutPicker(const Json& setting) {
    const auto baseline = setting.at("value");
    const auto options = setting.at("options").get<std::vector<std::string>>();
    const auto labels = setting.at("option-labels");
    std::vector<std::string> available; auto* choices = gtk_string_list_new(nullptr);
    for (std::size_t i = 0; i < options.size(); ++i) {
      if (std::ranges::any_of(baseline, [&](const Json& item) { return item.at("type") == options[i]; })) continue;
      available.push_back(options[i]); gtk_string_list_append(choices, labels.at(i).get<std::string>().c_str());
    }
    if (available.empty()) { g_object_unref(choices); toast("All available shortcuts are already added"); return; }
    auto* dialog = adw_alert_dialog_new("Add Quick Shortcut", "Choose a control to pin to Control Center Home. Availability depends on its desktop feature or device.");
    auto* fields = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    auto* type = ADW_COMBO_ROW(adw_combo_row_new()); title(ADW_PREFERENCES_ROW(type), "Shortcut");
    adw_combo_row_set_model(type, G_LIST_MODEL(choices)); g_object_unref(choices);
    adw_preferences_group_add(fields, GTK_WIDGET(type));
    auto* size = ADW_COMBO_ROW(adw_combo_row_new()); title(ADW_PREFERENCES_ROW(size), "Size");
    const char* sizes[] = {"Compact", "Wide", "Large", nullptr}; auto* sizeModel = gtk_string_list_new(sizes);
    adw_combo_row_set_model(size, G_LIST_MODEL(sizeModel)); g_object_unref(sizeModel);
    adw_preferences_group_add(fields, GTK_WIDGET(size));
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(fields));
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "add", "Add", nullptr);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "add", ADW_RESPONSE_SUGGESTED);
    struct Selection { Settings* self; Json baseline; std::vector<std::string> options; AdwComboRow *type, *size; };
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window), nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Selection> selection(static_cast<Selection*>(data));
        if (std::string(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result)) != "add") return;
        const auto index = adw_combo_row_get_selected(selection->type); if (index >= selection->options.size()) return;
        const char* sizes[] = {"small", "wide", "large"}; const auto size = adw_combo_row_get_selected(selection->size); if (size >= 3) return;
        auto next = selection->baseline; next.push_back({{"type", selection->options[index]}, {"size", sizes[size]}});
        selection->self->saveApplications("control_center.shortcuts", selection->baseline, next);
      }, new Selection{this, baseline, available, type, size});
  }
  void shortcutList(AdwPreferencesGroup* fields, const Json& setting) {
    const auto values = setting.at("value");
    auto* header = row(fields, "Quick Shortcuts", values.empty() ? "No pinned shortcuts" : "Controls appear in this order in Control Center Home.");
    adw_action_row_add_suffix(header, button("Add", [this, setting] { shortcutPicker(setting); }));
    if (setting.value("overridden", false)) adw_action_row_add_suffix(header, button("Reset", [this] {
      client.callAsync("configuration", "Reset", g_variant_new("(s)", "control_center.shortcuts"), [this](Json, std::string error) {
        if (!error.empty()) toast("Quick shortcuts could not be reset");
        scheduleRefresh();
      });
    }));
    std::map<std::string, std::string> labels;
    for (std::size_t i = 0; i < setting.at("options").size(); ++i) labels[setting.at("options").at(i).get<std::string>()] = setting.at("option-labels").at(i).get<std::string>();
    for (std::size_t i = 0; i < values.size(); ++i) {
      const auto type = values.at(i).at("type").get<std::string>();
      const auto label = labels.contains(type) ? labels.at(type) : type;
      auto* item = ADW_COMBO_ROW(adw_combo_row_new()); title(ADW_PREFERENCES_ROW(item), label);
      adw_action_row_set_title_lines(ADW_ACTION_ROW(item), 2);
      const char* sizes[] = {"Compact", "Wide", "Large", nullptr}; auto* model = gtk_string_list_new(sizes);
      adw_combo_row_set_model(item, G_LIST_MODEL(model)); g_object_unref(model);
      const auto size = values.at(i).value("size", "small");
      adw_combo_row_set_selected(item, size == "wide" ? 1 : size == "large" ? 2 : 0);
      adw_preferences_group_add(fields, GTK_WIDGET(item));
      onSignal(item, "notify::selected", [this, item, values, i] {
        const char* sizes[] = {"small", "wide", "large"}; const auto selected = adw_combo_row_get_selected(item); if (selected >= 3) return;
        auto next = values; next.at(i)["size"] = sizes[selected]; saveApplications("control_center.shortcuts", values, next);
      }, true);
      auto* menu = gtk_menu_button_new(); gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "view-more-symbolic");
      const auto accessibleLabel = "Edit " + label; gtk_widget_set_tooltip_text(menu, accessibleLabel.c_str());
      gtk_accessible_update_property(GTK_ACCESSIBLE(menu), GTK_ACCESSIBLE_PROPERTY_LABEL, accessibleLabel.c_str(), -1);
      gtk_widget_set_valign(menu, GTK_ALIGN_CENTER);
      auto* popover = gtk_popover_new(); auto* actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
      gtk_popover_set_child(GTK_POPOVER(popover), actions); gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), popover);
      adw_action_row_add_suffix(ADW_ACTION_ROW(item), menu);
      const auto control = [this, popover, actions, values, i](const char* label, int direction) {
        gtk_box_append(GTK_BOX(actions), button(label, [this, popover, values, i, direction] {
          auto next = values; if (!direction) next.erase(next.begin() + i); else std::swap(next.at(i), next.at(i + direction));
          gtk_popover_popdown(GTK_POPOVER(popover)); saveApplications("control_center.shortcuts", values, next);
        }));
      };
      if (i) control("Move earlier", -1);
      if (i + 1 < values.size()) control("Move later", 1);
      control("Remove", 0);
    }
  }
  void tabVisibility(AdwPreferencesGroup* fields, const Json& setting) {
    const auto id = setting.at("id").get<std::string>();
    const auto visible = setting.at("value");
    const auto options = setting.at("options").get<std::vector<std::string>>();
    const auto labels = setting.value("option-labels", Json::array());
    const std::string summary = std::to_string(visible.size()) + " of " + std::to_string(options.size()) + " pages shown. Home is always available.";
    auto* header = row(fields, setting.at("title"), summary);
    adw_action_row_add_suffix(header, button("Choose Pages", [this, id, visible, options, labels] {
      auto* dialog = adw_alert_dialog_new("Control Center Pages", "Choose the pages that appear in Control Center. Some pages also require an enabled feature or available device. Extension pages keep their current visibility.");
      auto* scroll = gtk_scrolled_window_new();
      gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
      gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), true);
      gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 420);
      auto* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
      std::vector<GtkCheckButton*> choices;
      for (std::size_t i = 0; i < options.size(); ++i) {
        const std::string label = i < labels.size() ? labels.at(i).get<std::string>() : options[i];
        auto* choice = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(label.c_str()));
        gtk_label_set_wrap(GTK_LABEL(gtk_check_button_get_child(choice)), true);
        gtk_widget_set_margin_start(GTK_WIDGET(choice), 12); gtk_widget_set_margin_end(GTK_WIDGET(choice), 12);
        gtk_widget_set_margin_top(GTK_WIDGET(choice), 5); gtk_widget_set_margin_bottom(GTK_WIDGET(choice), 5);
        gtk_check_button_set_active(choice, std::ranges::find(visible, Json(options[i])) != visible.end());
        gtk_box_append(GTK_BOX(list), GTK_WIDGET(choice)); choices.push_back(choice);
      }
      gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
      adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), scroll);
      adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "apply", "Apply", nullptr);
      adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "apply", ADW_RESPONSE_SUGGESTED);
      adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
      struct Edit { Settings* self; std::string id; std::vector<std::string> options; std::vector<GtkCheckButton*> choices; Json baseline; };
      adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<Edit> edit(static_cast<Edit*>(data));
          const auto* response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
          if (std::string(response) != "apply") return;
          Json candidate = Json::array();
          for (std::size_t i = 0; i < edit->choices.size(); ++i)
            if (gtk_check_button_get_active(edit->choices[i])) candidate.push_back(edit->options[i]);
          edit->self->saveApplications(edit->id, edit->baseline, candidate);
        }, new Edit{this, id, options, std::move(choices), visible});
    }));
    if (setting.value("overridden", false)) adw_action_row_add_suffix(header, button("Reset", [this, id] {
      client.callAsync("configuration", "Reset", g_variant_new("(s)", id.c_str()), [this](Json, std::string error) {
        if (!error.empty()) toast("Control Center pages could not be reset");
        scheduleRefresh();
      });
    }));
  }
  void applicationList(AdwPreferencesGroup* fields, const Json& setting) {
    const auto id = setting.at("id").get<std::string>(); const auto values = setting.at("value");
    const bool widgets = setting.at("type") == "widget-list";
    const bool displays = setting.at("type") == "display-list";
    auto* header = row(fields, setting.at("title"), values.empty() ? (displays ? "All displays" : widgets ? "No widgets in this section" : "No pinned applications") : "Items appear in this order.");
    adw_action_row_add_suffix(header, button("Add", [this, id, values, widgets, displays, setting] { if (displays) displayPicker(id, values); else if (widgets) widgetPicker(setting); else applicationPicker(id, values); }));
    if (setting.value("overridden", false)) adw_action_row_add_suffix(header, button("Reset", [this, id] {
      client.callAsync("configuration", "Reset", g_variant_new("(s)", id.c_str()), [this](Json, std::string error) { if (!error.empty()) toast("Favorites could not be reset"); scheduleRefresh(); });
    }));
    std::map<std::string, std::string> names;
    auto* apps = widgets || displays ? nullptr : g_app_info_get_all();
    for (auto* node = apps; node; node = node->next) {
      auto* app = G_APP_INFO(node->data); const auto* raw = g_app_info_get_id(app); if (!raw) continue;
      std::string key(raw); if (key.ends_with(".desktop")) key.resize(key.size() - 8);
      names[key] = g_app_info_get_display_name(app);
    } g_list_free_full(apps, g_object_unref);
    for (std::size_t i = 0; i < values.size(); ++i) {
      const auto appId = values.at(i).get<std::string>();
      auto* item = row(fields, names.contains(appId) ? names.at(appId) : appId, widgets ? (std::ranges::find(setting.at("options"), Json(appId)) == setting.at("options").end() ? "Widget is unavailable; its position is retained." : "Configured widget instance") : displays ? "Display selector" : names.contains(appId) ? appId : "Application is unavailable; its saved position is retained.");
      adw_action_row_set_title_lines(item, 1);
      gtk_widget_set_tooltip_text(GTK_WIDGET(item), appId.c_str());
      auto* menu = gtk_menu_button_new(); gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "view-more-symbolic");
      gtk_widget_set_tooltip_text(menu, "Edit favorite"); gtk_widget_set_valign(menu, GTK_ALIGN_CENTER);
      gtk_accessible_update_property(GTK_ACCESSIBLE(menu), GTK_ACCESSIBLE_PROPERTY_LABEL, "Edit favorite", -1);
      auto* popover = gtk_popover_new(); auto* actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
      gtk_popover_set_child(GTK_POPOVER(popover), actions); gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), popover);
      adw_action_row_add_suffix(item, menu);
      auto control = [this, actions, popover, id, values, i](const char*, const char* label, int direction) {
        auto* button = gtk_button_new_with_label(label); gtk_widget_set_tooltip_text(button, label);
        gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
        gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
        onSignal(button, "clicked", [this, popover, id, values, i, direction] {
          auto next = values;
          if (!direction) next.erase(next.begin() + i); else std::swap(next.at(i), next.at(i + direction));
          gtk_popover_popdown(GTK_POPOVER(popover)); saveApplications(id, values, next);
        }); gtk_box_append(GTK_BOX(actions), button);
      };
      if (i) control("go-up-symbolic", "Move earlier", -1);
      if (i + 1 < values.size()) control("go-down-symbolic", "Move later", 1);
      control("list-remove-symbolic", displays ? "Remove display" : widgets ? "Remove widget" : "Remove favorite", 0);
    }
  }
  void chooseFolder(const std::string& id) {
    auto* dialog = gtk_file_dialog_new(); gtk_file_dialog_set_title(dialog, "Choose Wallpaper Folder");
    struct Selection { Settings* self; std::string id; };
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(window), nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Selection> selection(static_cast<Selection*>(data)); GError* error = nullptr;
        auto* file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
        if (file) {
          auto* path = g_file_get_path(file);
          if (path) selection->self->update(selection->id, std::string(path));
          else selection->self->toast("Choose a local folder");
          g_free(path); g_object_unref(file);
        }
        if (error) {
          if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) selection->self->toast("Folder could not be selected");
          g_error_free(error);
        }
      }, new Selection{this, id});
    g_object_unref(dialog);
  }
  void chooseAccent() {
    auto* dialog = gtk_color_dialog_new(); gtk_color_dialog_set_title(dialog, "Accent Color");
    gtk_color_dialog_set_with_alpha(dialog, false);
    gtk_color_dialog_choose_rgba(dialog, GTK_WINDOW(window), nullptr, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        auto& self = *static_cast<Settings*>(data);
        GError* error = nullptr;
        auto* color = gtk_color_dialog_choose_rgba_finish(GTK_COLOR_DIALOG(source), result, &error);
        if (color) {
          auto* text = g_strdup_printf("#%02x%02x%02x", unsigned(color->red * 255), unsigned(color->green * 255), unsigned(color->blue * 255));
          self.client.callAsync("appearance", "SetAccent", g_variant_new("(s)", text), [&self](Json, std::string message) {
            if (!message.empty()) self.toast("Accent could not be saved");
            self.scheduleRefresh();
          });
          g_free(text); gdk_rgba_free(color);
        }
        if (error) g_error_free(error);
      }, this);
    g_object_unref(dialog);
  }
  void chooseWallpaper(bool remote = false) {
    if (!connected || !snapshots["appearance"].value("helix-desktop", false)) { toast("Wallpaper browsing requires a connected Helix desktop"); return; }
    if (auto browser = wallpaperBrowser.lock(); browser && !browser->isClosed()) { browser->present(remote); return; }
    const auto& appearance = snapshots.at("appearance");
    std::string folder;
    const auto mode = appearance.value("mode", "dark");
    for (const auto& setting : catalog) if (setting.value("id", "") == "wallpaper.directory_" + mode) folder = setting.value("value", "");
    if (folder.empty()) for (const auto& setting : catalog) if (setting.value("id", "") == "wallpaper.directory") folder = setting.value("value", "");
    if (folder.empty()) folder = FileUtils::defaultPicturesDirectory().string();
    wallpaperBrowser = helix::settings::WallpaperBrowser::open(GTK_WINDOW(window), FileUtils::expandUserPath(folder), appearance, remote, [this] { scheduleRefresh(); });
  }
  void choosePalette() {
    if (!connected || !snapshots.contains("appearance") || !snapshots.at("appearance").value("available", false)) {
      toast("Palette browsing requires connected Helix services"); return;
    }
    if (auto browser = paletteBrowser.lock(); browser && !browser->isClosed()) { browser->present(); return; }
    std::string source = "builtin", name;
    for (const auto& setting : catalog) if (setting.value("id", "") == "theme.source") source = setting.at("value").get<std::string>();
    const std::string id = source == "community" ? "theme.community_palette" : source == "custom" ? "theme.custom_palette" : "theme.builtin";
    for (const auto& setting : catalog) if (setting.value("id", "") == id) name = setting.at("value").get<std::string>();
    paletteBrowser = helix::settings::PaletteBrowser::open(GTK_WINDOW(window), snapshots.at("appearance"), source, name,
      [this](std::string selectedSource, std::string selectedName, std::function<void(bool)> finished) {
        const std::string id = selectedSource == "community" ? "theme.community_palette" : selectedSource == "custom" ? "theme.custom_palette" : "theme.builtin";
        client.callAsync("configuration", "Update", g_variant_new("(sv)", id.c_str(), g_variant_new_string(selectedName.c_str())),
          [this, selectedSource, finished = std::move(finished)](Json, std::string error) {
            if (!error.empty()) { finished(false); scheduleRefresh(); return; }
            client.callAsync("configuration", "Update", g_variant_new("(sv)", "theme.source", g_variant_new_string(selectedSource.c_str())),
              [this, finished](Json, std::string error) { finished(error.empty()); scheduleRefresh(); });
          });
      });
  }
  void chooseWallpaperFile() {
    auto* dialog = gtk_file_dialog_new(); gtk_file_dialog_set_title(dialog, "Choose Wallpaper");
    gtk_file_dialog_open(dialog, GTK_WINDOW(window), nullptr, [](GObject* source, GAsyncResult* result, gpointer data) {
      auto& self = *static_cast<Settings*>(data);
      GError* error = nullptr;
      auto* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
      if (file) {
        auto* path = g_file_get_path(file);
        if (path) self.client.callAsync("appearance", "SetWallpaper", g_variant_new("(s)", path),
          [&self](Json, std::string message) { if (!message.empty()) self.toast("Wallpaper could not be changed"); self.scheduleRefresh(); });
        g_free(path); g_object_unref(file);
      }
      if (error) g_error_free(error);
    }, this);
    g_object_unref(dialog);
  }
  void prompt(const std::string& domain, const Json& request) {
    auto* dialog = adw_alert_dialog_new(domain == "network" ? "Network Authentication" : "Bluetooth Pairing", nullptr);
    std::string text;
    bool entry = domain == "network" || request.value("kind", 0) == 1 || request.value("kind", 0) == 2;
    if (domain == "network") text = "Enter the password for " + request.value("ssid", "this network") + ".";
    else if (request.value("kind", 0) == 5) text = "Confirm that both devices show " + std::to_string(request.value("passkey", 0)) + ".";
    else if (request.value("kind", 0) == 3) text = "Enter " + request.value("pin", "") + " on the other device.";
    else if (request.value("kind", 0) == 4) text = "Enter " + std::to_string(request.value("passkey", 0)) + " on the other device.";
    else text = entry ? "Enter the pairing code." : "Allow this device to pair?";
    adw_alert_dialog_set_body(ADW_ALERT_DIALOG(dialog), text.c_str());
    GtkWidget* input = nullptr;
    if (entry) {
      input = gtk_password_entry_new(); gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(input), true);
      adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), input);
    }
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "confirm", entry ? "Connect" : "Confirm", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "confirm", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    struct Reply { Settings* settings; std::string domain; GtkWidget* input; };
    auto* reply = new Reply{this, domain, input};
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window), nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Reply> reply(static_cast<Reply*>(data));
        const auto* response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
        const auto secret = reply->input ? gtk_editable_get_text(GTK_EDITABLE(reply->input)) : "";
        reply->settings->client.callAsync(reply->domain, "Reply", g_variant_new("(sb)", secret, std::string(response) != "confirm"),
          [settings = reply->settings](Json, std::string error) { if (!error.empty()) settings->toast("The request is no longer available"); settings->scheduleRefresh(); });
      }, reply);
  }
  void password(const Json& network) {
    const bool enterprise = network.value("enterprise", false);
    if (enterprise && network.value("key-management", 0) == 3) {
      toast("This network requires a client certificate. Certificate authentication is not available here yet."); return;
    }
    auto* dialog = adw_alert_dialog_new("Connect to Network", enterprise ? "Enter your organization's network credentials." : "Enter the network password.");
    auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    auto* input = gtk_password_entry_new(); gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(input), true);
    gtk_accessible_update_property(GTK_ACCESSIBLE(input), GTK_ACCESSIBLE_PROPERTY_LABEL, "Network password", -1);
    GtkWidget *identity = nullptr, *anonymous = nullptr, *certificate = nullptr, *domain = nullptr;
    AdwComboRow *method = nullptr, *phase = nullptr;
    if (enterprise) {
      auto* fields = adw_preferences_group_new();
      auto entry = [fields](const char* label) {
        auto* widget = adw_entry_row_new(); adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widget), label);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(fields), widget); return widget;
      };
      identity = entry("Identity"); anonymous = entry("Anonymous identity (optional)");
      domain = entry("Authentication server domain"); certificate = entry("CA certificate path (optional)");
      auto combo = [fields](const char* label, const char* const* choices) {
        auto* widget = ADW_COMBO_ROW(adw_combo_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widget), label);
        auto* model = gtk_string_list_new(choices); adw_combo_row_set_model(widget, G_LIST_MODEL(model)); g_object_unref(model);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(fields), GTK_WIDGET(widget)); return widget;
      };
      const char* methods[] = {"PEAP", "TTLS", nullptr}; method = combo("EAP method", methods);
      const char* phases[] = {"MSCHAPv2", "PAP", "MSCHAP", "CHAP", nullptr}; phase = combo("Inner authentication", phases);
      gtk_box_append(GTK_BOX(content), fields);
    }
    gtk_box_append(GTK_BOX(content), input);
    if (enterprise) {
      auto* scroll = gtk_scrolled_window_new();
      gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
      gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), true);
      gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
      gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);
      adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), scroll);
    } else adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), content);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "connect", "Connect", nullptr);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    struct Request { Settings* settings; GtkWidget *input, *identity, *anonymous, *certificate, *domain; AdwComboRow *method, *phase; Json network; };
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window), nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Request> request(static_cast<Request*>(data));
        const auto* response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
        if (std::string(response) != "connect") return;
        Json args = {{"path", request->network.at("path")}, {"secret", gtk_editable_get_text(GTK_EDITABLE(request->input))}};
        if (request->identity) {
          args["identity"] = gtk_editable_get_text(GTK_EDITABLE(request->identity));
          args["anonymous"] = gtk_editable_get_text(GTK_EDITABLE(request->anonymous));
          args["certificate"] = gtk_editable_get_text(GTK_EDITABLE(request->certificate));
          args["domain"] = gtk_editable_get_text(GTK_EDITABLE(request->domain));
          args["eap"] = adw_combo_row_get_selected(request->method); args["phase2"] = adw_combo_row_get_selected(request->phase);
        }
        request->settings->act("network", request->identity ? "enterprise" : "connect", args);
      }, new Request{this, input, identity, anonymous, certificate, domain, method, phase, network});
  }

  void devices() {
    if (page != "sound" && page != "network" && page != "bluetooth") return;
    const std::string domain = page == "sound" ? "audio" : page;
    if (!snapshots.contains(domain)) return;
    const auto& state = snapshots.at(domain);
    if (!state.value("available", false)) {
      auto* status = adw_status_page_new(); adw_status_page_set_title(ADW_STATUS_PAGE(status), "Service Unavailable");
      adw_status_page_set_description(ADW_STATUS_PAGE(status), "The device backend is not available on this system.");
      gtk_box_append(body, status); return;
    }
    if (domain == "audio") {
      for (const std::string direction : {"sinks", "sources"}) {
        auto* section = group(direction == "sinks" ? "Output" : "Input");
        for (const auto& device : state.at(direction)) {
          const auto id = device.at("id").get<std::uint32_t>();
          if (!matches(device.value("description", ""))) continue;
          auto* item = row(section, device.value("description", "Audio Device"), device.value("default", false) ? "Default device" : "");
          adw_action_row_add_suffix(item, button("Use", [this, id, direction] { act("audio", "default", {{"id", id}, {"input", direction == "sources"}}); }));
          auto* volume = adw_spin_row_new_with_range(0, 150, 1);
          title(ADW_PREFERENCES_ROW(volume), "Volume (%)");
          adw_spin_row_set_value(ADW_SPIN_ROW(volume), device.at("volume").get<double>() * 100);
          const auto key = "audio-volume-" + std::to_string(id);
          setters[key] = [volume](const Json& value) { adw_spin_row_set_value(ADW_SPIN_ROW(volume), value.get<double>() * 100); };
          onSignal(volume, "notify::value", [this, volume, id] {
            if (!syncing) act("audio", "volume", {{"id", id}, {"value", adw_spin_row_get_value(ADW_SPIN_ROW(volume)) / 100}});
          }, true);
          adw_preferences_group_add(section, volume);
          auto* mute = adw_switch_row_new(); title(ADW_PREFERENCES_ROW(mute), "Mute");
          adw_switch_row_set_active(ADW_SWITCH_ROW(mute), device.value("muted", false));
          setters["audio-mute-" + std::to_string(id)] = [mute](const Json& value) { adw_switch_row_set_active(ADW_SWITCH_ROW(mute), value.get<bool>()); };
          onSignal(mute, "notify::active", [this, mute, id] { if (!syncing) act("audio", "mute", {{"id", id}, {"value", bool(adw_switch_row_get_active(ADW_SWITCH_ROW(mute)))}}); }, true);
          adw_preferences_group_add(section, mute);
          if (direction == "sources") {
            auto* test = row(section, "Microphone level", "Live level only; audio is not saved.");
            const bool active = meterSource == id;
            adw_action_row_add_suffix(test, button(active ? "Stop" : "Test", [this, id, active] {
              if (active) { stopMeter(); rendered.clear(); render(); } else startMeter(id);
            }));
            if (active) {
              meterRow = test; meterBar = GTK_LEVEL_BAR(gtk_level_bar_new_for_interval(0, 1));
              gtk_widget_set_hexpand(GTK_WIDGET(meterBar), true);
              gtk_accessible_update_property(GTK_ACCESSIBLE(meterBar), GTK_ACCESSIBLE_PROPERTY_LABEL, "Microphone signal level", -1);
              adw_preferences_group_add(section, GTK_WIDGET(meterBar));
            }
          }
        }
      }
    } else {
      auto* controls = group(domain == "network" ? "Wi-Fi" : "Bluetooth");
      auto* enabled = adw_switch_row_new(); title(ADW_PREFERENCES_ROW(enabled), domain == "network" ? "Wi-Fi" : "Bluetooth");
      adw_switch_row_set_active(ADW_SWITCH_ROW(enabled), state.value("enabled", false));
      setters[domain + "-enabled"] = [enabled](const Json& value) { adw_switch_row_set_active(ADW_SWITCH_ROW(enabled), value.get<bool>()); };
      onSignal(enabled, "notify::active", [this, enabled, domain] { if (!syncing) act(domain, "enable", {{"value", bool(adw_switch_row_get_active(ADW_SWITCH_ROW(enabled)))}}); }, true);
      adw_preferences_group_add(controls, enabled);
      auto* scan = row(controls, domain == "network" ? "Find Networks" : "Find Devices", state.value("scanning", false) ? "Scanning" : "");
      adw_action_row_add_suffix(scan, button("Scan", [this, domain] { act(domain, "scan"); }));
      if (domain == "bluetooth") adw_action_row_add_suffix(scan, button("Stop", [this] { act("bluetooth", "stop-scan"); }));
      if (domain == "network") {
        if (state.value("connected", false)) {
          auto* active = row(controls, state.value("ssid", "Connected"), state.value("address", "") + "  " + state.value("interface", ""));
          adw_action_row_add_suffix(active, button("Disconnect", [this] { act("network", "disconnect"); }));
        }
        auto* detail = row(controls, "Connection Details", "Addresses, interface and connection status");
        adw_action_row_add_suffix(detail, button("View", [this] { connectionDetails(); }));
        auto* dns = row(controls, "DNS", state.value("connected", false) ? "Configure DNS for the active connection" : "Connect to a network to configure DNS");
        auto* edit = button("Edit", [this] { loadDnsEditor(); }); gtk_widget_set_sensitive(edit, state.value("connected", false));
        adw_action_row_add_suffix(dns, edit);
        auto* connections = group("Connections");
        if (state.value("wired", false) && matches("Wired Ethernet")) {
          const bool active = state.value("kind", 0) == 2 && state.value("connected", false);
          auto* wired = row(connections, "Wired", active ? "Connected" : "Connect an Ethernet cable to use this connection.");
          adw_action_row_add_suffix(wired, button(active ? "Disconnect" : "Connect", [this, active] { act("network", active ? "disconnect" : "wired"); }));
        }
        if (state.value("cellular", false) && matches("Mobile Cellular")) {
          const bool active = state.value("cellular-active", false);
          auto* cellular = row(connections, "Mobile Network", active ? "Connected" : "Disconnected");
          adw_action_row_add_suffix(cellular, button(active ? "Disconnect" : "Connect", [this, active] { act("network", active ? "cellular-disconnect" : "cellular-connect"); }));
        }
        for (const auto& vpn : state.at("vpns")) {
          const auto name = vpn.value("name", "VPN"); if (!matches(name + " VPN")) continue;
          const bool active = vpn.value("active", false); const auto path = vpn.at("path");
          auto* item = row(connections, name, active ? "VPN connected" : "VPN disconnected");
          adw_action_row_add_suffix(item, button(active ? "Disconnect" : "Connect", [this, path, active] { act("network", active ? "vpn-disconnect" : "vpn-connect", {{"path", path}}); }));
        }
        auto* networks = group("Available Networks");
        for (const auto& network : state.at("access-points")) {
          auto name = network.value("ssid", ""); if (name.empty() || !matches(name)) continue;
          auto* item = row(networks, name, std::to_string(network.at("strength").get<unsigned>()) + "% signal" + (network.value("secured", false) ? " · Secured" : ""));
          adw_action_row_add_suffix(item, button(network.value("active", false) ? "Connected" : "Connect", [this, network] {
            if (network.value("secured", false) && !network.value("saved", false)) password(network);
            else act("network", "connect", {{"path", network.at("path")}});
          }));
          if (network.value("saved", false)) adw_action_row_add_suffix(item, button("Forget", [this, name] { act("network", "forget", {{"ssid", name}}); }));
        }
      } else {
        auto* list = group("Devices");
        for (const auto& device : state.at("devices")) {
          auto name = device.value("name", "Bluetooth Device"); if (!matches(name)) continue;
          const auto path = device.at("path");
          std::string subtitle = device.value("connected", false) ? "Connected" : device.value("paired", false) ? "Paired" : "Not paired";
          if (device.value("has-battery", false)) subtitle += " · " + std::to_string(device.at("battery").get<unsigned>()) + "% battery";
          auto* item = row(list, name, subtitle);
          if (!device.value("paired", false)) adw_action_row_add_suffix(item, button("Pair", [this, path] { act("bluetooth", "pair", {{"path", path}}); }));
          else {
            bool active = device.value("connected", false);
            adw_action_row_add_suffix(item, button(active ? "Disconnect" : "Connect", [this, path, active] { act("bluetooth", active ? "disconnect" : "connect", {{"path", path}}); }));
            adw_action_row_add_suffix(item, button("Forget", [this, path] { act("bluetooth", "forget", {{"path", path}}); }));
          }
        }
      }
    }
  }
  void render() {
    if (!body) return;
    Json shape = {{"meter", meterSource}, {"pending-operations", Json::array()}, {"page", page}, {"query", query}, {"connected", connected}, {"settings", catalog}};
    for (const auto& [id, operation] : operations) shape["pending-operations"].push_back(id);
    for (auto& setting : shape["settings"]) if (setting.value("type", "") != "application-list" && setting.value("type", "") != "widget-list" && setting.value("type", "") != "display-list" && setting.value("type", "") != "notification-rules" && setting.value("type", "") != "tab-list" && setting.value("type", "") != "shortcut-list") setting.erase("value");
    const std::string domain = page == "sound" ? "audio" : page;
    if (snapshots.contains(domain)) {
      shape["devices"] = snapshots.at(domain);
      if (domain == "audio") for (const std::string kind : {"sinks", "sources"})
        if (shape["devices"].contains(kind)) for (auto& device : shape["devices"][kind]) { device.erase("volume"); device.erase("muted"); }
      if (domain == "network" || domain == "bluetooth") shape["devices"].erase("enabled");
    }
    if (page == "about" && snapshots.contains("configuration")) shape["system"] = snapshots.at("configuration");
    const auto fingerprint = shape.dump();
    syncing = true;
    if (fingerprint != rendered) {
      rendered = fingerprint; setters.clear(); meterBar = nullptr; meterRow = nullptr;
      paletteSwatches = 0; wallpaperPreview = nullptr;
      layoutPreview = nullptr;
      appearanceModes.clear();
      sectionGroups.clear();
      while (auto* child = gtk_widget_get_first_child(GTK_WIDGET(body))) gtk_box_remove(body, child);
      if (!operations.empty()) {
        auto* progress = group("In Progress");
        for (const auto& [id, operation] : operations) {
          auto* item = row(progress, operation.domain == "network" ? "Network operation" : operation.domain == "bluetooth" ? "Bluetooth operation" : "Audio operation", operation.action);
          auto* spinner = gtk_spinner_new(); gtk_spinner_start(GTK_SPINNER(spinner));
          gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER); adw_action_row_add_suffix(item, spinner);
          if (operation.cancellable) adw_action_row_add_suffix(item, button("Cancel", [this, id, domain = operation.domain] {
            client.callAsync(domain, "Cancel", g_variant_new("(t)", id), [this](Json, std::string error) {
              if (!error.empty()) toast("The operation is no longer cancellable");
              scheduleRefresh();
            });
          }));
        }
      }
      devices();
      if (page == "appearance") appearancePreview();
      if (query.empty() && (page == "desktop-dock" || page == "notifications") &&
          snapshots.contains("configuration") && snapshots.at("configuration").value("helix-desktop", false)) {
        auto* preview = group(page == "notifications" ? "Notification Preview" : "Your Desktop Layout");
        adw_preferences_group_set_description(preview, page == "notifications" ?
          "See popup placement and visible actions as you adjust the settings below." :
          "See your bar, dock and active corners. This is an illustration; display-specific layouts may differ.");
        layoutPreview = helix::settings::LayoutPreview::create(page == "notifications");
        adw_preferences_group_add(preview, layoutPreview);
      }
      if (page == "system" && std::ranges::any_of(catalog, [](const Json& item) { return item.value("group-overridden", false); })) {
        auto* reset = row(group("Idle Timers"), "Restore configured idle timers", "Resets the timer collection, including its original commands and order.");
        adw_action_row_add_suffix(reset, button("Reset", [this] {
          client.callAsync("configuration", "Reset", g_variant_new("(s)", "idle.behavior"), [this](Json, std::string error) { if (!error.empty()) toast("Idle timers could not be reset"); scheduleRefresh(); });
        }));
      }
      if (page == "desktop-dock" && connected && snapshots.contains("configuration") && snapshots.at("configuration").value("helix-desktop", false)) {
        if (matches("Extensions Widgets Automation Hooks Niri Keyboard Agents Advanced Desktop Settings")) {
          auto* pending = group("Advanced Desktop Settings");
          adw_preferences_group_set_description(pending, "Your existing configuration remains active. These controls are not yet available in Settings.");
          for (const auto& name : {"Extension, widget and automation editors", "Keyboard shortcuts and advanced Niri configuration"}) {
            auto* item = row(pending, name, "Not yet available");
            gtk_widget_set_sensitive(GTK_WIDGET(item), false);
          }
        }
      }
      if (page == "about" && snapshots.contains("configuration")) {
        const auto& state = snapshots.at("configuration"); auto* system = group("System");
        if (state.contains("system")) for (const auto& [key, label] : std::vector<std::pair<std::string, std::string>>{{"os", "Operating system"}, {"kernel", "Kernel"}, {"architecture", "Architecture"}}) {
          auto* item = row(system, label, state.at("system").value(key, "Unavailable")); adw_action_row_set_subtitle_selectable(item, true);
        }
        auto* services = group("Helix Services");
        for (const auto& [key, label] : std::vector<std::pair<std::string, std::string>>{{"helix-desktop", "Helix desktop"}, {"audio", "Audio"}, {"network", "Network"}, {"bluetooth", "Bluetooth"}})
          row(services, label, state.value(key, false) ? "Available" : "Unavailable on this session");
      }
      std::map<std::string, AdwPreferencesGroup*> groups;
      auto orderedCatalog = catalog;
      if (query.empty() && page == "desktop-dock") {
        auto rank = [](const Json& setting) {
          const auto name = setting.value("group", "");
          if (name == "Dock") return 0;
          if (name == "Dock Favorites") return 1;
          if (name == "Dock Layout") return 2;
          if (name.starts_with("Bar · ")) return 3;
          if (name == "Launcher" || name == "Launcher Favorites") return 4;
          if (name == "Control Center") return 5;
          if (name == "On-Screen Indicators" || name == "Indicator Events") return 6;
          if (name == "Hot Corners") return 7;
          return 8;
        };
        std::stable_sort(orderedCatalog.begin(), orderedCatalog.end(), [&](const Json& a, const Json& b) { return rank(a) < rank(b); });
      }
      for (const auto& setting : orderedCatalog) {
        if ((query.empty() && setting.value("page", "") != page) || !matches(setting.value("title", "") + " " + setting.value("group", "") + " " + setting.value("page", ""))) continue;
        const auto name = (query.empty() ? std::string{} : setting.value("page", "") + " · ") + setting.at("group").get<std::string>();
        if (!groups.contains(name)) groups[name] = group(name);
        addSetting(groups[name], setting);
      }
      if (page == "appearance") {
        auto* palette = row(group("Browse Palettes"), "Palette Library", "Compare built-in, community and custom palettes before applying one.");
        adw_action_row_add_suffix(palette, button("Browse", [this] { choosePalette(); }));
        auto* catalogActions = row(group("Palette Catalogs"), "Community Palettes", "Refresh available palettes from the community catalog.");
        adw_action_row_add_suffix(catalogActions, button("Refresh", [this] {
          client.callAsync("appearance", "RefreshPalettes", nullptr, [this](Json, std::string error) {
            toast(error.empty() ? "Refreshing the palette catalog" : "Palette catalog could not be refreshed"); scheduleRefresh();
          });
        }));
        auto* save = row(group("Custom Palettes"), "Save Current Palette");
        adw_action_row_add_suffix(save, button("Save", [this] {
          client.callAsync("appearance", "SavePalette", g_variant_new("(s)", "current"), [this](Json, std::string error) {
            toast(error.empty() ? "Palette saved" : "Palette could not be saved"); scheduleRefresh();
          });
        }));
        auto* accent = row(group("Accent"), "Accent Color", "Creates a custom palette from the current appearance.");
        adw_action_row_add_suffix(accent, button("Choose", [this] { chooseAccent(); }));
      }
      if (page == "appearance" && snapshots.contains("configuration") && snapshots["configuration"].value("helix-desktop", false)) {
        if (matches("Templates Icons Advanced Appearance")) {
          auto* pending = row(group("Advanced Appearance"), "Icon and template editors", "Not yet available in Settings. Your existing configuration remains active.");
          gtk_widget_set_sensitive(GTK_WIDGET(pending), false);
        }
        auto* wallpaper = row(group("Wallpaper"), "Desktop Wallpaper");
        adw_action_row_add_suffix(wallpaper, button("Browse", [this] { chooseWallpaper(); }));
        auto* file = row(group("Wallpaper Files"), "Choose a Local File", "Use an image or video supported by Helix Shell.");
        adw_action_row_add_suffix(file, button("Choose", [this] { chooseWallpaperFile(); }));
        auto* wallhaven = row(group("Wallhaven"), "Browse Wallhaven", "Search online wallpapers using your existing Wallhaven preferences and credentials.");
        adw_action_row_add_suffix(wallhaven, button("Browse", [this] { chooseWallpaper(true); }));
      }
      if (!gtk_widget_get_first_child(GTK_WIDGET(body))) {
        auto* status = adw_status_page_new(); adw_status_page_set_title(ADW_STATUS_PAGE(status), connected ? "No Matching Settings" : "Connect to Helix Services");
        adw_status_page_set_description(ADW_STATUS_PAGE(status), connected ? "Try a different search." : "Settings will appear when the service becomes available."); gtk_box_append(body, status);
      }
    }
    for (const auto& setting : catalog) {
      const auto id = setting.at("id").get<std::string>();
      if (setters.contains(id)) setters.at(id)(setting.at("value"));
      if (id == "theme.mode") for (const auto& [mode, widget] : appearanceModes) gtk_toggle_button_set_active(widget, setting.at("value") == mode);
    }
    if (snapshots.contains("audio")) for (const std::string kind : {"sinks", "sources"})
      if (snapshots["audio"].contains(kind)) for (const auto& device : snapshots["audio"][kind]) {
        auto id = std::to_string(device.at("id").get<unsigned>());
        if (setters.contains("audio-volume-" + id)) setters.at("audio-volume-" + id)(device.at("volume"));
        if (setters.contains("audio-mute-" + id)) setters.at("audio-mute-" + id)(device.at("muted"));
      }
    for (const std::string kind : {"network", "bluetooth"}) if (setters.contains(kind + "-enabled")) setters.at(kind + "-enabled")(snapshots.at(kind).at("enabled"));
    if (layoutPreview) helix::settings::LayoutPreview::update(layoutPreview, catalog);
    gtk_widget_set_sensitive(GTK_WIDGET(body), connected);
    syncing = false;
  }
  void create() {
    window = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
    g_object_ref(window);
    gtk_window_set_title(GTK_WINDOW(window), "Helix Settings");
    gtk_window_set_default_size(GTK_WINDOW(window), 960, 720);
    gtk_widget_set_size_request(GTK_WIDGET(window), 320, 360);
    toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    split = ADW_NAVIGATION_SPLIT_VIEW(adw_navigation_split_view_new());
    auto* breakpoint = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 600sp"));
    GValue collapsed = G_VALUE_INIT; g_value_init(&collapsed, G_TYPE_BOOLEAN); g_value_set_boolean(&collapsed, true);
    adw_breakpoint_add_setter(breakpoint, G_OBJECT(split), "collapsed", &collapsed); g_value_unset(&collapsed);
    adw_application_window_add_breakpoint(window, breakpoint);
    auto* left = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(left), adw_header_bar_new());
    auto* leftBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    search = GTK_SEARCH_ENTRY(gtk_search_entry_new()); gtk_widget_set_margin_start(GTK_WIDGET(search), 12); gtk_widget_set_margin_end(GTK_WIDGET(search), 12); gtk_widget_set_margin_bottom(GTK_WIDGET(search), 12);
    gtk_box_append(GTK_BOX(leftBox), GTK_WIDGET(search));
    onSignal(search, "search-changed", [this] { query = gtk_editable_get_text(GTK_EDITABLE(search)); rendered.clear(); render(); });
    sidebar = GTK_LIST_BOX(gtk_list_box_new()); gtk_widget_add_css_class(GTK_WIDGET(sidebar), "navigation-sidebar");
    for (const auto& item : pages) {
      auto* itemRow = gtk_list_box_row_new();
      auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
      gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(item.icon));
      gtk_box_append(GTK_BOX(box), gtk_label_new(item.title));
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(itemRow), box);
      g_object_set_data(G_OBJECT(itemRow), "page-id", const_cast<char*>(item.id));
      gtk_list_box_append(sidebar, itemRow); pageRows[item.id] = GTK_LIST_BOX_ROW(itemRow);
    }
    g_signal_connect(sidebar, "row-activated", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
      auto* id = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "page-id")); static_cast<Settings*>(data)->select(id);
    }), this);
    gtk_box_append(GTK_BOX(leftBox), GTK_WIDGET(sidebar));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(left), leftBox);
    adw_navigation_split_view_set_sidebar(split, adw_navigation_page_new(left, "Settings"));
    auto* right = adw_toolbar_view_new(); adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(right), adw_header_bar_new());
    banner = ADW_BANNER(adw_banner_new("Connecting to Helix services…")); adw_banner_set_button_label(banner, "Retry");
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(right), GTK_WIDGET(banner));
    onSignal(banner, "button-clicked", [this] { scheduleRefresh(); });
    scroller = gtk_scrolled_window_new(); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    auto* clamp = adw_clamp_new(); adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 760);
    body = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 24));
    for (auto side : {"margin-start", "margin-end", "margin-top", "margin-bottom"}) g_object_set(body, side, 18, nullptr);
    adw_clamp_set_child(ADW_CLAMP(clamp), GTK_WIDGET(body)); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), clamp);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(right), scroller);
    contentPage = adw_navigation_page_new(right, "Appearance"); adw_navigation_split_view_set_content(split, contentPage);
    adw_toast_overlay_set_child(toasts, GTK_WIDGET(split)); adw_application_window_set_content(window, GTK_WIDGET(toasts));
    css = gtk_css_provider_new(); gtk_style_context_add_provider_for_display(gtk_widget_get_display(GTK_WIDGET(window)), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (bus) subscription = g_dbus_connection_signal_subscribe(bus, helix::busName, nullptr, nullptr, helix::objectPath,
      nullptr, G_DBUS_SIGNAL_FLAGS_NONE, [](GDBusConnection*, const gchar*, const gchar*, const gchar* interface,
        const gchar* signal, GVariant* parameters, gpointer data) {
        auto& self = *static_cast<Settings*>(data);
        if (std::string(signal) == "InputLevel") {
          auto values = helix::fromVariant(parameters);
          if (self.meterSource == values.at(0).get<std::uint32_t>() && self.meterBar) {
            gtk_level_bar_set_value(self.meterBar, values.at(1).get<double>());
            if (self.meterRow) adw_action_row_set_subtitle(self.meterRow, values.at(2).get<bool>() ? "Microphone is active. Audio is not saved." : "Microphone stream unavailable.");
          }
        } else if (std::string(signal) == "Prompt") self.prompt(std::string(interface) == helix::networkInterface ? "network" : "bluetooth", helix::fromVariant(parameters).at(0));
        else if (std::string(signal) == "Completed") {
          auto result = helix::fromVariant(parameters); if (!self.operations.erase(result.at(0).get<std::uint64_t>())) {
            if (self.earlyCompletions.size() >= 64) self.earlyCompletions.erase(self.earlyCompletions.begin());
            self.earlyCompletions.insert(result.at(0).get<std::uint64_t>());
          } if (!result.at(1).get<bool>() && result.at(2) != "Cancelled") self.toast(result.at(2).get<std::string>()); self.scheduleRefresh();
        } else self.scheduleRefresh();
      }, this, nullptr);
    if (bus) ownerSubscription = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus", "org.freedesktop.DBus",
      "NameOwnerChanged", "/org/freedesktop/DBus", helix::busName, G_DBUS_SIGNAL_FLAGS_NONE,
      [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer data) {
        auto& self = *static_cast<Settings*>(data);
        self.stopMeter(); self.operations.clear(); self.earlyCompletions.clear();
        self.connected = false; self.rendered.clear(); self.scheduleRefresh();
      }, this, nullptr);
    gtk_list_box_select_row(sidebar, pageRows.at(page));
    for (const auto& item : pages) if (item.desktop) gtk_widget_set_visible(GTK_WIDGET(pageRows.at(item.id)), false);
    poll = g_timeout_add_seconds(3, [](gpointer data) -> gboolean {
      auto& self = *static_cast<Settings*>(data);
      if (self.meterSource) self.client.callAsync("audio", "StartInputMeter", g_variant_new("(u)", self.meterSource), [&self](Json, std::string error) {
        if (!error.empty()) { self.stopMeter(); self.scheduleRefresh(); }
      });
      self.refresh(); return G_SOURCE_CONTINUE;
    }, this);
    refresh();
  }
};
}
int main(int argc, char** argv) {
  auto* app = adw_application_new("org.helix.Settings", G_APPLICATION_HANDLES_COMMAND_LINE);
  Settings settings(app);
  g_application_add_main_option(G_APPLICATION(app), "section", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, "Open a Desktop & Dock section", "ID");
  g_application_add_main_option(G_APPLICATION(app), "page", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, "Open a Settings page", "ID");
  g_application_add_main_option(G_APPLICATION(app), "wallpapers", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, "Open the library or wallhaven wallpaper browser", "SOURCE");
  g_application_add_main_option(G_APPLICATION(app), "palettes", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, "Open the palette browser", nullptr);
  g_application_add_main_option(G_APPLICATION(app), "inspect-layout", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, "Print layout state of the running Settings window", nullptr);
  g_application_add_main_option(G_APPLICATION(app), "save-preview", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, "Save a PNG of the running window", "FILE");
  g_signal_connect(app, "activate", G_CALLBACK(+[](GApplication*, gpointer data) {
    auto& self = *static_cast<Settings*>(data); if (!self.window) self.create(); gtk_window_present(GTK_WINDOW(self.window));
  }), &settings);
  g_signal_connect(app, "command-line", G_CALLBACK((+[](GApplication* application, GApplicationCommandLine* command, gpointer data) -> int {
    auto& self = *static_cast<Settings*>(data); const gchar *page = nullptr, *preview = nullptr, *wallpapers = nullptr, *section = nullptr;
    g_variant_dict_lookup(g_application_command_line_get_options_dict(command), "wallpapers", "&s", &wallpapers);
    if (wallpapers && std::string(wallpapers) != "library" && std::string(wallpapers) != "wallhaven") {
      g_application_command_line_printerr(command, "Wallpaper source must be library or wallhaven\n"); return 2;
    }
    g_variant_dict_lookup(g_application_command_line_get_options_dict(command), "save-preview", "&s", &preview);
    if (preview) {
      if (self.window && helix::apps::savePreview(GTK_WIDGET(self.window), preview)) return 0;
      g_application_command_line_printerr(command, "Open Settings before saving a preview\n"); return 1;
    }
    g_variant_dict_lookup(g_application_command_line_get_options_dict(command), "page", "&s", &page);
    g_variant_dict_lookup(g_application_command_line_get_options_dict(command), "section", "&s", &section);
    if (section && (settings::nativeGroupForSection(section).empty() || (page && std::string_view(page) != "desktop-dock"))) {
      g_application_command_line_printerr(command, "Section must be control-center, dock, launcher, osd or hot-corners on desktop-dock\n"); return 2;
    }
    gboolean inspect = false;
    g_variant_dict_lookup(g_application_command_line_get_options_dict(command), "inspect-layout", "b", &inspect);
    if (inspect && self.window) {
      Json layout = {{"width", gtk_widget_get_width(GTK_WIDGET(self.window))}, {"height", gtk_widget_get_height(GTK_WIDGET(self.window))},
        {"collapsed", bool(adw_navigation_split_view_get_collapsed(self.split))}, {"page", self.page},
        {"desktop-page", bool(gtk_widget_get_visible(GTK_WIDGET(self.pageRows.at("desktop-dock"))))}, {"connected", self.connected},
        {"content-width", gtk_widget_get_width(GTK_WIDGET(self.body))},
        {"section", self.section}, {"scroll-position", gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self.scroller)))},
        {"mutations-enabled", bool(gtk_widget_get_sensitive(GTK_WIDGET(self.body)))},
        {"palette-swatches", self.paletteSwatches}, {"wallpaper-preview", self.wallpaperPreview && gtk_picture_get_paintable(self.wallpaperPreview) != nullptr}};
      if (auto browser = self.wallpaperBrowser.lock(); browser && !browser->isClosed()) layout["wallpaper-browser"] = browser->inspect();
      if (auto browser = self.paletteBrowser.lock(); browser && !browser->isClosed()) layout["palette-browser"] = browser->inspect();
      if (self.layoutPreview) layout["layout-preview"] = helix::settings::LayoutPreview::description(self.layoutPreview);
      for (const auto& [mode, widget] : self.appearanceModes) if (gtk_toggle_button_get_active(widget)) layout["appearance-mode"] = mode;
      g_application_command_line_print(command, "%s\n", layout.dump().c_str());
      return 0;
    }
    if (page) self.requestedPage = page;
    self.requestedSection = section ? section : "";
    if (section) self.requestedPage = "desktop-dock";
    if (wallpapers) { self.requestedPage = "appearance"; self.requestedBrowser = wallpapers; }
    gboolean palettes = false; g_variant_dict_lookup(g_application_command_line_get_options_dict(command), "palettes", "b", &palettes);
    if (palettes) { self.requestedPage = "appearance"; self.requestedPalette = true; }
    g_application_activate(application);
    if (page || section || wallpapers || palettes) self.scheduleRefresh();
    return 0;
  })), &settings);
  const int result = g_application_run(G_APPLICATION(app), argc, argv);
  // Settings owns the window reference until its callbacks have been disconnected.
  g_object_unref(app);
  return result;
}
