#pragma once

#include <adwaita.h>
#include <nlohmann/json.hpp>
#include <functional>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#ifdef HELIX_HAVE_GOA
#define GOA_API_IS_SUBJECT_TO_CHANGE
#define GOA_BACKEND_API_IS_SUBJECT_TO_CHANGE
#include <goa/goa.h>
#include <goabackend/goabackend.h>
#endif

namespace helix::settings {
// GOA owns credentials and account operations. Helix hosts its native provider UI.
class AccountsPage : public std::enable_shared_from_this<AccountsPage> {
  GtkWidget* root = nullptr;
  GWeakRef parent;
  bool stopped = false, loading = false, busy = false;
  unsigned accountCount = 0, providerCount = 0;
  std::function<void(const std::string&)> toast;
#ifdef HELIX_HAVE_GOA
  GoaClient* client = nullptr;
  GCancellable* cancellation = g_cancellable_new();
  std::vector<GoaProvider*> providers;
  bool available() const {
    if (!client) return false;
    auto* manager = goa_client_get_object_manager(client);
    if (!G_IS_DBUS_OBJECT_MANAGER_CLIENT(manager)) return false;
    auto* owner = g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(manager));
    const bool present = owner && *owner; g_free(owner); return present;
  }
  void manage(GoaObject* object) {
    if (busy || !available() || stopped) return;
    auto* account = goa_object_peek_account(object);
    auto* provider = goa_provider_get_for_provider_type(goa_account_get_provider_type(account));
    if (!provider) { if (toast) toast("This account provider is unavailable."); return; }
    auto* window = GTK_WIDGET(g_weak_ref_get(&parent));
    if (!window) { g_object_unref(provider); return; }
    busy = true; gtk_widget_set_sensitive(root, false);
    goa_provider_show_account(provider, client, object, window, cancellation,
      +[](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<std::shared_ptr<AccountsPage>> owner(static_cast<std::shared_ptr<AccountsPage>*>(data));
        auto& self = **owner; GError* error = nullptr;
        const bool ok = goa_provider_show_account_finish(GOA_PROVIDER(source), result, &error);
        if (!self.stopped && !ok && error && !g_error_matches(error, GOA_ERROR, GOA_ERROR_DIALOG_DISMISSED) &&
            !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && self.toast) self.toast("Account settings could not be opened. Retry the connection.");
        if (error) g_error_free(error);
        self.busy = false; if (!self.stopped) self.render();
      }, new std::shared_ptr<AccountsPage>(shared_from_this()));
    g_object_unref(provider); g_object_unref(window);
  }
  void add(GoaProvider* provider) {
    if (busy || !available() || stopped) return;
    auto* window = GTK_WIDGET(g_weak_ref_get(&parent)); if (!window) return;
    busy = true; gtk_widget_set_sensitive(root, false);
    goa_provider_add_account(provider, client, window, cancellation,
      +[](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<std::shared_ptr<AccountsPage>> owner(static_cast<std::shared_ptr<AccountsPage>*>(data));
        auto& self = **owner; GError* error = nullptr;
        auto* object = goa_provider_add_account_finish(GOA_PROVIDER(source), result, &error);
        if (object) g_object_unref(object);
        if (!self.stopped && error && !g_error_matches(error, GOA_ERROR, GOA_ERROR_DIALOG_DISMISSED) &&
            !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && self.toast) self.toast("The account could not be added. Retry sign-in.");
        if (error) g_error_free(error);
        self.busy = false; if (!self.stopped) self.render();
      }, new std::shared_ptr<AccountsPage>(shared_from_this()));
    g_object_unref(window);
  }
#else
  bool available() const { return false; }
#endif
  GtkWidget* action(AdwActionRow* row, const char* label, std::function<void()> callback) {
    auto* button = gtk_button_new_with_label(label); gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    g_signal_connect_data(button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
      (*static_cast<std::function<void()>*>(data))();
    }), new std::function<void()>(std::move(callback)), +[](gpointer data, GClosure*) {
      delete static_cast<std::function<void()>*>(data);
    }, G_CONNECT_DEFAULT);
    adw_action_row_add_suffix(row, button); adw_action_row_set_activatable_widget(row, button); return button;
  }
  AdwPreferencesGroup* group(const char* name) {
    auto* fields = adw_preferences_group_new(); adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(fields), name);
    gtk_box_append(GTK_BOX(root), fields); return ADW_PREFERENCES_GROUP(fields);
  }
  AdwActionRow* row(AdwPreferencesGroup* group, const char* name, const char* subtitle = "") {
    auto* item = adw_action_row_new(); adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(item), false);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(item), name); adw_action_row_set_subtitle(ADW_ACTION_ROW(item), subtitle);
    adw_action_row_set_title_lines(ADW_ACTION_ROW(item), 2); adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(item), 2);
    adw_preferences_group_add(group, item); return ADW_ACTION_ROW(item);
  }
  void render() {
    if (stopped) return;
    while (auto* child = gtk_widget_get_first_child(root)) gtk_box_remove(GTK_BOX(root), child);
    gtk_widget_set_sensitive(root, !busy);
    accountCount = 0; providerCount = 0;
    if (loading || !available()) {
      auto* status = adw_status_page_new(); adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), "goa-panel-symbolic");
      adw_status_page_set_title(ADW_STATUS_PAGE(status), loading ? "Connecting to Online Accounts" : "Online Accounts Unavailable");
