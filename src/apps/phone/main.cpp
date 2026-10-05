#include "apps/common/inspection_guard.h"
#include "apps/common/preview_export.h"
#include "apps/common/appearance_binding.h"
#include "dbus/session_bus.h"
#include "dbus/kdeconnect/phone_connect_service.h"
#include "core/deferred_call.h"
#include "core/timer_manager.h"
#include <adwaita.h>
#include <nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <string>

namespace {
using Callback = std::function<void()>;
void signal(gpointer object, const char* name, Callback callback) {
  g_signal_connect_data(object, name, G_CALLBACK(+[](gpointer, gpointer data) { (*static_cast<Callback*>(data))(); }),
    new Callback(std::move(callback)), +[](gpointer data, GClosure*) { delete static_cast<Callback*>(data); }, G_CONNECT_DEFAULT);
}
struct Phone {
  AdwApplication* app;
  std::unique_ptr<helix::apps::AppearanceBinding> appearance;
  AdwApplicationWindow* window = nullptr;
  AdwToastOverlay* toasts = nullptr;
  GtkBox* body = nullptr;
  std::unique_ptr<SessionBus> bus;
  std::unique_ptr<PhoneConnectService> backend;
  guint poll = 0;
  std::string fingerprint;
  bool closed = false;
  explicit Phone(AdwApplication* application) : app(application) {}
  ~Phone() {
    appearance.reset();
    if (poll) g_source_remove(poll);
    if (backend) backend->setChangeCallback({});
    backend.reset();
    if (window) g_object_unref(window);
  }
  void toast(const char* message) { if (!closed) adw_toast_overlay_add_toast(toasts, adw_toast_new(message)); }
  AdwPreferencesGroup* group(const char* name) {
    auto* fields = adw_preferences_group_new(); adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(fields), name);
    gtk_box_append(body, fields); return ADW_PREFERENCES_GROUP(fields);
  }
  AdwActionRow* row(AdwPreferencesGroup* fields, const std::string& name, const std::string& subtitle = "") {
    auto* item = adw_action_row_new(); adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(item), false);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(item), name.c_str()); adw_action_row_set_subtitle(ADW_ACTION_ROW(item), subtitle.c_str());
    adw_action_row_set_title_lines(ADW_ACTION_ROW(item), 2); adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(item), 3);
    adw_preferences_group_add(fields, item); return ADW_ACTION_ROW(item);
  }
  GtkWidget* action(AdwActionRow* row, const char* label, Callback callback) {
    auto* button = gtk_button_new_with_label(label); gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    signal(button, "clicked", std::move(callback)); adw_action_row_add_suffix(row, button); return button;
  }
  void confirm(const PhoneDevice& device, bool pair) {
    const auto title = std::string(pair ? "Pair with " : "Remove ") + device.name + "?";
    const auto explanation = pair ? std::string("Verify this pairing on your phone before approving it.") + (device.verificationKey.empty() ? "" : "\nVerification: " + device.verificationKey) : "This computer will no longer trust the device until you pair it again.";
    auto* dialog = adw_alert_dialog_new(title.c_str(), explanation.c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "apply", pair ? "Pair" : "Remove", nullptr);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "apply", pair ? ADW_RESPONSE_SUGGESTED : ADW_RESPONSE_DESTRUCTIVE);
    struct Request { Phone* self; std::string id; bool pair, requested; std::string verification; };
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window), nullptr, +[](GObject* source, GAsyncResult* result, gpointer data) {
      std::unique_ptr<Request> request(static_cast<Request*>(data));
      if (std::string(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result)) != "apply" || request->self->closed || !request->self->backend) return;
      const PhoneDevice* current = nullptr;
      for (const auto& device : request->self->backend->snapshot().devices) if (device.id == request->id) current = &device;
      if (!current || (request->pair && (!current->reachable || current->paired ||
          (request->requested && (!current->pairRequestedByPeer || current->verificationKey != request->verification))))) {
        request->self->toast("The device or pairing request changed. Review it again."); return;
      }
      if (!request->pair) request->self->backend->unpair(request->id);
      else if (request->requested) request->self->backend->acceptPair(request->id);
      else request->self->backend->requestPair(request->id);
      request->self->backend->scheduleRefresh();
    }, new Request{this, device.id, pair, device.pairRequestedByPeer, device.verificationKey});
  }
  void sendFile(const std::string& id) {
    auto* dialog = gtk_file_dialog_new(); gtk_file_dialog_set_title(dialog, "Send a File to Your Phone");
    struct Request { Phone* self; std::string id; };
    gtk_file_dialog_open(dialog, GTK_WINDOW(window), nullptr, +[](GObject* source, GAsyncResult* result, gpointer data) {
      std::unique_ptr<Request> request(static_cast<Request*>(data)); GError* error = nullptr;
      auto* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
      if (file) {
        auto* path = g_file_get_path(file);
        if (!request->self->closed && request->self->backend && path) {
          const PhoneDevice* current = nullptr;
          for (const auto& device : request->self->backend->snapshot().devices) if (device.id == request->id) current = &device;
          if (current && current->paired && current->reachable && current->sharePlugin) {
            request->self->backend->sharePath(request->id, path); request->self->toast("File transfer requested. Check your phone for completion.");
          } else request->self->toast("The phone is no longer available for file transfer.");
        }
        g_free(path); g_object_unref(file);
      }
      if (error) g_error_free(error);
    }, new Request{this, id});
    g_object_unref(dialog);
  }
  void render() {
    if (!body || closed) return;
    const auto state = backend ? backend->snapshot() : PhoneSnapshot{};
    nlohmann::json shape = {{"available", state.available}, {"activating", state.activating}, {"devices", nlohmann::json::array()}};
    for (const auto& device : state.devices) shape["devices"].push_back({device.id, device.name, device.reachable, device.paired, device.pairRequested, device.pairRequestedByPeer, device.verificationKey, device.hasBattery, device.charge, device.charging, device.sharePlugin, device.ringPlugin});
    const auto next = shape.dump(); if (next == fingerprint) return; fingerprint = next;
    while (auto* child = gtk_widget_get_first_child(GTK_WIDGET(body))) gtk_box_remove(body, child);
    if (!state.available) {
      auto* empty = adw_status_page_new(); adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "phone-symbolic");
      adw_status_page_set_title(ADW_STATUS_PAGE(empty), state.activating ? "Connecting to Phone Service" : "Phone Service Unavailable");
      adw_status_page_set_description(ADW_STATUS_PAGE(empty), "Helix Phone uses KDE Connect for device discovery, pairing and file transfer. Install KDE Connect on compatible devices."); gtk_box_append(body, empty);
      auto* connect = row(group("Connection"), "KDE Connect", "Start or reconnect the device service.");
      auto* retry = action(connect, "Connect", [this] { if (backend) backend->tryActivate(); else toast("The session device service is unavailable."); });
      gtk_widget_set_sensitive(retry, backend && !state.activating); return;
    }
    auto* discover = row(group("Find Devices"), "Devices on Your Network", "Open KDE Connect on your phone, then refresh to discover it.");
    action(discover, "Refresh", [this] { backend->setDiscovery(true); backend->refresh(); });
    if (state.devices.empty()) {
      auto* empty = adw_status_page_new(); adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "phone-symbolic"); adw_status_page_set_title(ADW_STATUS_PAGE(empty), "No Devices Found");
      adw_status_page_set_description(ADW_STATUS_PAGE(empty), "Use the same reachable network and check that device discovery is allowed by your firewall."); gtk_box_append(body, empty); return;
    }
    for (const auto& device : state.devices) {
      auto* fields = group(device.name.c_str());
      const auto subtitle = std::string(device.paired ? "Paired" : device.pairRequested ? "Pairing requested" : "Not paired") + (device.reachable ? " · Available" : " · Offline");
      auto* connection = row(fields, "Connection", subtitle); adw_action_row_add_prefix(connection, gtk_image_new_from_icon_name("phone-symbolic"));
      if (device.paired) action(connection, "Remove", [this, device] { confirm(device, false); });
      else {
        auto* pair = action(connection, device.pairRequestedByPeer ? "Review Pairing" : "Pair", [this, device] { confirm(device, true); }); gtk_widget_set_sensitive(pair, device.reachable);
        if (device.pairRequested) action(connection, "Decline", [this, id = device.id] { backend->rejectPair(id); });
      }
      if (device.hasBattery) row(fields, "Battery", std::to_string(device.charge) + "%" + (device.charging ? " · Charging" : ""));
      if (!device.paired) continue;
      auto* find = row(fields, "Find My Phone", !device.reachable ? "Your phone is offline. Reconnect it to use this feature." : device.ringPlugin ? "Ring the paired device so you can locate it." : "Finding this phone is not supported by its current connection.");
      auto* ring = action(find, "Ring", [this, id = device.id] { backend->ring(id); }); gtk_widget_set_sensitive(ring, device.reachable && device.ringPlugin);
      auto* files = row(fields, "Send a File", !device.reachable ? "Your phone is offline. Reconnect it to send files." : device.sharePlugin ? "Choose a local file to send to this device." : "File sharing is not enabled on this device.");
      auto* send = action(files, "Choose File", [this, id = device.id] { sendFile(id); }); gtk_widget_set_sensitive(send, device.reachable && device.sharePlugin);
    }
  }
  void create() {
    window = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app))); g_object_ref(window); closed = false;
    appearance = std::make_unique<helix::apps::AppearanceBinding>(GTK_WIDGET(window));
    gtk_window_set_title(GTK_WINDOW(window), "Helix Phone"); gtk_window_set_default_size(GTK_WINDOW(window), 800, 720); gtk_widget_set_size_request(GTK_WIDGET(window), 320, 360);
    g_signal_connect(window, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean { static_cast<Phone*>(data)->closed = true; return false; }), this);
    auto* toolbar = adw_toolbar_view_new(); adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
    body = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 24)); for (auto side : {"margin-start", "margin-end", "margin-top", "margin-bottom"}) g_object_set(body, side, 18, nullptr);
    auto* clamp = adw_clamp_new(); adw_clamp_set_child(ADW_CLAMP(clamp), GTK_WIDGET(body)); auto* scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll); toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new()); adw_toast_overlay_set_child(toasts, toolbar); adw_application_window_set_content(window, GTK_WIDGET(toasts));
    try { bus = std::make_unique<SessionBus>(); backend = std::make_unique<PhoneConnectService>(*bus, false); backend->setChangeCallback([this] { render(); }); backend->start(); }
    catch (...) { backend.reset(); bus.reset(); }
    render();
    poll = g_timeout_add(50, +[](gpointer data) -> gboolean {
      auto& self = *static_cast<Phone*>(data); if (self.closed) return G_SOURCE_CONTINUE;
      try { if (self.bus) self.bus->processPendingEvents(); DeferredCall::drainWakeFd(); for (auto& callback : DeferredCall::takePending()) callback(); TimerManager::instance().tick(); }
      catch (...) { self.toast("The phone connection could not be refreshed."); }
      return G_SOURCE_CONTINUE;
    }, this);
  }
};
}
int main(int argc, char** argv) {
  auto* app = adw_application_new("org.helix.Phone", G_APPLICATION_HANDLES_COMMAND_LINE); Phone phone(app);
  helix::apps::guardInspection(G_APPLICATION(app));
  g_application_add_main_option(G_APPLICATION(app), "inspect-layout", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, "Print window and connection state", nullptr);
  g_application_add_main_option(G_APPLICATION(app), "save-preview", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, "Save a PNG of the running window", "FILE");
  g_signal_connect(app, "activate", G_CALLBACK(+[](GApplication*, gpointer data) { auto& self = *static_cast<Phone*>(data); if (!self.window) self.create(); gtk_window_present(GTK_WINDOW(self.window)); }), &phone);
  g_signal_connect(app, "command-line", G_CALLBACK((+[](GApplication* application, GApplicationCommandLine* command, gpointer data) -> int {
    auto& self = *static_cast<Phone*>(data); auto* options = g_application_command_line_get_options_dict(command); const gchar* preview = nullptr; gboolean inspect = false;
    g_variant_dict_lookup(options, "save-preview", "&s", &preview); g_variant_dict_lookup(options, "inspect-layout", "b", &inspect);
    if (preview) return self.window && helix::apps::savePreview(GTK_WIDGET(self.window), preview) ? 0 : 1;
    if (inspect && (!self.window || self.closed)) { g_application_command_line_printerr(command, "Open Phone before inspecting its layout\n"); return 1; }
    if (inspect && self.window) {
      nlohmann::json state = {{"width", gtk_widget_get_width(GTK_WIDGET(self.window))}, {"height", gtk_widget_get_height(GTK_WIDGET(self.window))}, {"available", self.backend && self.backend->available()}, {"devices", self.backend ? self.backend->snapshot().devices.size() : 0}};
      g_application_command_line_print(command, "%s\n", state.dump().c_str()); return 0;
    }
    g_application_activate(application); return 0;
  })), &phone);
  const int result = g_application_run(G_APPLICATION(app), argc, argv); g_object_unref(app); return result;
}
