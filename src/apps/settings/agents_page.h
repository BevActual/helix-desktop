#pragma once

#include "desktop/client.h"
#include "desktop/display_text.h"

#include <adwaita.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace helix::settings {

// Lists the apps that may use the desktop through Agent1, lets the user change or remove their access and shows
// recent activity. The service enforces everything; this page is the manager role: it registers with the service
// and only reads and edits what the service reports. Every string shown here can be influenced by an agent (client
// names, tool arguments), so it is sanitized and never interpreted as markup.
class AgentsPage : public std::enable_shared_from_this<AgentsPage> {
  using Json = nlohmann::json;

  struct Tier { const char* id; const char* title; const char* detail; };
  static constexpr Tier kTiers[] = {
    {"observe", "See the desktop", "Status, workspaces and settings"},
    {"observe-private", "Read private content", "Clipboard, screen, notifications and window titles"},
    {"act", "Change settings", "Open panels, adjust volume, brightness and appearance"},
    {"confirm", "Ask to do more", "Launch apps, close windows and similar. You are asked each time"},
  };
  static constexpr unsigned kAuditRows = 50;

  GtkWidget* root = nullptr;
  std::function<void(const std::string&)> toast;
  bool stopped = false, registered = false, loaded = false;
  std::string unavailable;
  std::vector<Json> grants, audit;
  unsigned observer = 0;

  static std::string shown(const std::string& text, std::size_t limit = 160) { return agent::sanitizeForDisplay(text, limit); }

  static std::string friendlyName(const std::string& client) { return agent::friendlyClientName(client); }

