#include "apps/common/inspection_guard.h"
#include "apps/common/preview_export.h"
#include "apps/common/appearance_binding.h"
#include "system/package_kit_service.h"
#include "core/process/process.h"
#include <adwaita.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <tuple>

namespace {
using namespace noctalia::packages;
using Callback = std::function<void()>;
void signal(gpointer object, const char* name, Callback callback, bool notify = false) {
  auto* owned = new Callback(std::move(callback));
  const auto destroy = +[](gpointer data, GClosure*) { delete static_cast<Callback*>(data); };
  if (notify) g_signal_connect_data(object, name, G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) { (*static_cast<Callback*>(data))(); }), owned, destroy, G_CONNECT_DEFAULT);
  else g_signal_connect_data(object, name, G_CALLBACK(+[](gpointer, gpointer data) { (*static_cast<Callback*>(data))(); }), owned, destroy, G_CONNECT_DEFAULT);
}
void queue(Callback callback) {
  g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer data) -> gboolean { (*static_cast<Callback*>(data))(); return G_SOURCE_REMOVE; },
    new Callback(std::move(callback)), +[](gpointer data) { delete static_cast<Callback*>(data); });
}
GtkWidget* button(const char* label, Callback callback) {
  auto* result = gtk_button_new_with_label(label); signal(result, "clicked", std::move(callback)); return result;
}
struct Software : std::enable_shared_from_this<Software> {
  AdwApplication* app;
  std::unique_ptr<helix::apps::AppearanceBinding> appearance;
  AdwApplicationWindow* window = nullptr;
  AdwToastOverlay* toasts = nullptr;
  AdwViewStack* stack = nullptr;
  AdwViewStackPage* browsePage = nullptr;
  GtkWidget *switcher = nullptr, *bottom = nullptr, *source = nullptr, *search = nullptr, *refresh = nullptr, *cancel = nullptr;
  GtkLabel* status = nullptr;
  std::map<std::string, GtkBox*> bodies;
  std::vector<Kind> sources;
  Kind selectedKind = Kind::Flatpak;
  std::vector<PackageHit> hits;
  std::string page = "browse", query;
  bool busy = false, mutation = false, closing = false, creating = false, failed = false;
  unsigned shown = 100;
  explicit Software(AdwApplication* application) : app(application) {}
  ~Software() { appearance.reset(); if (window) g_object_unref(window); }
  Kind kind() const { return selectedKind; }
  void toast(const char* text) { if (!closing) adw_toast_overlay_add_toast(toasts, adw_toast_new(text)); }
  void controls() {
    if (closing) return;
    for (auto* widget : {source, search, refresh, switcher, bottom}) gtk_widget_set_sensitive(widget, !busy && !sources.empty());
    if (browsePage) adw_view_stack_page_set_visible(browsePage, selectedKind != Kind::Firmware);
    gtk_widget_set_visible(cancel, busy); gtk_widget_set_sensitive(GTK_WIDGET(stack), !busy);
  }
  void results() {
    if (closing) return;
    auto* body = bodies.at(page);
    while (auto* child = gtk_widget_get_first_child(GTK_WIDGET(body))) gtk_box_remove(body, child);
    if (hits.empty()) {
      auto* empty = adw_status_page_new();
      adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "system-software-install-symbolic");
      adw_status_page_set_title(ADW_STATUS_PAGE(empty), failed ? "Software List Unavailable" : sources.empty() ? "Software Backends Unavailable" : page == "browse" && query.empty() ? "Find Software" : page == "updates" ? "No Updates Listed" : page == "installed" ? "No Software Installed from This Source" : "No Results");
      adw_status_page_set_description(ADW_STATUS_PAGE(empty), failed ? "The selected backend could not provide a software list. Refresh to retry." : sources.empty() ? "Install Flatpak or PackageKit to browse supported software sources." : page == "browse" && query.empty() ? "Search Flatpak applications or Fedora packages using the source selector." : "Refresh or try another source or search.");
      gtk_box_append(body, empty); return;
    }
    auto* group = adw_preferences_group_new(); adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), page == "updates" ? "Available Updates" : page == "installed" ? "Installed Software" : "Search Results");
    gtk_box_append(body, group);
    for (std::size_t i = 0; i < std::min<std::size_t>(shown, hits.size()); ++i) {
      const auto hit = hits[i]; auto* row = adw_action_row_new();
      adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), false);
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), (hit.title.empty() ? hit.name : hit.title).c_str());
      std::string detail = hit.summary.empty() ? hit.name : hit.summary;
      if (!hit.version.empty()) detail += " · " + hit.version;
      if (!hit.installation.empty()) detail += " · " + hit.installation;
      adw_action_row_set_subtitle(ADW_ACTION_ROW(row), detail.c_str());
      adw_action_row_set_title_lines(ADW_ACTION_ROW(row), 2); adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 3);
      adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name(kind() == Kind::Flatpak ? "application-x-executable-symbolic" : kind() == Kind::Firmware ? "computer-symbolic" : "package-x-generic-symbolic"));
      if (kind() == Kind::Firmware && hit.state != PackageState::Update) { adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row); continue; }
      const auto action = hit.state == PackageState::Update ? Action::Update : hit.state == PackageState::Installed ? Action::Remove : Action::Install;
      const bool protectedPackage = kind() == Kind::Fedora && action == Action::Remove && isProtectedPackage(hit.name);
      if (protectedPackage) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), ("Protected system package · " + detail).c_str());
      auto* apply = button(action == Action::Install ? "Install" : action == Action::Remove ? "Remove" : "Update", [this, hit, action] { confirm(hit, action); });
      gtk_widget_set_valign(apply, GTK_ALIGN_CENTER);
      if (protectedPackage) { gtk_button_set_label(GTK_BUTTON(apply), "Protected"); gtk_widget_set_sensitive(apply, false); gtk_widget_set_tooltip_text(apply, "This system package is protected."); }
      adw_action_row_add_suffix(ADW_ACTION_ROW(row), apply); adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }
    if (page == "updates") gtk_box_append(body, button("Update All Listed", [this] { confirmAll(); }));
    if (shown < hits.size()) gtk_box_append(body, button("Show More", [this] { shown += 100; results(); }));
  }
  void begin(bool changing) {
    busy = true; mutation = changing; failed = false; g_application_hold(G_APPLICATION(app)); controls();
    gtk_label_set_text(status, changing ? "Working on the selected package…" : "Loading software…");
  }
  void finish(CommandResult result) {
    const bool changing = mutation; busy = false; mutation = false;
    if (!closing) {
      if (!result.success && !changing) { failed = !result.cancelled; results(); }
      if (result.success && !changing) { hits = std::move(result.hits); shown = 100; results(); }
      gtk_label_set_text(status, result.cancelled ? "Request canceled. Refresh to check the current package state." : result.success ? changing ? "Operation completed." : "Software list is ready." : "The software backend could not complete this request. Refresh and retry.");
      if (changing && result.success) toast("Package operation completed. Refresh to see the current state.");
      controls();
    }
    g_application_release(G_APPLICATION(app));
  }
  CommandCompletion completion() {
    auto self = shared_from_this(); return [self](CommandResult result) { if (result.success) sortHits(result.hits); queue([self, result = std::move(result)]() mutable { self->finish(std::move(result)); }); };
  }
  void load() {
    if (busy || creating || closing || sources.empty()) return;
    if (selectedKind == Kind::Firmware && page == "browse") { page = "updates"; creating = true; adw_view_stack_set_visible_child_name(stack, page.c_str()); gtk_widget_set_visible(search, false); creating = false; }
    query = gtk_editable_get_text(GTK_EDITABLE(search)); hits.clear(); results();
    if (page == "browse" && query.empty()) { gtk_label_set_text(status, "Choose a source and search for software."); return; }
    if (page == "browse" && !isSafeSearchQuery(query)) { toast("Use a short search without control characters or shell punctuation."); return; }
    begin(false);
    const bool accepted = page == "installed" ? listInstalled(completion(), kind()) : page == "updates" ? listUpdates(completion(), kind()) : noctalia::packages::search(query, completion(), kind());
    if (!accepted) finish({.error = "Backend unavailable or busy"});
  }
  void confirmAll() {
    if (busy || hits.empty()) return;
    const auto selected = kind(); std::vector<std::string> names;
    for (const auto& hit : hits) names.push_back(hit.name);
    const auto description = std::to_string(names.size()) + " listed updates from the selected source will be applied.";
    auto* dialog = adw_alert_dialog_new("Apply All Listed Updates?", description.c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "apply", "Update", nullptr);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "apply", ADW_RESPONSE_SUGGESTED);
    struct Request { std::shared_ptr<Software> self; std::vector<std::string> names; Kind kind; };
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window), nullptr, +[](GObject* source, GAsyncResult* result, gpointer data) {
      std::unique_ptr<Request> request(static_cast<Request*>(data));
      if (std::string(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result)) != "apply" || request->self->closing || request->self->busy) return;
      request->self->begin(true);
      if (!updatePackages(std::move(request->names), request->self->completion(), {}, request->kind)) request->self->finish({.error = "Updates unavailable"});
    }, new Request{shared_from_this(), std::move(names), selected});
  }
  void confirm(const PackageHit& hit, Action action) {
    if (busy || sources.empty()) return;
    const auto selectedKind = kind();
    const char* verb = action == Action::Install ? "Install" : action == Action::Remove ? "Remove" : "Update";
    const auto heading = std::string(verb) + " " + (hit.title.empty() ? hit.name : hit.title) + "?";
    const auto detail = hit.name + (selectedKind == Kind::Flatpak ? "\nFlatpak application" : selectedKind == Kind::Firmware ? "\nDevice firmware" : "\nFedora system package") + (hit.version.empty() ? "" : "\nVersion: " + hit.version);
    auto* dialog = adw_alert_dialog_new(heading.c_str(), detail.c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "apply", verb, nullptr);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "apply", action == Action::Remove ? ADW_RESPONSE_DESTRUCTIVE : ADW_RESPONSE_SUGGESTED);
    struct Request { std::shared_ptr<Software> self; PackageHit hit; Action action; Kind kind; };
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window), nullptr, +[](GObject* source, GAsyncResult* result, gpointer data) {
      std::unique_ptr<Request> request(static_cast<Request*>(data));
      if (std::string(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result)) != "apply" || request->self->closing) return;
      auto self = request->self; if (self->busy) return; self->begin(true);
      auto progress = [self](PkconProgress value) { queue([self, value] { if (!self->closing) gtk_label_set_text(self->status, (value.status + (value.percent < 0 ? "" : " · " + std::to_string(value.percent) + "%")).c_str()); }); };
      if (!mutate(request->action, request->hit.name, self->completion(), progress, request->kind, request->hit.installation, request->hit.repo)) self->finish({.error = "Operation unavailable"});
    }, new Request{shared_from_this(), hit, action, selectedKind});
  }
  void create() {
    creating = true; closing = false;
    if (window) g_object_unref(window);
    window = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app))); g_object_ref(window);
    appearance = std::make_unique<helix::apps::AppearanceBinding>(GTK_WIDGET(window));
    gtk_window_set_title(GTK_WINDOW(window), "Helix Software"); gtk_window_set_default_size(GTK_WINDOW(window), 960, 720); gtk_widget_set_size_request(GTK_WIDGET(window), 320, 360);
    g_signal_connect(window, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean { static_cast<Software*>(data)->closing = true; return false; }), this);
    toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new()); auto* toolbar = adw_toolbar_view_new(); auto* header = adw_header_bar_new(); adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12); auto* fields = adw_preferences_group_new();
    for (auto side : {"margin-start", "margin-end", "margin-top"}) g_object_set(fields, side, 12, nullptr);
    auto* options = gtk_string_list_new(nullptr); sources.clear();
    if (flatpakAvailable()) { sources.push_back(Kind::Flatpak); gtk_string_list_append(options, "Flatpak Apps"); }
    if (backendAvailable() && process::commandExists("rpm")) { sources.push_back(Kind::Fedora); gtk_string_list_append(options, "Fedora Packages"); }
    if (firmwareAvailable()) { sources.push_back(Kind::Firmware); gtk_string_list_append(options, "Device Firmware"); }
    if (sources.empty()) gtk_string_list_append(options, "Unavailable");
    auto* sourceRow = adw_action_row_new(); adw_preferences_row_set_title(ADW_PREFERENCES_ROW(sourceRow), "Source");
    source = gtk_drop_down_new(G_LIST_MODEL(options), nullptr); g_object_unref(options);
    if (!sources.empty()) {
      const auto selected = std::ranges::find(sources, selectedKind);
      const auto index = selected == sources.end() ? 0u : guint(selected - sources.begin());
      selectedKind = sources[index]; gtk_drop_down_set_selected(GTK_DROP_DOWN(source), index);
    }
 gtk_widget_set_valign(source, GTK_ALIGN_CENTER);
    gtk_accessible_update_property(GTK_ACCESSIBLE(source), GTK_ACCESSIBLE_PROPERTY_LABEL, "Software source", -1); adw_action_row_add_suffix(ADW_ACTION_ROW(sourceRow), source); adw_preferences_group_add(ADW_PREFERENCES_GROUP(fields), sourceRow);
    gtk_box_append(GTK_BOX(root), fields);
    auto* actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_widget_set_margin_start(actions, 12); gtk_widget_set_margin_end(actions, 12);
    search = gtk_search_entry_new(); gtk_widget_set_hexpand(search, true); gtk_accessible_update_property(GTK_ACCESSIBLE(search), GTK_ACCESSIBLE_PROPERTY_LABEL, "Search software", -1);
    refresh = button("Refresh", [this] { load(); }); cancel = button("Cancel Request", [] { cancelCurrent(); });
    gtk_box_append(GTK_BOX(actions), search); auto* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_box_append(GTK_BOX(buttons), refresh); gtk_box_append(GTK_BOX(buttons), cancel); gtk_box_append(GTK_BOX(actions), buttons); gtk_box_append(GTK_BOX(root), actions);
    status = GTK_LABEL(gtk_label_new("Choose a source and search for software.")); gtk_label_set_wrap(status, true); gtk_widget_set_margin_start(GTK_WIDGET(status), 12); gtk_widget_set_margin_end(GTK_WIDGET(status), 12); gtk_box_append(GTK_BOX(root), GTK_WIDGET(status));
    stack = ADW_VIEW_STACK(adw_view_stack_new()); gtk_widget_set_vexpand(GTK_WIDGET(stack), true); bodies.clear();
    for (const auto& [id, label, icon] : std::vector<std::tuple<const char*, const char*, const char*>>{{"browse", "Browse", "system-search-symbolic"}, {"installed", "Installed", "application-x-executable-symbolic"}, {"updates", "Updates", "software-update-available-symbolic"}}) {
      auto* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12); for (auto side : {"margin-start", "margin-end", "margin-top", "margin-bottom"}) g_object_set(body, side, 18, nullptr);
      auto* clamp = adw_clamp_new(); adw_clamp_set_child(ADW_CLAMP(clamp), body); auto* scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
      auto* view = adw_view_stack_add_titled_with_icon(stack, scroll, id, label, icon); if (std::string_view(id) == "browse") browsePage = view; bodies[id] = GTK_BOX(body);
    }
    adw_view_stack_set_visible_child_name(stack, page.c_str());
    switcher = adw_view_switcher_new(); adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), stack);
    auto* headerTitle = gtk_stack_new(); gtk_stack_add_named(GTK_STACK(headerTitle), switcher, "tabs");
    gtk_stack_add_named(GTK_STACK(headerTitle), adw_window_title_new("Helix Software", nullptr), "title");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), headerTitle);
    bottom = adw_view_switcher_bar_new(); adw_view_switcher_bar_set_stack(ADW_VIEW_SWITCHER_BAR(bottom), stack); adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar), bottom);
    auto* breakpoint = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 600sp")); GValue yes = G_VALUE_INIT, no = G_VALUE_INIT;
    g_value_init(&yes, G_TYPE_BOOLEAN); g_value_set_boolean(&yes, true); g_value_init(&no, G_TYPE_BOOLEAN); g_value_set_boolean(&no, false);
    adw_breakpoint_add_setter(breakpoint, G_OBJECT(bottom), "reveal", &yes);
    GValue title = G_VALUE_INIT; g_value_init(&title, G_TYPE_STRING); g_value_set_string(&title, "title");
    adw_breakpoint_add_setter(breakpoint, G_OBJECT(headerTitle), "visible-child-name", &title); g_value_unset(&title); g_value_unset(&yes); g_value_unset(&no); adw_application_window_add_breakpoint(window, breakpoint);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(stack)); adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root); adw_toast_overlay_set_child(toasts, toolbar); adw_application_window_set_content(window, GTK_WIDGET(toasts));
    signal(source, "notify::selected", [this] { if (!creating && !sources.empty()) { selectedKind = sources.at(gtk_drop_down_get_selected(GTK_DROP_DOWN(source))); load(); } }, true);
    signal(stack, "notify::visible-child-name", [this] { page = adw_view_stack_get_visible_child_name(stack); gtk_widget_set_visible(search, page == "browse"); load(); }, true);
    signal(search, "activate", [this] { load(); });
    creating = false; controls(); results();
  }
};
}
int main(int argc, char** argv) {
  auto* app = adw_application_new("org.helix.Software", G_APPLICATION_HANDLES_COMMAND_LINE);
  helix::apps::guardInspection(G_APPLICATION(app));
  auto software = std::make_shared<Software>(app);
  for (const auto& [id, description] : std::vector<std::pair<const char*, const char*>>{{"page", "Open browse, installed or updates"}, {"source", "Select flatpak, packages or firmware"}, {"search", "Search software"}, {"save-preview", "Save a PNG of the running window"}})
    g_application_add_main_option(G_APPLICATION(app), id, 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, description, "VALUE");
  g_application_add_main_option(G_APPLICATION(app), "inspect-layout", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, "Print running window state", nullptr);
  g_signal_connect(app, "activate", G_CALLBACK(+[](GApplication*, gpointer data) { auto& self = *static_cast<Software*>(data); if (!self.window || self.closing) self.create(); gtk_window_present(GTK_WINDOW(self.window)); }), software.get());
  g_signal_connect(app, "command-line", G_CALLBACK((+[](GApplication* application, GApplicationCommandLine* command, gpointer data) -> int {
    auto& self = *static_cast<Software*>(data); auto* options = g_application_command_line_get_options_dict(command); const gchar *page = nullptr, *source = nullptr, *query = nullptr, *preview = nullptr; gboolean inspect = false;
    g_variant_dict_lookup(options, "page", "&s", &page); g_variant_dict_lookup(options, "source", "&s", &source); g_variant_dict_lookup(options, "search", "&s", &query); g_variant_dict_lookup(options, "save-preview", "&s", &preview); g_variant_dict_lookup(options, "inspect-layout", "b", &inspect);
    if (preview) return self.window && helix::apps::savePreview(GTK_WIDGET(self.window), preview) ? 0 : 1;
    if (inspect && (!self.window || self.closing)) { g_application_command_line_printerr(command, "Open Software before inspecting its layout\n"); return 1; }
    if (inspect && self.window) { nlohmann::json state = {{"page", self.page}, {"busy", self.busy}, {"results", self.hits.size()}, {"shown", std::min<std::size_t>(self.shown, self.hits.size())}, {"width", gtk_widget_get_width(GTK_WIDGET(self.window))}, {"height", gtk_widget_get_height(GTK_WIDGET(self.window))}, {"source", self.kind() == Kind::Flatpak ? "flatpak" : self.kind() == Kind::Firmware ? "firmware" : "packages"}}; g_application_command_line_print(command, "%s\n", state.dump().c_str()); return 0; }
    if ((page && std::string(page) != "browse" && std::string(page) != "installed" && std::string(page) != "updates") || (source && std::string(source) != "flatpak" && std::string(source) != "packages" && std::string(source) != "firmware")) { g_application_command_line_printerr(command, "Unknown software page or source\n"); return 2; }
    if (query && ((source && std::string(source) == "firmware") || (!source && self.selectedKind == Kind::Firmware))) { g_application_command_line_printerr(command, "Firmware does not support software search; open installed or updates\n"); return 2; }
    if (self.busy && (page || source || query)) { g_application_command_line_printerr(command, "A software operation is in progress\n"); return 1; }
    g_application_activate(application); self.creating = true;
    if (source) { const auto kind = std::string(source) == "flatpak" ? Kind::Flatpak : std::string(source) == "firmware" ? Kind::Firmware : Kind::Fedora; const auto it = std::ranges::find(self.sources, kind); if (it == self.sources.end()) { self.creating = false; g_application_command_line_printerr(command, "Software source unavailable\n"); return 1; } gtk_drop_down_set_selected(GTK_DROP_DOWN(self.source), guint(it - self.sources.begin())); self.selectedKind = kind; }
    if (query) { gtk_editable_set_text(GTK_EDITABLE(self.search), query); self.page = "browse"; }
    else if (page) self.page = page;
    adw_view_stack_set_visible_child_name(self.stack, self.page.c_str()); gtk_widget_set_visible(self.search, self.page == "browse"); self.creating = false;
    if (page || source || query) self.load();
    return 0;
  })), software.get());
  const int result = g_application_run(G_APPLICATION(app), argc, argv); software.reset(); g_object_unref(app); return result;
}
