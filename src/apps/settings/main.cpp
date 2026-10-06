#include "apps/common/inspection_guard.h"
#include "apps/common/appearance.h"
#include "apps/common/image_preview.h"
#include "apps/common/preview_export.h"
#include "apps/settings/wallpaper_browser.h"
#include "apps/settings/layout_preview.h"
#include "apps/settings/choice_preview.h"
#include "apps/settings/hub_navigation.h"
#include "apps/settings/palette_browser.h"
#include "apps/settings/accounts_page.h"
#include "apps/settings/agents_page.h"
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
#include <optional>
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
  {"online-accounts", "Online Accounts", "goa-panel-symbolic"},
  {"agents", "Agents", "system-run-symbolic"},
  {"privacy-security", "Privacy & Security", "security-high-symbolic", true},
  {"keyboard", "Keyboard", "input-keyboard-symbolic", true},
  {"displays", "Displays", "video-display-symbolic", true},
  {"power", "Power", "battery-symbolic", true},
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
void singleAction(AdwActionRow* row, GtkWidget* action) {
  adw_action_row_add_suffix(row, action);
  adw_action_row_set_activatable_widget(row, action);
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
  AdwNavigationView* hubNav = nullptr;
  AdwNavigationPage* overviewPage = nullptr;
  AdwToastOverlay* toasts = nullptr;
  AdwBanner* banner = nullptr;
  GtkListBox* sidebar = nullptr;
  GtkSearchEntry* search = nullptr;
  GtkBox* body = nullptr;
  GtkBox* overviewBody = nullptr;
  GtkBox* paneBody = nullptr;
  GtkWidget* scroller = nullptr;
  GtkWidget* overviewScroller = nullptr;
  GtkWidget* paneScroller = nullptr;
  AdwNavigationPage* hubPanePage = nullptr;
  GtkWidget* hubPaneHeader = nullptr;
  std::string hubPane;
  std::string hubOriginPane;
  std::map<std::string, GtkWidget*> hubNavRows;
  std::map<std::string, GtkWidget*> privacyChips;
  std::string privacyChip = "privacy";
  bool restorePrivacyFocus = false;
  bool suppressingHubPop = false;
  guint focusIdle = 0;
  std::vector<GtkWidget*> choicePreviews;
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
  std::optional<std::string> requestedQuery;
  std::string requestedSection;
  std::string section;
  std::map<std::string, GtkWidget*> sectionGroups;
  guint sectionTick = 0;
  unsigned sectionFrames = 0;
  std::string page = "appearance";
  std::string rendered;
  std::string query;
  unsigned paletteSwatches = 0;
  GtkPicture* wallpaperPreview = nullptr;
  GtkWidget* layoutPreview = nullptr;
  std::map<std::string, GtkToggleButton*> appearanceModes;
  std::weak_ptr<helix::settings::WallpaperBrowser> wallpaperBrowser;
  std::weak_ptr<helix::settings::PaletteBrowser> paletteBrowser;
  std::shared_ptr<helix::settings::AccountsPage> accountsPage;
  std::shared_ptr<helix::settings::AgentsPage> agentsPage;
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
    if (accountsPage) accountsPage->shutdown();
    if (agentsPage) agentsPage->shutdown();
    stopMeter();
    if (focusIdle) g_source_remove(focusIdle);
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
    refreshing = true; refreshFailed = false; pending = 7;
    client.callAsync("configuration", "GetSettings", nullptr, [this](Json value, std::string error) {
      if (error.empty()) catalog = std::move(value);
      finish(error);
    });
    for (const std::string domain : {"configuration", "appearance", "audio", "network", "bluetooth", "displays"}) {
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
    // Backend snapshots include explicit unavailable states.
    connected = !refreshFailed && snapshots.contains("configuration") && snapshots["configuration"].value("available", false) &&
      client.available();
    adw_banner_set_revealed(banner, !connected && page != "online-accounts");
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
    if (!requestedSection.empty() && page == settings::nativePageForTargetSection(requestedSection)) {
      section = std::move(requestedSection); requestedSection.clear(); navigateSection();
    }
    if (requestedQuery) {
      query = std::move(*requestedQuery); requestedQuery.reset();
      gtk_editable_set_text(GTK_EDITABLE(search), query.c_str());
      section.clear();
      if (sectionTick) { gtk_widget_remove_tick_callback(scroller, sectionTick); sectionTick = 0; }
      rendered.clear(); render();
      gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller)), 0);
      adw_navigation_split_view_set_show_content(split, true);
      gtk_widget_grab_focus(GTK_WIDGET(search));
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
    auto* preview = group("Appearance");
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
    auto* actions = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(actions), GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(actions), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(actions), 2);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(actions), true);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(actions), 8);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(actions), 8);
    auto* paletteAction = button("Palette Library", [this] { choosePalette(); });
    gtk_widget_set_hexpand(paletteAction, true);
    gtk_flow_box_append(GTK_FLOW_BOX(actions), paletteAction);
    if (appearance.value("helix-desktop", false)) {
      auto* wallpaperAction = button("Wallpaper Library", [this] { chooseWallpaper(); });
      gtk_widget_set_hexpand(wallpaperAction, true);
      gtk_flow_box_append(GTK_FLOW_BOX(actions), wallpaperAction);
    }
    adw_preferences_group_add(preview, actions);
  }
  void navigateSection() {
    if (helix::settings::isHubPage(page) && query.empty()) {
      const auto paneId = std::string(helix::settings::paneIdForSection(section));
      if (helix::settings::usesChipHub(page)) {
        if (helix::settings::findPane(page, paneId)) {
          privacyChip = paneId;
          hubPane.clear();
          popHubToOverview();
          rendered.clear();
          render();
        }
        return;
      }
      if (helix::settings::findPane(page, paneId)) {
        const auto scrollId = section;
        openHubPane(paneId);
        section = scrollId;
        scheduleSectionScroll();
        return;
      }
    }
    scheduleSectionScroll();
  }
  void scheduleSectionScroll() {
    if (!scroller) return;
    if (sectionTick) gtk_widget_remove_tick_callback(scroller, sectionTick);
    sectionFrames = 0;
    sectionTick = gtk_widget_add_tick_callback(scroller, +[](GtkWidget*, GdkFrameClock*, gpointer data) -> gboolean {
      auto& self = *static_cast<Settings*>(data);
      // Tick callbacks precede allocation; let rebuilt groups settle before scrolling.
      if (++self.sectionFrames < 3) return G_SOURCE_CONTINUE;
      const auto target = self.sectionGroups.find(std::string(settings::nativeGroupForSection(self.section)));
      if (target == self.sectionGroups.end()) { self.sectionTick = 0; return G_SOURCE_REMOVE; }
      graphene_rect_t bounds;
      if (!gtk_widget_compute_bounds(target->second, GTK_WIDGET(self.body), &bounds) || bounds.size.height <= 0) return G_SOURCE_CONTINUE;
      auto* adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self.scroller));
      gtk_widget_child_focus(target->second, GTK_DIR_TAB_FORWARD);
      gtk_adjustment_set_value(adjustment, std::max(0.0, double(bounds.origin.y) - 12));
      self.sectionTick = 0;
      return G_SOURCE_REMOVE;
    }, this, nullptr);
  }
  void popHubToOverview() {
    if (!hubNav) return;
    suppressingHubPop = true;
    while (adw_navigation_view_get_navigation_stack(hubNav) &&
           g_list_model_get_n_items(adw_navigation_view_get_navigation_stack(hubNav)) > 1)
      adw_navigation_view_pop(hubNav);
    suppressingHubPop = false;
    hubPanePage = nullptr;
    hubPaneHeader = nullptr;
    paneBody = nullptr;
    paneScroller = nullptr;
    body = overviewBody;
    scroller = overviewScroller;
  }
  void openHubPane(const std::string& paneId) {
    const auto* spec = helix::settings::findPane(page, paneId);
    if (!spec || helix::settings::usesChipHub(page) || !query.empty()) return;
    hubPane = paneId;
    hubOriginPane = paneId;
    section = paneId;
    if (!rendered.empty()) {
      try {
        auto shape = Json::parse(rendered);
        shape["hub-pane"] = hubPane;
        rendered = shape.dump();
      } catch (...) {}
    }
    if (hubPanePage && adw_navigation_view_get_visible_page(hubNav) == hubPanePage) {
      body = paneBody;
      scroller = paneScroller;
      buildHubPane(*spec);
      return;
    }
    popHubToOverview();
    auto* toolbar = adw_toolbar_view_new();
    hubPaneHeader = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), hubPaneHeader);
    auto [scroll, box] = makeScrollBody();
    paneScroller = scroll;
    paneBody = box;
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);
    hubPanePage = adw_navigation_page_new(toolbar, spec->title);
    body = paneBody;
    scroller = paneScroller;
    buildHubPane(*spec);
    adw_navigation_view_push(hubNav, hubPanePage);
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller)), 0);
    scheduleHubBackFocus();
  }
  void scheduleHubBackFocus() {
    if (focusIdle) g_source_remove(focusIdle);
    focusIdle = g_idle_add([](gpointer data) -> gboolean {
      auto& self = *static_cast<Settings*>(data);
      self.focusIdle = 0;
      if (self.hubPaneHeader) gtk_widget_child_focus(self.hubPaneHeader, GTK_DIR_TAB_FORWARD);
      return G_SOURCE_REMOVE;
    }, this);
  }
  void focusHubNavRow(const std::string& paneId) {
    if (paneId.empty()) return;
    const auto found = hubNavRows.find(paneId);
    if (found == hubNavRows.end() || !found->second) return;
    gtk_widget_grab_focus(found->second);
    graphene_rect_t bounds;
    if (overviewBody && gtk_widget_compute_bounds(found->second, GTK_WIDGET(overviewBody), &bounds)) {
      auto* adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(overviewScroller));
      gtk_adjustment_set_value(adjustment, std::max(0.0, double(bounds.origin.y) - 12));
    }
  }
  void schedulePrivacyChipFocus() {
    if (focusIdle) g_source_remove(focusIdle);
    focusIdle = g_idle_add([](gpointer data) -> gboolean {
      auto& self = *static_cast<Settings*>(data);
      self.focusIdle = 0;
      const auto found = self.privacyChips.find(self.privacyChip);
      if (found != self.privacyChips.end() && found->second) gtk_widget_grab_focus(found->second);
      return G_SOURCE_REMOVE;
    }, this);
  }
  void showEmptyState() {
    auto* status = adw_status_page_new();
    if (!connected && page != "online-accounts") {
      adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), "network-offline-symbolic");
      adw_status_page_set_title(ADW_STATUS_PAGE(status), "Connect to Helix Services");
      adw_status_page_set_description(ADW_STATUS_PAGE(status), "Settings will appear when the service becomes available.");
    } else if (!query.empty()) {
      adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), "edit-find-symbolic");
      adw_status_page_set_title(ADW_STATUS_PAGE(status), "No Matching Settings");
      adw_status_page_set_description(ADW_STATUS_PAGE(status), "Try a different search.");
      auto* clear = button("Clear Search", [this] {
        query.clear();
        gtk_editable_set_text(GTK_EDITABLE(search), "");
        rendered.clear();
        render();
      });
      gtk_widget_add_css_class(clear, "pill");
      adw_status_page_set_child(ADW_STATUS_PAGE(status), clear);
    } else if (helix::settings::usesChipHub(page)) {
      adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), "security-high-symbolic");
      adw_status_page_set_title(ADW_STATUS_PAGE(status), "Nothing in This Facet");
      adw_status_page_set_description(ADW_STATUS_PAGE(status), "Choose another facet above.");
    } else {
      adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), "preferences-system-symbolic");
      adw_status_page_set_title(ADW_STATUS_PAGE(status), "No Settings Here");
      adw_status_page_set_description(ADW_STATUS_PAGE(status), "This page has no settings to show right now.");
    }
    gtk_box_append(body, status);
  }
  std::pair<GtkWidget*, GtkBox*> makeScrollBody() {
    auto* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    auto* clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 760);
    auto* box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 24));
    for (auto side : {"margin-start", "margin-end", "margin-top", "margin-bottom"}) g_object_set(box, side, 18, nullptr);
    adw_clamp_set_child(ADW_CLAMP(clamp), GTK_WIDGET(box));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
    return {scroll, box};
  }
  void addChoicePreview(AdwPreferencesGroup* host, helix::settings::ChoicePreview::Kind kind) {
    auto* preview = helix::settings::ChoicePreview::create(kind);
    choicePreviews.push_back(preview);
    adw_preferences_group_add(host, preview);
    helix::settings::ChoicePreview::update(preview, catalog);
  }
  void buildHubPane(const helix::settings::HubPaneSpec& pane) {
    sectionGroups.clear();
    choicePreviews.clear();
    layoutPreview = nullptr;
    while (auto* child = gtk_widget_get_first_child(GTK_WIDGET(body))) gtk_box_remove(body, child);
    if (page == "desktop-dock" && std::string_view(pane.id) == "dock") {
      auto* preview = group("Shape Preview", false);
      addChoicePreview(preview, helix::settings::ChoicePreview::Kind::DockShape);
    }
    if (page == "appearance" && std::string_view(pane.id) == "wallpaper") {
      auto* preview = group("Fill Preview", false);
      addChoicePreview(preview, helix::settings::ChoicePreview::Kind::WallpaperFill);
    }
    std::map<std::string, AdwPreferencesGroup*> groups;
    const int distinct = helix::settings::matchingGroupCount(catalog, page, pane);
    for (const auto& setting : catalog) {
      if (setting.value("page", "") != page || !helix::settings::settingMatchesPane(setting, pane)) continue;
      if (page == "appearance" && !appearanceModes.empty() && setting.value("id", "") == "theme.mode") continue;
      const auto name = setting.at("group").get<std::string>();
      if (!groups.contains(name)) {
        const auto display = helix::settings::paneGroupTitle(name, pane, distinct);
        auto* fields = group(name, false);
        if (!display.empty()) adw_preferences_group_set_title(fields, display.c_str());
        groups[name] = fields;
      }
      addSetting(groups[name], setting);
    }
    if (page == "appearance" && std::string_view(pane.id) == "wallpaper" &&
        snapshots.contains("configuration") && snapshots["configuration"].value("helix-desktop", false)) {
      AdwPreferencesGroup* wallpapers = nullptr;
      auto wallpaperGroup = [&] {
        if (!wallpapers) wallpapers = group("Wallpaper Sources");
        return wallpapers;
      };
      if (matches("Appearance Wallpaper Files Choose Local File Image Video")) {
        auto* file = row(wallpaperGroup(), "Choose a Local File", "Use an image or video supported by Helix Shell.");
        singleAction(file, button("Choose", [this] { chooseWallpaperFile(); }));
      }
      if (matches("Appearance Wallpaper Wallhaven Browse Online")) {
        auto* wallhaven = row(wallpaperGroup(), "Wallhaven", "Search online wallpapers using your existing preferences and credentials.");
        singleAction(wallhaven, button("Browse", [this] { chooseWallpaper(true); }));
      }
    }
    if (page == "appearance" && std::string_view(pane.id) == "colors") {
      AdwPreferencesGroup* palettes = nullptr;
      auto paletteGroup = [&] {
        if (!palettes) palettes = group("Palette Management");
        return palettes;
      };
      if (matches("Appearance Palette Library Browse Colors")) {
        auto* palette = row(paletteGroup(), "Palette Library", "Compare built-in, community and custom palettes before applying one.");
        singleAction(palette, button("Browse", [this] { choosePalette(); }));
      }
      if (matches("Appearance Accent Color Choose")) {
        auto* accent = row(paletteGroup(), "Accent Color", "Creates a custom palette from the current appearance.");
        singleAction(accent, button("Choose", [this] { chooseAccent(); }));
      }
      if (matches("Appearance Community Palettes Refresh Catalog")) {
        auto* catalogActions = row(paletteGroup(), "Community Palettes", "Refresh available palettes from the community catalog.");
        singleAction(catalogActions, button("Refresh", [this] {
          client.callAsync("appearance", "RefreshPalettes", nullptr, [this](Json, std::string error) {
            toast(error.empty() ? "Refreshing the palette catalog" : "Palette catalog could not be refreshed"); scheduleRefresh();
          });
        }));
      }
      if (matches("Appearance Custom Palettes Save Current Palette")) {
        auto* save = row(paletteGroup(), "Save Current Palette", "Keep a copy of the current colors in your custom palette library.");
        singleAction(save, button("Save", [this] {
          client.callAsync("appearance", "SavePalette", g_variant_new("(s)", "current"), [this](Json, std::string error) {
            toast(error.empty() ? "Palette saved" : "Palette could not be saved"); scheduleRefresh();
          });
        }));
      }
    }
    if (!gtk_widget_get_first_child(GTK_WIDGET(body))) {
      auto* status = adw_status_page_new();
      adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), "preferences-system-symbolic");
      adw_status_page_set_title(ADW_STATUS_PAGE(status), "Nothing Here");
      adw_status_page_set_description(ADW_STATUS_PAGE(status), "This section has no settings right now.");
      gtk_box_append(body, status);
    }
    for (const auto& setting : catalog) {
      const auto id = setting.at("id").get<std::string>();
      if (setters.contains(id)) setters.at(id)(setting.at("value"));
    }
    for (auto* preview : choicePreviews) helix::settings::ChoicePreview::update(preview, catalog);
  }
  void openApplication(const char* command) {
    GError* error = nullptr;
    auto* info = g_app_info_create_from_commandline(command, nullptr, G_APP_INFO_CREATE_SUPPORTS_STARTUP_NOTIFICATION, &error);
    if (info) {
      auto* context = gdk_display_get_app_launch_context(gtk_widget_get_display(GTK_WIDGET(window)));
      if (!g_app_info_launch(info, nullptr, G_APP_LAUNCH_CONTEXT(context), &error)) toast("The application could not be opened. Check that it is installed.");
      g_object_unref(context); g_object_unref(info);
    } else toast("The application could not be opened.");
    if (error) g_error_free(error);
  }
  void select(const std::string& id) {
    auto it = pageRows.find(id);
    if (it == pageRows.end()) { toast("Unknown Settings page"); return; }
    if (!gtk_widget_get_visible(GTK_WIDGET(it->second))) { toast("This page requires a Helix desktop session"); return; }
    if (id != "sound") stopMeter();
    section.clear();
    hubPane.clear();
    hubOriginPane.clear();
    privacyChip = "privacy";
    if (sectionTick) { gtk_widget_remove_tick_callback(scroller, sectionTick); sectionTick = 0; }
    popHubToOverview();
    query.clear(); gtk_editable_set_text(GTK_EDITABLE(search), "");
    page = id;
    adw_banner_set_revealed(banner, !connected && page != "online-accounts");
    gtk_list_box_select_row(sidebar, it->second);
    for (const auto& item : pages) if (id == item.id) {
      adw_navigation_page_set_title(contentPage, item.title);
      if (overviewPage) adw_navigation_page_set_title(overviewPage, item.title);
    }
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
  AdwPreferencesGroup* group(const std::string& name, bool titled = true) {
    auto* widget = adw_preferences_group_new();
    if (titled) adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(widget), name.c_str());
    gtk_box_append(body, widget);
    sectionGroups[name] = widget;
    if (name.starts_with("Bar · ")) sectionGroups.try_emplace("Bar", widget);
    if (name.starts_with("Dock")) sectionGroups.try_emplace("Dock", widget);
    if (name.starts_with("Screenshot") || name == "Screen Record") sectionGroups.try_emplace("Capture", widget);
    if (name.starts_with("Automation · ")) sectionGroups.try_emplace("Automation", widget);
    if (name.starts_with("Panels · ")) sectionGroups.try_emplace("Panels", widget);
    if (name.starts_with("Niri ")) sectionGroups.try_emplace("Niri", widget);
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
  bool matchesSetting(const Json& setting) const {
    if (query.empty()) return true;
    return matches(setting.value("title", "") + " " + setting.value("group", "") + " " +
                   setting.value("page", "") + " " + setting.value("id", "") + " " +
                   setting.value("description", ""));
  }
  void addSetting(AdwPreferencesGroup* group, const Json& setting) {
    const auto id = setting.at("id").get<std::string>();
    const auto type = setting.at("type").get<std::string>();
    const auto name = setting.at("title").get<std::string>();
    GtkWidget* widget;
    if (type == "key-chords") { keyShortcuts(group, setting); return; }
    if (type == "notification-rules") { ruleList(group, setting); return; }
    if (type == "shortcut-list") { shortcutList(group, setting); return; }
    if (type == "tab-list") { tabVisibility(group, setting); return; }
    if (type == "application-list" || type == "widget-list" || type == "display-list" || type == "template-list" || type == "transition-list") { applicationList(group, setting); return; }
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
      if (type == "color") {
        adw_action_row_add_suffix(ADW_ACTION_ROW(widget), button("Custom", [this, id] {
          auto* dialog = gtk_color_dialog_new(); gtk_color_dialog_set_title(dialog, "Icon Tint Color");
          gtk_color_dialog_set_with_alpha(dialog, false);
          gtk_color_dialog_choose_rgba(dialog, GTK_WINDOW(window), nullptr, nullptr,
            [](GObject* source, GAsyncResult* result, gpointer data) {
              auto* payload = static_cast<std::pair<Settings*, std::string>*>(data);
              auto& self = *payload->first; const auto id = payload->second; delete payload;
              GError* error = nullptr;
              auto* color = gtk_color_dialog_choose_rgba_finish(GTK_COLOR_DIALOG(source), result, &error);
              if (color) {
                auto* text = g_strdup_printf("#%02x%02x%02x", unsigned(color->red * 255), unsigned(color->green * 255), unsigned(color->blue * 255));
                self.update(id, std::string(text));
                g_free(text); gdk_rgba_free(color);
              }
              if (error) g_error_free(error);
            }, new std::pair<Settings*, std::string>{this, id});
          g_object_unref(dialog);
        }));
      }
    } else if (type == "number" || type == "integer") {
      widget = adw_spin_row_new_with_range(setting.at("minimum"), setting.at("maximum"), setting.value("step", type == "integer" ? 1.0 : 0.05));
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
  void keyShortcuts(AdwPreferencesGroup* fields, const Json& setting) {
    const auto id = setting.at("id").get<std::string>();
    const auto values = setting.at("value");
    std::string shortcutText;
    for (const auto& value : values) { if (!shortcutText.empty()) shortcutText += "; "; shortcutText += value.get<std::string>(); }
    auto* item = row(fields, setting.at("title"), shortcutText);
    singleAction(item, button("Edit", [this, id, values, shortcutText] {
      auto* dialog = adw_alert_dialog_new("Shell Navigation Shortcuts", "Enter key names such as Ctrl+c or Shift+Tab. Separate alternatives with semicolons. Super shortcuts belong to compositor bindings.");
      auto* entry = adw_entry_row_new(); title(ADW_PREFERENCES_ROW(entry), "Shortcuts");
      gtk_editable_set_text(GTK_EDITABLE(entry), shortcutText.c_str());
      auto* group = adw_preferences_group_new(); adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), entry);
      adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), group);
      adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "save", "Save", nullptr);
      adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "save");
      adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
      struct Edit { Settings* settings; std::string id; Json baseline; GtkWidget* entry; };
      adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window), nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<Edit> edit(static_cast<Edit*>(data));
          if (std::string(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result)) != "save") return;
          Json candidate = Json::array(); auto** parts = g_strsplit(gtk_editable_get_text(GTK_EDITABLE(edit->entry)), ";", -1);
          for (auto** part = parts; *part; ++part) { auto* trimmed = g_strstrip(*part); if (*trimmed) candidate.push_back(trimmed); }
          g_strfreev(parts);
          if (candidate.empty() || candidate.size() > 16) { edit->settings->toast("Enter between one and sixteen shortcuts."); return; }
          edit->settings->client.callAsync("configuration", "UpdateOrderedList", g_variant_new("(s@av@av)", edit->id.c_str(), helix::toVariant(edit->baseline), helix::toVariant(candidate)),
            [self = edit->settings](Json, std::string error) {
              if (!error.empty()) self->toast("Shortcuts could not be saved. Use valid, distinct keys with Ctrl, Alt or Shift, then refresh and retry.");
              self->scheduleRefresh();
            });
        }, new Edit{this, id, values, entry});
    }));
    if (setting.value("overridden", false)) adw_action_row_add_suffix(item, button("Reset", [this, id] {
      client.callAsync("configuration", "Reset", g_variant_new("(s)", id.c_str()), [this](Json, std::string error) {
        if (!error.empty()) toast("The configured shortcuts could not be restored.");
        scheduleRefresh();
      });
    }));
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
  void templatePicker(const Json& setting) {
    const auto id = setting.at("id").get<std::string>(); const auto values = setting.at("value");
    auto* dialog = adw_alert_dialog_new("Add Template", "Choose a template. Saving applies it the next time Helix writes theme files.");
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    auto* search = gtk_search_entry_new(); gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search), "Search templates");
    gtk_box_append(GTK_BOX(box), search);
    auto* fields = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    auto* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 180);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), true);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(fields)); gtk_box_append(GTK_BOX(box), scroll);
    std::vector<std::pair<GtkWidget*, std::string>> rows;
    const auto labels = setting.value("option-labels", Json::array());
    bool any = false;
    for (std::size_t i = 0; i < setting.at("options").size(); ++i) {
      const auto option = setting.at("options").at(i);
      if (std::ranges::find(values, option) != values.end()) continue;
      const auto name = option.get<std::string>();
      const auto label = i < labels.size() ? labels.at(i).get<std::string>() : name;
      auto* item = row(fields, label, name); rows.emplace_back(GTK_WIDGET(item), label + " " + name); any = true;
      adw_action_row_add_suffix(item, button("Add", [this, dialog, id, values, name] {
        auto next = values; next.push_back(name); saveApplications(id, values, next); adw_dialog_close(ADW_DIALOG(dialog));
      }));
    }
    if (!any) row(fields, "No additional templates", "Every available template is already selected, or none are installed.");
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
    const bool templates = setting.at("type") == "template-list" || setting.at("type") == "transition-list";
    const bool transitions = setting.at("type") == "transition-list";
    const std::string fallback = values.empty() ? (displays ? "All displays" : widgets ? "No widgets in this section" : transitions ? "No transitions selected" : templates ? "No templates selected" : "No pinned applications") : "Items appear in this order.";
    auto* header = row(fields, setting.at("title"), templates && setting.contains("description") ? setting.at("description").get<std::string>() : fallback);
    adw_action_row_add_suffix(header, button("Add", [this, id, values, widgets, displays, templates, setting] { if (displays) displayPicker(id, values); else if (widgets) widgetPicker(setting); else if (templates) templatePicker(setting); else applicationPicker(id, values); }));
    if (setting.value("overridden", false)) adw_action_row_add_suffix(header, button("Reset", [this, id] {
      client.callAsync("configuration", "Reset", g_variant_new("(s)", id.c_str()), [this](Json, std::string error) { if (!error.empty()) toast("Favorites could not be reset"); scheduleRefresh(); });
    }));
    std::map<std::string, std::string> names;
    if (templates && setting.contains("option-labels")) {
      const auto& options = setting.at("options"); const auto& labels = setting.at("option-labels");
      for (std::size_t i = 0; i < options.size() && i < labels.size(); ++i) names[options.at(i).get<std::string>()] = labels.at(i).get<std::string>();
    }
    auto* apps = widgets || displays || templates ? nullptr : g_app_info_get_all();
    for (auto* node = apps; node; node = node->next) {
      auto* app = G_APP_INFO(node->data); const auto* raw = g_app_info_get_id(app); if (!raw) continue;
      std::string key(raw); if (key.ends_with(".desktop")) key.resize(key.size() - 8);
      names[key] = g_app_info_get_display_name(app);
    } g_list_free_full(apps, g_object_unref);
    for (std::size_t i = 0; i < values.size(); ++i) {
      const auto appId = values.at(i).get<std::string>();
      auto* item = row(fields, names.contains(appId) ? names.at(appId) : appId, widgets ? (std::ranges::find(setting.at("options"), Json(appId)) == setting.at("options").end() ? "Widget is unavailable; its position is retained." : "Configured widget instance") : transitions ? "Wallpaper transition style" : templates ? (std::ranges::find(setting.at("options"), Json(appId)) == setting.at("options").end() ? "Template is unavailable; its selection is retained." : "Writes theme files for this template") : displays ? "Display selector" : names.contains(appId) ? appId : "Application is unavailable; its saved position is retained.");
      adw_action_row_set_title_lines(item, 1);
      gtk_widget_set_tooltip_text(GTK_WIDGET(item), appId.c_str());
      auto* menu = gtk_menu_button_new(); gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "view-more-symbolic");
      gtk_widget_set_tooltip_text(menu, transitions ? "Edit transition" : templates ? "Edit template" : displays ? "Edit display" : widgets ? "Edit widget" : "Edit favorite"); gtk_widget_set_valign(menu, GTK_ALIGN_CENTER);
      gtk_accessible_update_property(GTK_ACCESSIBLE(menu), GTK_ACCESSIBLE_PROPERTY_LABEL, transitions ? "Edit transition" : templates ? "Edit template" : displays ? "Edit display" : widgets ? "Edit widget" : "Edit favorite", -1);
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
      control("list-remove-symbolic", displays ? "Remove display" : widgets ? "Remove widget" : templates ? "Remove template" : "Remove favorite", 0);
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

  void displayDevices() {
    if (page != "displays" || !query.empty()) return;
    if (!snapshots.contains("displays")) return;
    const auto& state = snapshots.at("displays");
    if (!state.value("available", false)) {
      auto* status = adw_status_page_new();
      adw_status_page_set_title(ADW_STATUS_PAGE(status), "Displays Unavailable");
      adw_status_page_set_description(ADW_STATUS_PAGE(status), state.value("reason", "Waiting for the display backend.").c_str());
      gtk_box_append(body, status);
    } else {
      auto* overview = group("Connected Displays");
      adw_preferences_group_set_description(overview, "Current display state from helix-niri. Arrangement and mode changes use the existing display controls.");
      const auto outputs = state.value("outputs", Json::array());
      if (outputs.empty()) row(overview, "No displays reported");
      else row(overview, "Detected displays", std::to_string(outputs.size()));
      if (std::ranges::any_of(outputs, [](const Json& item) { return item.value("enabled", false); })) {
        auto* diagram = gtk_drawing_area_new();
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(diagram), 180);
        gtk_accessible_update_property(GTK_ACCESSIBLE(diagram), GTK_ACCESSIBLE_PROPERTY_LABEL,
          "Display arrangement preview. Connection, resolution and position are listed below.", -1);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(diagram), +[](GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer data) {
          const auto& outputs = *static_cast<Json*>(data);
          double left = 0, top = 0, right = 0, bottom = 0; bool first = true;
          for (const auto& output : outputs) if (output.value("enabled", false)) {
            const double x = output.value("x", 0), y = output.value("y", 0);
            const double w = output.value("logical-width", 0), h = output.value("logical-height", 0);
            if (w <= 0 || h <= 0) continue;
            if (first) { left = x; top = y; right = x + w; bottom = y + h; first = false; }
            else { left = std::min(left, x); top = std::min(top, y); right = std::max(right, x + w); bottom = std::max(bottom, y + h); }
          }
          if (first) return;
          const double factor = std::min((width - 24.0) / std::max(1.0, right - left), (height - 24.0) / std::max(1.0, bottom - top));
          const double offsetX = (width - (right - left) * factor) / 2, offsetY = (height - (bottom - top) * factor) / 2;
          GdkRGBA foreground; gtk_widget_get_color(GTK_WIDGET(area), &foreground);
          for (const auto& output : outputs) if (output.value("enabled", false)) {
            const double w = output.value("logical-width", 0) * factor, h = output.value("logical-height", 0) * factor;
            if (w <= 0 || h <= 0) continue;
            const double x = offsetX + (output.value("x", 0) - left) * factor, y = offsetY + (output.value("y", 0) - top) * factor;
            cairo_rectangle(cr, x + 2, y + 2, std::max(1.0, w - 4), std::max(1.0, h - 4));
            cairo_set_source_rgba(cr, foreground.red, foreground.green, foreground.blue, .12); cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, foreground.red, foreground.green, foreground.blue, foreground.alpha); cairo_set_line_width(cr, 1); cairo_stroke(cr);
            const auto name = output.value("name", "Display");
            auto* text = gtk_widget_create_pango_layout(GTK_WIDGET(area), name.c_str());
            pango_layout_set_width(text, int(std::max(1.0, w - 16) * PANGO_SCALE)); pango_layout_set_ellipsize(text, PANGO_ELLIPSIZE_MIDDLE);
            pango_layout_set_alignment(text, PANGO_ALIGN_CENTER);
            int textHeight = 0; pango_layout_get_pixel_size(text, nullptr, &textHeight);
            cairo_save(cr); cairo_rectangle(cr, x, y, w, h); cairo_clip(cr);
            cairo_move_to(cr, x + 8, y + (h - textHeight) / 2); pango_cairo_show_layout(cr, text); cairo_restore(cr); g_object_unref(text);
          }
        }, new Json(outputs), +[](gpointer data) { delete static_cast<Json*>(data); });
        adw_preferences_group_add(overview, diagram);
      }

      for (const auto& output : outputs) {
        const auto name = output.value("name", "");
        const auto label = output.value("label", name);
        auto* fields = group(label);
        row(fields, "Connection", name + (output.value("enabled", false) ? " · Enabled" : " · Disabled"));
        if (!output.value("enabled", false)) continue;
        const auto resolution = std::to_string(output.value("width", 0)) + " × " + std::to_string(output.value("height", 0));
        row(fields, "Resolution", resolution);
        auto* refresh = g_strdup_printf("%.2f Hz", output.value("refresh-millihertz", 0u) / 1000.0);
        row(fields, "Refresh rate", refresh); g_free(refresh);
        auto* scale = g_strdup_printf("%.0f%%", output.value("scale", 1.0) * 100);
        row(fields, "Scale", scale); g_free(scale);
        row(fields, "Position", std::to_string(output.value("x", 0)) + ", " + std::to_string(output.value("y", 0)));
        row(fields, "Orientation", output.value("transform", "Normal"));
        if (output.value("vrr-supported", false)) row(fields, "Variable refresh rate", output.value("vrr-enabled", false) ? "Enabled" : "Disabled");
      }
    }
    if (snapshots.contains("configuration") && !snapshots.at("configuration").value("desktop-capabilities", Json::object()).value("gamma-control", false)) {
      auto* unavailable = row(group("Night Light"), "Night Light unavailable", "This compositor connection does not report gamma control. Display colors cannot be changed here.");
      gtk_widget_set_sensitive(GTK_WIDGET(unavailable), false);
    }
    auto* controls = row(group("Display Configuration"), "Resolution & Arrangement", "Open the current Helix display controls. Native editing is still being migrated.");
    singleAction(controls, button("Open", [this] {
      GError* error = nullptr;
      if (!g_spawn_command_line_async("noctalia msg panel-open control-center monitor", &error)) {
        toast("Display controls could not be opened"); if (error) g_error_free(error);
      }
    }));
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
    for (auto& setting : shape["settings"]) if (setting.value("type", "") != "application-list" && setting.value("type", "") != "widget-list" && setting.value("type", "") != "display-list" && setting.value("type", "") != "notification-rules" && setting.value("type", "") != "tab-list" && setting.value("type", "") != "shortcut-list" && setting.value("type", "") != "key-chords" && setting.value("type", "") != "template-list" && setting.value("type", "") != "transition-list") setting.erase("value");
    const std::string domain = page == "sound" ? "audio" : page;
    if (snapshots.contains(domain)) {
      shape["devices"] = snapshots.at(domain);
      if (domain == "audio") for (const std::string kind : {"sinks", "sources"})
        if (shape["devices"].contains(kind)) for (auto& device : shape["devices"][kind]) { device.erase("volume"); device.erase("muted"); }
      if (domain == "network" || domain == "bluetooth") shape["devices"].erase("enabled");
    }
    if (page == "about" && snapshots.contains("configuration")) shape["system"] = snapshots.at("configuration");
    // GOA is independent: Helix service changes must not cancel an account dialog.
    if (page == "online-accounts" && query.empty()) shape = {{"page", page}, {"query", query}};
    else {
      shape["hub-pane"] = hubPane;
      shape["privacy-chip"] = privacyChip;
    }
    const auto fingerprint = shape.dump();
    syncing = true;
    if (fingerprint != rendered) {
      if (accountsPage) { accountsPage->shutdown(); accountsPage.reset(); }
      if (agentsPage) { agentsPage->shutdown(); agentsPage.reset(); }
      const bool wantPane = !hubPane.empty() && helix::settings::isHubPage(page) &&
          !helix::settings::usesChipHub(page) && query.empty();
      if (!wantPane) popHubToOverview();
      body = overviewBody;
      scroller = overviewScroller;
      rendered = fingerprint; setters.clear(); meterBar = nullptr; meterRow = nullptr;
      paletteSwatches = 0; wallpaperPreview = nullptr;
      layoutPreview = nullptr;
      appearanceModes.clear();
      sectionGroups.clear();
      choicePreviews.clear();
      while (auto* child = gtk_widget_get_first_child(GTK_WIDGET(body))) gtk_box_remove(body, child);
      const bool hubOverview = helix::settings::isHubPage(page) && query.empty();
      const auto* chipPane = helix::settings::usesChipHub(page) && query.empty()
          ? helix::settings::findPane(page, privacyChip) : nullptr;
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
      if (page == "online-accounts" && query.empty()) {
        accountsPage = helix::settings::AccountsPage::create(GTK_WINDOW(window), [this](const std::string& message) { toast(message); });
        gtk_box_append(body, accountsPage->widget());
      }
      if (page == "agents" && query.empty()) {
        agentsPage = helix::settings::AgentsPage::create([this](const std::string& message) { toast(message); });
        gtk_box_append(body, agentsPage->widget());
      }
      displayDevices();
      devices();
      if (page == "sound" && query.empty()) {
        auto* preview = group("Volume Preview", false);
        addChoicePreview(preview, helix::settings::ChoicePreview::Kind::VolumeOverdrive);
      }
      if (page == "appearance" && query.empty()) appearancePreview();
      if (query.empty() && (page == "desktop-dock" || page == "notifications") &&
          snapshots.contains("configuration") && snapshots.at("configuration").value("helix-desktop", false)) {
        auto* preview = group(page == "notifications" ? "Notification Preview" : "Layout", false);
        layoutPreview = helix::settings::LayoutPreview::create(page == "notifications");
        adw_preferences_group_add(preview, layoutPreview);
      }
      if (page == "privacy-security" && query.empty()) {
        privacyChips.clear();
        auto* chipsHost = group("Facets", false);
        auto* chips = gtk_flow_box_new();
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(chips), GTK_SELECTION_NONE);
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(chips), 1);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(chips), 4);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(chips), 6);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(chips), 6);
        GtkToggleButton* chipGroup = nullptr;
        for (const auto& pane : helix::settings::privacyPanes()) {
          if (!helix::settings::paneHasSettings(catalog, page, pane)) continue;
          auto* chip = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(pane.title));
          if (!chipGroup) chipGroup = chip;
          else gtk_toggle_button_set_group(chip, chipGroup);
          gtk_widget_add_css_class(GTK_WIDGET(chip), "pill");
          gtk_widget_set_tooltip_text(GTK_WIDGET(chip), pane.subtitle);
          gtk_toggle_button_set_active(chip, privacyChip == pane.id);
          const std::string id = pane.id;
          privacyChips[id] = GTK_WIDGET(chip);
          onSignal(chip, "toggled", [this, id, chip] {
            if (!gtk_toggle_button_get_active(chip) || privacyChip == id) return;
            privacyChip = id;
            section = id;
            restorePrivacyFocus = true;
            rendered.clear();
            render();
          });
          gtk_flow_box_append(GTK_FLOW_BOX(chips), GTK_WIDGET(chip));
        }
        adw_preferences_group_add(chipsHost, chips);
        if (restorePrivacyFocus) {
          restorePrivacyFocus = false;
          schedulePrivacyChipFocus();
        }
      }
      if (page == "system" && query.empty()) {
        auto* destinations = group("Apps & Destinations");
        auto addDestination = [&](const char* icon, const char* title, const char* subtitle, Callback activate) {
          adw_preferences_group_add(destinations, helix::settings::makeNavRow(icon, title, subtitle, std::move(activate)));
        };
        addDestination("battery-symbolic", "Power", "Battery warnings, suspend behavior and idle timers.", [this] { select("power"); });
        addDestination("application-x-executable-symbolic", "Helix Software", "Browse Flatpak applications and manage Fedora packages.", [this] { openApplication("helix-software --page installed"); });
        addDestination("utilities-system-monitor-symbolic", "System Monitor", "Processes, resources and service health.", [this] { openApplication("helix-system-monitor"); });
        addDestination("phone-symbolic", "Helix Phone", "Pair devices, find your phone and send files.", [this] { openApplication("helix-phone"); });
        addDestination("help-about-symbolic", "About", "Operating system, kernel and Helix service availability.", [this] { select("about"); });
      }
      if (hubOverview && !helix::settings::usesChipHub(page)) {
        const char* fallback = page == "network" ? "Preferences"
            : page == "system" ? "More"
            : page == "appearance" ? "Appearance Settings"
            : "Desktop Settings";
        hubNavRows.clear();
        helix::settings::populateHubNav(catalog, page, fallback,
          [this](const char* title) { return group(title); },
          [this](const helix::settings::HubPaneSpec& pane) { openHubPane(pane.id); },
          [this](const helix::settings::HubPaneSpec& pane, GtkWidget* row) {
            hubNavRows[pane.id] = row;
          });
      }
      if (page == "power" && std::ranges::any_of(catalog, [](const Json& item) { return item.value("group-overridden", false); })) {
        auto* reset = row(group("Idle Timers"), "Restore configured idle timers", "Resets the timer collection, including its original commands and order.");
        adw_action_row_add_suffix(reset, button("Reset", [this] {
          client.callAsync("configuration", "Reset", g_variant_new("(s)", "idle.behavior"), [this](Json, std::string error) { if (!error.empty()) toast("Idle timers could not be reset"); scheduleRefresh(); });
        }));
      }
      if (page == "keyboard" && query.empty()) {
        auto* scope = row(group("Shortcut Scope"), "Helix Shell", "These bindings control shell navigation. GTK applications keep their standard shortcuts. Global desktop and workspace bindings remain in your Niri configuration.");
        adw_action_row_set_subtitle_lines(scope, 0);
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
      const int chipDistinct = chipPane ? helix::settings::matchingGroupCount(catalog, page, *chipPane) : -1;
      for (const auto& setting : orderedCatalog) {
        if (query.empty() && page == "appearance" && !appearanceModes.empty() && setting.value("id", "") == "theme.mode") continue;
        if ((query.empty() && setting.value("page", "") != page) || !matchesSetting(setting)) continue;
        if (hubOverview && !helix::settings::usesChipHub(page)) {
          if (helix::settings::paneForSettingGroup(page, setting.value("group", ""))) continue;
        }
        if (chipPane && !helix::settings::settingMatchesPane(setting, *chipPane)) continue;
        const auto name = (query.empty() ? std::string{} : setting.value("page", "") + " · ") + setting.at("group").get<std::string>();
        if (page == "displays" && query.empty() && setting.value("group", "") == "Brightness" && !groups.contains("_brightness-preview")) {
          auto* preview = group("Brightness Sync", false);
          addChoicePreview(preview, helix::settings::ChoicePreview::Kind::BrightnessSync);
          groups["_brightness-preview"] = preview;
        }
        if (!groups.contains(name)) {
          if (chipPane) {
            const auto display = helix::settings::paneGroupTitle(setting.at("group").get<std::string>(), *chipPane, chipDistinct);
            auto* fields = group(name, false);
            if (!display.empty()) adw_preferences_group_set_title(fields, display.c_str());
            groups[name] = fields;
          } else groups[name] = group(name);
        }
        addSetting(groups[name], setting);
      }
      if (page == "desktop-dock" && connected && snapshots.contains("configuration") && snapshots.at("configuration").value("helix-desktop", false) && query.empty()) {
        if (matches("Widgets Extensions Editors Advanced Desktop Settings")) {
          auto* pending = group("Advanced Desktop Settings");
          adw_preferences_group_set_description(pending, "Per-widget editors are not in Settings yet. Extensions are retired; existing configuration stays active until those editors land.");
          for (const auto& name : {"Per-widget settings editors", "Extensions (retired)"}) {
            auto* item = row(pending, name, "Not yet available");
            gtk_widget_set_sensitive(GTK_WIDGET(item), false);
          }
        }
      }
      if (!query.empty()) {
        AdwPreferencesGroup* palettes = nullptr;
        auto paletteGroup = [&] { if (!palettes) palettes = group("Palette Management"); return palettes; };
        if (matches("Appearance Palette Library Browse Colors")) {
          auto* palette = row(paletteGroup(), "Palette Library", "Compare built-in, community and custom palettes before applying one.");
          singleAction(palette, button("Browse", [this] { choosePalette(); }));
        }
        if (matches("Appearance Community Palettes Refresh Catalog")) {
          auto* catalogActions = row(paletteGroup(), "Community Palettes", "Refresh available palettes from the community catalog.");
          singleAction(catalogActions, button("Refresh", [this] {
            client.callAsync("appearance", "RefreshPalettes", nullptr, [this](Json, std::string error) {
              toast(error.empty() ? "Refreshing the palette catalog" : "Palette catalog could not be refreshed"); scheduleRefresh();
            });
          }));
        }
        if (matches("Appearance Custom Palettes Save Current Palette")) {
          auto* save = row(paletteGroup(), "Save Current Palette", "Keep a copy of the current colors in your custom palette library.");
          singleAction(save, button("Save", [this] {
            client.callAsync("appearance", "SavePalette", g_variant_new("(s)", "current"), [this](Json, std::string error) {
              toast(error.empty() ? "Palette saved" : "Palette could not be saved"); scheduleRefresh();
            });
          }));
        }
        if (matches("Appearance Accent Color Choose")) {
          auto* accent = row(paletteGroup(), "Accent Color", "Creates a custom palette from the current appearance.");
          singleAction(accent, button("Choose", [this] { chooseAccent(); }));
        }
        if (snapshots.contains("configuration") && snapshots["configuration"].value("helix-desktop", false)) {
          AdwPreferencesGroup* wallpapers = nullptr;
          auto wallpaperGroup = [&] { if (!wallpapers) wallpapers = group("Wallpaper Sources"); return wallpapers; };
          if (matches("Appearance Desktop Wallpaper Library Browse")) {
            auto* wallpaper = row(wallpaperGroup(), "Wallpaper Library", "Browse wallpapers from your configured folders.");
            singleAction(wallpaper, button("Browse", [this] { chooseWallpaper(); }));
          }
          if (matches("Appearance Wallpaper Files Choose Local File Image Video")) {
            auto* file = row(wallpaperGroup(), "Choose a Local File", "Use an image or video supported by Helix Shell.");
            singleAction(file, button("Choose", [this] { chooseWallpaperFile(); }));
          }
          if (matches("Appearance Wallpaper Wallhaven Browse Online")) {
            auto* wallhaven = row(wallpaperGroup(), "Wallhaven", "Search online wallpapers using your existing preferences and credentials.");
            singleAction(wallhaven, button("Browse", [this] { chooseWallpaper(true); }));
          }
        }
      }
      if (!gtk_widget_get_first_child(GTK_WIDGET(body))) showEmptyState();
      if (wantPane) {
        const auto paneId = hubPane;
        openHubPane(paneId);
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
    for (auto* preview : choicePreviews) helix::settings::ChoicePreview::update(preview, catalog);
    gtk_widget_set_sensitive(GTK_WIDGET(overviewBody), connected || page == "online-accounts");
    if (paneBody) gtk_widget_set_sensitive(GTK_WIDGET(paneBody), connected || page == "online-accounts");
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
    onSignal(search, "search-changed", [this] {
      const std::string nextQuery = gtk_editable_get_text(GTK_EDITABLE(search));
      if (query == nextQuery) return;
      query = nextQuery;
      if (!query.empty()) {
        section.clear(); requestedSection.clear(); hubPane.clear();
        if (sectionTick) { gtk_widget_remove_tick_callback(scroller, sectionTick); sectionTick = 0; }
        popHubToOverview();
      }
      rendered.clear(); render();
    });
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
    banner = ADW_BANNER(adw_banner_new("Connecting to Helix services…")); adw_banner_set_button_label(banner, "Retry");
    onSignal(banner, "button-clicked", [this] { scheduleRefresh(); });
    hubNav = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
    auto* overviewToolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(overviewToolbar), adw_header_bar_new());
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(overviewToolbar), GTK_WIDGET(banner));
    auto [scroll, box] = makeScrollBody();
    overviewScroller = scroll;
    overviewBody = box;
    body = overviewBody;
    scroller = overviewScroller;
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(overviewToolbar), overviewScroller);
    overviewPage = adw_navigation_page_new(overviewToolbar, "Appearance");
    adw_navigation_view_push(hubNav, overviewPage);
    g_signal_connect(hubNav, "popped", G_CALLBACK(+[](AdwNavigationView*, AdwNavigationPage*, gpointer data) {
      auto& self = *static_cast<Settings*>(data);
      if (self.suppressingHubPop) return;
      const auto restore = self.hubOriginPane.empty() ? self.hubPane : self.hubOriginPane;
      self.hubPane.clear();
      self.section.clear();
      self.hubPanePage = nullptr;
      self.hubPaneHeader = nullptr;
      self.paneBody = nullptr;
      self.paneScroller = nullptr;
      self.body = self.overviewBody;
      self.scroller = self.overviewScroller;
      self.rendered.clear();
      self.render();
      self.focusHubNavRow(restore);
    }), this);
    contentPage = adw_navigation_page_new(GTK_WIDGET(hubNav), "Appearance");
    adw_navigation_split_view_set_content(split, contentPage);
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
  helix::apps::guardInspection(G_APPLICATION(app));
  Settings settings(app);
  g_application_add_main_option(G_APPLICATION(app), "search", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, "Find settings", "TEXT");
  g_application_add_main_option(G_APPLICATION(app), "section", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, "Open a Settings section", "ID");
  g_application_add_main_option(G_APPLICATION(app), "page", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, "Open a Settings page", "ID");
  g_application_add_main_option(G_APPLICATION(app), "wallpapers", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, "Open the library or wallhaven wallpaper browser", "SOURCE");
  g_application_add_main_option(G_APPLICATION(app), "palettes", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, "Open the palette browser", nullptr);
  g_application_add_main_option(G_APPLICATION(app), "inspect-layout", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, "Print layout state of the running Settings window", nullptr);
  g_application_add_main_option(G_APPLICATION(app), "save-preview", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, "Save a PNG of the running window", "FILE");
  g_signal_connect(app, "activate", G_CALLBACK(+[](GApplication*, gpointer data) {
    auto& self = *static_cast<Settings*>(data); if (!self.window) self.create(); gtk_window_present(GTK_WINDOW(self.window));
  }), &settings);
  g_signal_connect(app, "command-line", G_CALLBACK((+[](GApplication* application, GApplicationCommandLine* command, gpointer data) -> int {
    auto& self = *static_cast<Settings*>(data); const gchar *page = nullptr, *preview = nullptr, *wallpapers = nullptr, *section = nullptr, *search = nullptr;
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
    if (section && (settings::nativeGroupForSection(section).empty() || (page && std::string_view(page) != settings::nativePageForTargetSection(section)))) {
      g_application_command_line_printerr(command, "Section must match its page\n"); return 2;
    }
    g_variant_dict_lookup(g_application_command_line_get_options_dict(command), "search", "&s", &search);
    if (search && (section || wallpapers || g_variant_dict_contains(g_application_command_line_get_options_dict(command), "palettes"))) {
      g_application_command_line_printerr(command, "Search cannot be combined with a section or browser\n"); return 2;
    }
    gboolean inspect = false;
    g_variant_dict_lookup(g_application_command_line_get_options_dict(command), "inspect-layout", "b", &inspect);
    if (inspect && (!self.window)) { g_application_command_line_printerr(command, "Open Settings before inspecting its layout\n"); return 1; }
    if (inspect && self.window) {
      Json layout = {{"width", gtk_widget_get_width(GTK_WIDGET(self.window))}, {"height", gtk_widget_get_height(GTK_WIDGET(self.window))},
        {"collapsed", bool(adw_navigation_split_view_get_collapsed(self.split))}, {"page", self.page},
        {"hub-pane", self.hubPane}, {"privacy-chip", self.privacyChip},
        {"desktop-page", bool(gtk_widget_get_visible(GTK_WIDGET(self.pageRows.at("desktop-dock"))))}, {"connected", self.connected},
        {"content-width", gtk_widget_get_width(GTK_WIDGET(self.body))},
        {"query", self.query}, {"section", self.section}, {"scroll-position", gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self.scroller)))},
        {"mutations-enabled", bool(gtk_widget_get_sensitive(GTK_WIDGET(self.body)))},
        {"palette-swatches", self.paletteSwatches}, {"wallpaper-preview", self.wallpaperPreview && gtk_picture_get_paintable(self.wallpaperPreview) != nullptr}};
      layout["groups"] = Json::array();
      for (auto* child = gtk_widget_get_first_child(GTK_WIDGET(self.body)); child; child = gtk_widget_get_next_sibling(child))
        if (ADW_IS_PREFERENCES_GROUP(child)) layout["groups"].push_back(adw_preferences_group_get_title(ADW_PREFERENCES_GROUP(child)));
      if (self.accountsPage) layout["online-accounts"] = self.accountsPage->inspect();
      if (self.agentsPage) layout["agents"] = self.agentsPage->inspect();
      if (auto browser = self.wallpaperBrowser.lock(); browser && !browser->isClosed()) layout["wallpaper-browser"] = browser->inspect();
      if (auto browser = self.paletteBrowser.lock(); browser && !browser->isClosed()) layout["palette-browser"] = browser->inspect();
      if (self.layoutPreview) layout["layout-preview"] = helix::settings::LayoutPreview::description(self.layoutPreview);
      for (const auto& [mode, widget] : self.appearanceModes) if (gtk_toggle_button_get_active(widget)) layout["appearance-mode"] = mode;
      g_application_command_line_print(command, "%s\n", layout.dump().c_str());
      return 0;
    }
    if (page) self.requestedPage = page;
    if (search) self.requestedQuery = search;
    self.requestedSection = section ? section : "";
    if (section) self.requestedPage = settings::nativePageForTargetSection(section);
    if (wallpapers) { self.requestedPage = "appearance"; self.requestedBrowser = wallpapers; }
    gboolean palettes = false; g_variant_dict_lookup(g_application_command_line_get_options_dict(command), "palettes", "b", &palettes);
    if (palettes) { self.requestedPage = "appearance"; self.requestedPalette = true; }
    g_application_activate(application);
    if (page || section || wallpapers || palettes || search) self.scheduleRefresh();
    return 0;
  })), &settings);
  const int result = g_application_run(G_APPLICATION(app), argc, argv);
  // Settings owns the window reference until its callbacks have been disconnected.
  g_object_unref(app);
  return result;
}