#ifdef HELIX_HAVE_GOA
      adw_status_page_set_description(ADW_STATUS_PAGE(status), "GNOME Online Accounts manages sign-in and credentials. You can retry when its service is available.");
      if (!loading) {
        auto* fields = group("Connection"); auto* retry = row(fields, "Reconnect to Online Accounts"); action(retry, "Retry", [this] { load(); });
      }
#else
      adw_status_page_set_description(ADW_STATUS_PAGE(status), "This Settings build needs GNOME Online Accounts support to manage accounts.");
#endif
      gtk_box_prepend(GTK_BOX(root), status); return;
    }
#ifdef HELIX_HAVE_GOA
    auto* accounts = group("Your Accounts");
    auto* entries = goa_client_get_accounts(client);
    for (auto* entry = entries; entry; entry = entry->next) {
      auto* object = GOA_OBJECT(entry->data); auto* account = goa_object_peek_account(object); if (!account) continue;
      ++accountCount;
      const auto subtitle = std::string(goa_account_get_attention_needed(account) ? "Sign-in required · " : "") + goa_account_get_presentation_identity(account);
      auto* item = row(accounts, goa_account_get_provider_name(account), subtitle.c_str());
      auto* icon = g_icon_new_for_string(goa_account_get_provider_icon(account), nullptr);
      if (icon) { adw_action_row_add_prefix(item, gtk_image_new_from_gicon(icon)); g_object_unref(icon); }
      // The row owns a reference until it is removed, including while provider UI opens.
      g_object_set_data_full(G_OBJECT(item), "goa-object", g_object_ref(object), g_object_unref);
      action(item, "Manage", [this, object] { manage(object); });
    }
    g_list_free_full(entries, g_object_unref);
    if (!accountCount) row(accounts, "No accounts connected", "Add an account to use its supported services in compatible Linux applications.");
    auto* choices = group("Add an Account");
    adw_preferences_group_set_description(choices, "Sign in using the provider's authentication flow. You can choose enabled services and remove accounts from Manage.");
    for (auto* provider : providers) {
      auto* name = goa_provider_get_provider_name(provider, nullptr);
      auto* item = row(choices, name ? name : "Account provider"); g_free(name);
      auto* icon = goa_provider_get_provider_icon(provider, nullptr);
      if (icon) { adw_action_row_add_prefix(item, gtk_image_new_from_gicon(icon)); g_object_unref(icon); }
      action(item, "Add", [this, provider] { add(provider); }); ++providerCount;
    }
    if (!providerCount) row(choices, "No providers available", "Provider availability is controlled by GNOME Online Accounts.");