  static GVariant* tierList(const std::string& client, const std::vector<std::string>& tiers) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
    for (const auto& tier : tiers) g_variant_builder_add(&builder, "s", tier.c_str());
    return g_variant_new("(sas)", client.c_str(), &builder);
  }

  static void styled(AdwPreferencesRow* row, const std::string& value) {
    adw_preferences_row_set_use_markup(row, false);
    adw_preferences_row_set_title(row, value.c_str());
  }

  AdwActionRow* textRow(AdwPreferencesGroup* group, const std::string& title, const std::string& subtitle) {
    auto* row = ADW_ACTION_ROW(adw_action_row_new());
    styled(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(row, subtitle.c_str());
    adw_action_row_set_title_lines(row, 2);
    adw_action_row_set_subtitle_lines(row, 3);
    adw_preferences_group_add(group, GTK_WIDGET(row));
    return row;
  }

  void clear() {
    while (auto* child = gtk_widget_get_first_child(root)) gtk_box_remove(GTK_BOX(root), child);
  }

  // Calls the service, re-registering as manager once if the service restarted since we last registered.
  // Each call owns one reference to `parameters` and releases it on its single completion path.
  void call(const char* method, GVariant* parameters, std::function<void(Json, std::string)> done, bool retry = true) {
    GVariant* owned = parameters ? g_variant_ref_sink(parameters) : nullptr;
    desktopClient().callAsync("agent", method, owned,
      [weak = weak_from_this(), method, owned, done = std::move(done), retry](Json value, std::string error) mutable {
        auto release = [&owned] { if (owned) { g_variant_unref(owned); owned = nullptr; } };
        auto self = weak.lock();
        if (!self || self->stopped) { release(); return; }
        if (!error.empty() && retry && error.find("registered manager") != std::string::npos) {
          GVariant* again = owned ? g_variant_ref(owned) : nullptr;  // the retry owns its own reference
          release();
          desktopClient().callAsync("agent", "RegisterManager", nullptr,
            [weak, method, again, done = std::move(done)](Json, std::string registerError) mutable {
              auto page = weak.lock();
              if (!page || page->stopped) { if (again) g_variant_unref(again); return; }
              if (!registerError.empty()) { if (again) g_variant_unref(again); done(Json::object(), registerError); return; }
              page->call(method, again, std::move(done), false);
            });
          return;
        }
        release();
        done(std::move(value), std::move(error));
      });
  }

  void load() {
    desktopClient().callAsync("agent", "RegisterManager", nullptr, [weak = weak_from_this()](Json, std::string error) {
      auto self = weak.lock();
      if (!self || self->stopped) return;
      self->loaded = true;
      if (!error.empty()) {
        self->registered = false;
        self->unavailable = error;
        self->render();
        return;
      }
      self->registered = true;
      self->unavailable.clear();
      self->observer = desktopClient().observe([weak](std::string_view interface, std::string_view signal, const Json&) {
        if (interface != agentInterface || signal != "Changed") return;
        if (auto page = weak.lock(); page && !page->stopped) page->refresh();
      });
      self->refresh();
    });
    render();
  }

  void refresh() {
    call("GetGrants", nullptr, [weak = weak_from_this()](Json value, std::string error) {
      auto self = weak.lock();
      if (!self || self->stopped) return;
      if (!error.empty()) { self->unavailable = error; self->render(); return; }
      std::vector<Json> next;
      if (value.is_array()) for (const auto& entry : value) next.push_back(entry);
      self->call("GetAudit", g_variant_new("(tu)", static_cast<guint64>(0), static_cast<guint32>(kAuditRows)),
        [weak, next = std::move(next)](Json records, std::string auditError) mutable {
          auto page = weak.lock();
          if (!page || page->stopped) return;
          std::vector<Json> log;
          if (auditError.empty() && records.is_array()) for (const auto& record : records) log.push_back(record);
          // Unchanged data must not rebuild the page: that would collapse a row the user has open.
          if (page->unavailable.empty() && next == page->grants && log == page->audit) return;
          page->unavailable.clear();
          page->grants = std::move(next);
          page->audit = std::move(log);
          page->render();
        });
    });
  }

  void setTier(const std::string& client, const std::string& tier, bool on) {
    auto current = std::find_if(grants.begin(), grants.end(), [&](const Json& g) { return g.value("client", "") == client; });
    std::vector<std::string> tiers;
    if (current != grants.end() && current->contains("tiers"))
      for (const auto& item : current->at("tiers")) if (item.is_string()) tiers.push_back(item.get<std::string>());
    tiers.erase(std::remove(tiers.begin(), tiers.end(), tier), tiers.end());
    if (on) tiers.push_back(tier);
    call("SetGrant", tierList(client, tiers), [weak = weak_from_this()](Json, std::string error) {
      auto self = weak.lock();
      if (!self || self->stopped) return;
      if (!error.empty() && self->toast) self->toast("That change could not be saved.");
      self->refresh();
    });
  }

  void remove(const std::string& client) {
    call("RevokeClient", g_variant_new("(s)", client.c_str()), [weak = weak_from_this()](Json, std::string error) {
      auto self = weak.lock();
      if (!self || self->stopped) return;
      if (self->toast) self->toast(error.empty() ? "Access removed." : "Access could not be removed.");
      self->refresh();
    });
  }

  void render() {
    if (stopped) return;
    clear();
    if (!unavailable.empty() || !registered) {
      auto* group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
      adw_preferences_group_set_title(group, "Agents");
      gtk_box_append(GTK_BOX(root), GTK_WIDGET(group));
      textRow(group, loaded ? "App access is unavailable" : "Loading",
              loaded ? shown(unavailable.empty() ? "The desktop service does not offer app access yet." : unavailable, 240) : "");
      return;
    }

    auto* access = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(access, "Apps with access");
    adw_preferences_group_set_description(access,
      "Apps are asked about the first time they try to use your desktop. Switch off what an app should no longer do, or remove it.");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(access));
    if (grants.empty()) textRow(access, "No apps have access", "Nothing can use your desktop on an app's behalf.");

    for (const auto& grant : grants) {
      const std::string client = grant.value("client", "");
      std::vector<std::string> held;
      if (grant.contains("tiers")) for (const auto& item : grant.at("tiers")) if (item.is_string()) held.push_back(item.get<std::string>());
      auto* expander = ADW_EXPANDER_ROW(adw_expander_row_new());
      styled(ADW_PREFERENCES_ROW(expander), friendlyName(client));
      adw_expander_row_set_subtitle(expander, held.empty() ? "No access" : (std::to_string(held.size()) + " of 4 allowed").c_str());
      adw_preferences_group_add(access, GTK_WIDGET(expander));

      auto* identity = ADW_ACTION_ROW(adw_action_row_new());
      styled(ADW_PREFERENCES_ROW(identity), "Identified as");
      adw_action_row_set_subtitle(identity, shown(client, 240).c_str());
      adw_action_row_set_subtitle_selectable(identity, true);
      adw_expander_row_add_row(expander, GTK_WIDGET(identity));

      for (const auto& tier : kTiers) {
        auto* row = ADW_SWITCH_ROW(adw_switch_row_new());
        styled(ADW_PREFERENCES_ROW(row), tier.title);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), tier.detail);
        adw_switch_row_set_active(row, std::find(held.begin(), held.end(), tier.id) != held.end());
        g_signal_connect_data(row, "notify::active", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
          auto& request = *static_cast<std::function<void(bool)>*>(data);
          request(adw_switch_row_get_active(ADW_SWITCH_ROW(object)));
        }), new std::function<void(bool)>([weak = weak_from_this(), client, id = std::string(tier.id), initial = adw_switch_row_get_active(row)](bool on) mutable {
          if (on == initial) return;  // programmatic initialisation, not a user change
          initial = on;
          if (auto self = weak.lock(); self && !self->stopped) self->setTier(client, id, on);
        }), +[](gpointer data, GClosure*) { delete static_cast<std::function<void(bool)>*>(data); }, G_CONNECT_DEFAULT);
        adw_expander_row_add_row(expander, GTK_WIDGET(row));
      }

      auto* removal = ADW_ACTION_ROW(adw_action_row_new());
      styled(ADW_PREFERENCES_ROW(removal), "Remove access");
      adw_action_row_set_subtitle(removal, "It will be asked again the next time it tries to use your desktop.");
      auto* button = gtk_button_new_with_label("Remove");
      gtk_widget_add_css_class(button, "destructive-action");
      gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
      g_signal_connect_data(button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        (*static_cast<std::function<void()>*>(data))();
      }), new std::function<void()>([weak = weak_from_this(), client] {
        if (auto self = weak.lock(); self && !self->stopped) self->remove(client);
      }), +[](gpointer data, GClosure*) { delete static_cast<std::function<void()>*>(data); }, G_CONNECT_DEFAULT);
      adw_action_row_add_suffix(removal, button);
      adw_expander_row_add_row(expander, GTK_WIDGET(removal));
    }

    auto* activity = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(activity, "Recent activity");
    adw_preferences_group_set_description(activity, "What apps asked to do, and what happened.");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(activity));
    if (audit.empty()) textRow(activity, "Nothing yet", "Requests from apps appear here.");
    unsigned shownRows = 0;
    for (auto it = audit.rbegin(); it != audit.rend() && shownRows < kAuditRows; ++it, ++shownRows) {
      std::string what = it->value("tool", "");
      if (it->contains("args")) for (const auto& arg : it->at("args")) if (arg.is_string()) what += " " + arg.get<std::string>();
      std::string detail = shown(it->value("time", ""), 32) + " · " + friendlyName(it->value("client", "")) + " · " + shown(it->value("decision", ""), 24);
      if (!it->value("outcome", "").empty() && it->value("decision", "") != "granted") detail += " (" + shown(it->value("outcome", ""), 60) + ")";
      if (it->value("tainted", false)) detail += " · after reading private content";
      textRow(activity, shown(what), detail);
    }
  }

public:
  explicit AgentsPage(std::function<void(const std::string&)> feedback) : toast(std::move(feedback)) {
    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
    g_object_ref_sink(root);
  }
  ~AgentsPage() {
    shutdown();
    g_object_unref(root);
  }
  void shutdown() {
    stopped = true;
    toast = {};
    if (observer) { desktopClient().unobserve(observer); observer = 0; }
  }
  static std::shared_ptr<AgentsPage> create(std::function<void(const std::string&)> feedback) {
    auto self = std::make_shared<AgentsPage>(std::move(feedback));
    self->load();
    return self;
  }
  GtkWidget* widget() const { return root; }
  Json inspect() const {
    return {{"available", registered && unavailable.empty()}, {"loaded", loaded}, {"clients", grants.size()},
            {"audit", audit.size()}, {"unavailable", unavailable}};
  }
};

}  // namespace helix::settings