#endif
  }
  void load() {
    if (loading || stopped) return;
#ifdef HELIX_HAVE_GOA
    loading = true; render();
    if (client) {
      g_signal_handlers_disconnect_by_data(client, this);
      g_signal_handlers_disconnect_by_data(goa_client_get_object_manager(client), this); g_clear_object(&client);
    }
    for (auto* provider : providers) g_object_unref(provider);
    providers.clear();
    goa_client_new(cancellation, +[](GObject*, GAsyncResult* result, gpointer data) {
      std::unique_ptr<std::shared_ptr<AccountsPage>> owner(static_cast<std::shared_ptr<AccountsPage>*>(data));
      auto& self = **owner; GError* error = nullptr; auto* client = goa_client_new_finish(result, &error);
      if (error) g_error_free(error);
      if (self.stopped) { if (client) g_object_unref(client); return; }
      self.client = client;
      if (!client) { self.loading = false; self.render(); return; }
      for (const char* signal : {"account-added", "account-removed", "account-changed"})
        g_signal_connect(client, signal, G_CALLBACK(+[](GoaClient*, GoaObject*, gpointer data) { static_cast<AccountsPage*>(data)->render(); }), &self);
      g_signal_connect(goa_client_get_object_manager(client), "notify::name-owner", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) { static_cast<AccountsPage*>(data)->render(); }), &self);
      goa_provider_get_all(+[](GObject*, GAsyncResult* result, gpointer data) {
        std::unique_ptr<std::shared_ptr<AccountsPage>> owner(static_cast<std::shared_ptr<AccountsPage>*>(data));
        auto& self = **owner; GList* providers = nullptr; GError* error = nullptr;
        if (goa_provider_get_all_finish(&providers, result, &error)) {
          for (auto* entry = providers; entry; entry = entry->next) self.providers.push_back(GOA_PROVIDER(entry->data));
          g_list_free(providers);
          std::stable_sort(self.providers.begin(), self.providers.end(), [](GoaProvider* left, GoaProvider* right) {
            const bool leftBranded = goa_provider_get_provider_features(left) & GOA_PROVIDER_FEATURE_BRANDED;
            const bool rightBranded = goa_provider_get_provider_features(right) & GOA_PROVIDER_FEATURE_BRANDED;
            if (leftBranded != rightBranded) return leftBranded;
            auto* leftName = goa_provider_get_provider_name(left, nullptr);
            auto* rightName = goa_provider_get_provider_name(right, nullptr);
            const bool before = g_utf8_collate(leftName ? leftName : "", rightName ? rightName : "") < 0;
            g_free(leftName); g_free(rightName); return before;
          });

        }
        if (error) g_error_free(error);
        self.loading = false; if (!self.stopped) self.render();
      }, new std::shared_ptr<AccountsPage>(self.shared_from_this()));
    }, new std::shared_ptr<AccountsPage>(shared_from_this()));
#else
    render();
#endif
  }
public:
  explicit AccountsPage(GtkWindow* window, std::function<void(const std::string&)> feedback) : toast(std::move(feedback)) {
    g_weak_ref_init(&parent, G_OBJECT(window)); root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24); g_object_ref_sink(root);
  }
  ~AccountsPage() {
    shutdown();
#ifdef HELIX_HAVE_GOA
    if (client) { g_signal_handlers_disconnect_by_data(client, this); g_signal_handlers_disconnect_by_data(goa_client_get_object_manager(client), this); g_object_unref(client); }
    for (auto* provider : providers) g_object_unref(provider);
    g_object_unref(cancellation);
#endif
    g_weak_ref_clear(&parent); g_object_unref(root);
  }
  void shutdown() {
    stopped = true; toast = {}; gtk_widget_set_sensitive(root, false);
#ifdef HELIX_HAVE_GOA
    g_cancellable_cancel(cancellation);
#endif
  }
  static std::shared_ptr<AccountsPage> create(GtkWindow* window, std::function<void(const std::string&)> feedback) {
    auto self = std::make_shared<AccountsPage>(window, std::move(feedback)); self->load(); return self;
  }
  GtkWidget* widget() const { return root; }
  nlohmann::json inspect() const {
    return {{"available", available()}, {"loading", loading}, {"busy", busy}, {"accounts", accountCount}, {"providers", providerCount}};
  }
};
}
