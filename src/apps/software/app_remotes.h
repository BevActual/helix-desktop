#pragma once

#include "apps/common/motion.h"
#include "system/package_spec.h"

#include <adwaita.h>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace helix::software {

inline const char* remoteScopeLabel(const noctalia::packages::FlatpakRemote& remote) {
  if (remote.installation == "user") {
    return "User";
  }
  return "System";
}

inline std::string remoteSubtitle(const noctalia::packages::FlatpakRemote& remote) {
  std::string text = remoteScopeLabel(remote);
  if (remote.disabled) {
    text += " · Disabled";
  }
  if (remote.filtered) {
    text += " · Filtered";
  }
  if (remote.oci) {
    text += " · OCI";
  }
  if (!remote.url.empty()) {
    text += "\n" + remote.url;
  }
  return text;
}

inline GtkWidget* buildRemotesPage(
    const std::vector<noctalia::packages::FlatpakRemote>& remotes, bool loading, bool canAct,
    const std::function<void()>& onRefresh, const std::function<void()>& onAddFlathub,
    const std::function<void(noctalia::packages::FlatpakRemote, noctalia::packages::FlatpakRemoteAction)>& onAction
) {
  auto* page = adw_navigation_page_new(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), "Software Sources");
  auto* toolbar = adw_toolbar_view_new();
  auto* header = adw_header_bar_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);

  auto* refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_widget_set_tooltip_text(refresh, "Refresh software sources");
  gtk_widget_set_sensitive(refresh, canAct && !loading);
  struct RefreshData {
    std::function<void()> onRefresh;
  };
  auto* refreshData = new RefreshData{onRefresh};
  g_signal_connect_data(
      refresh, "clicked",
      G_CALLBACK(+[](gpointer, gpointer user) { static_cast<RefreshData*>(user)->onRefresh(); }), refreshData,
      +[](gpointer user, GClosure*) { delete static_cast<RefreshData*>(user); }, G_CONNECT_DEFAULT
  );
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), refresh);

  auto* clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 720);
  auto* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_top(body, 18);
  gtk_widget_set_margin_bottom(body, 24);
  gtk_widget_set_margin_start(body, 18);
  gtk_widget_set_margin_end(body, 18);

  auto* intro = gtk_label_new(
      "Manage Flatpak remotes used by Explore. System remotes may ask for authentication. Changing a remote "
      "refreshes the app catalog."
  );
  gtk_label_set_wrap(GTK_LABEL(intro), true);
  gtk_label_set_xalign(GTK_LABEL(intro), 0);
  gtk_widget_add_css_class(intro, "body");
  gtk_widget_add_css_class(intro, "dim-label");
  gtk_box_append(GTK_BOX(body), intro);

  if (loading) {
    gtk_box_append(GTK_BOX(body), helix::apps::loadingDots(12));
    auto* loadingLabel = gtk_label_new("Loading software sources…");
    gtk_widget_add_css_class(loadingLabel, "dim-label");
    gtk_widget_set_halign(loadingLabel, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(body), loadingLabel);
  } else if (remotes.empty()) {
    auto* empty = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "network-server-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(empty), "No Flatpak Remotes");
    adw_status_page_set_description(
        ADW_STATUS_PAGE(empty), "Add Flathub to browse applications, or install Flatpak remotes from your distributor."
    );
    gtk_box_append(GTK_BOX(body), empty);
  } else {
    auto* group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Flatpak remotes");
    for (const auto& remote : remotes) {
      auto* row = adw_action_row_new();
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), remote.title.empty() ? remote.name.c_str() : remote.title.c_str());
      const auto subtitle = remoteSubtitle(remote);
      adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());
      adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 3);

      struct ActionData {
        noctalia::packages::FlatpakRemote remote;
        noctalia::packages::FlatpakRemoteAction action = noctalia::packages::FlatpakRemoteAction::Enable;
        std::function<void(noctalia::packages::FlatpakRemote, noctalia::packages::FlatpakRemoteAction)> onAction;
      };

      auto* toggle = gtk_switch_new();
      gtk_switch_set_active(GTK_SWITCH(toggle), !remote.disabled);
      gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
      gtk_widget_set_sensitive(toggle, canAct);
      auto* toggleData = new ActionData{
          remote,
          remote.disabled ? noctalia::packages::FlatpakRemoteAction::Enable
                          : noctalia::packages::FlatpakRemoteAction::Disable,
          onAction
      };
      g_signal_connect_data(
          toggle, "notify::active",
          G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer user) {
            auto& request = *static_cast<ActionData*>(user);
            const bool active = gtk_switch_get_active(GTK_SWITCH(object));
            const bool shouldEnable = active;
            const bool currentlyEnabled = !request.remote.disabled;
            if (shouldEnable == currentlyEnabled) {
              return;
            }
            request.action = shouldEnable ? noctalia::packages::FlatpakRemoteAction::Enable
                                          : noctalia::packages::FlatpakRemoteAction::Disable;
            // Revert visually until the backend confirms; the page will rebuild.
            g_signal_handlers_block_matched(object, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, user);
            gtk_switch_set_active(GTK_SWITCH(object), currentlyEnabled);
            g_signal_handlers_unblock_matched(object, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, user);
            request.onAction(request.remote, request.action);
          }),
          toggleData, +[](gpointer user, GClosure*) { delete static_cast<ActionData*>(user); }, G_CONNECT_DEFAULT
      );
      adw_action_row_add_suffix(ADW_ACTION_ROW(row), toggle);

      auto* remove = gtk_button_new_from_icon_name("user-trash-symbolic");
      gtk_widget_add_css_class(remove, "flat");
      gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text(remove, "Remove remote");
      gtk_widget_set_sensitive(remove, canAct);
      auto* removeData = new ActionData{remote, noctalia::packages::FlatpakRemoteAction::Delete, onAction};
      g_signal_connect_data(
          remove, "clicked",
          G_CALLBACK(+[](gpointer, gpointer user) {
            auto& request = *static_cast<ActionData*>(user);
            request.onAction(request.remote, noctalia::packages::FlatpakRemoteAction::Delete);
          }),
          removeData, +[](gpointer user, GClosure*) { delete static_cast<ActionData*>(user); }, G_CONNECT_DEFAULT
      );
      adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);
      adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(row));
    }
    gtk_box_append(GTK_BOX(body), group);
  }

  if (!noctalia::packages::hasUserFlathubRemote(remotes) || loading) {
    auto* add = gtk_button_new_with_label("Add Flathub (User)");
    gtk_widget_add_css_class(add, "suggested-action");
    gtk_widget_set_halign(add, GTK_ALIGN_START);
    gtk_widget_set_sensitive(add, canAct && !loading);
    struct AddData {
      std::function<void()> onAddFlathub;
    };
    auto* addData = new AddData{onAddFlathub};
    g_signal_connect_data(
        add, "clicked", G_CALLBACK(+[](gpointer, gpointer user) { static_cast<AddData*>(user)->onAddFlathub(); }),
        addData, +[](gpointer user, GClosure*) { delete static_cast<AddData*>(user); }, G_CONNECT_DEFAULT
    );
    gtk_box_append(GTK_BOX(body), add);
  }

  auto* note = gtk_label_new(
      "Fedora RPM repositories stay with PackageKit/DNF. Firmware sources are managed by fwupd."
  );
  gtk_label_set_wrap(GTK_LABEL(note), true);
  gtk_label_set_xalign(GTK_LABEL(note), 0);
  gtk_widget_add_css_class(note, "caption");
  gtk_widget_add_css_class(note, "dim-label");
  gtk_box_append(GTK_BOX(body), note);

  adw_clamp_set_child(ADW_CLAMP(clamp), body);
  auto* scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
  gtk_widget_set_vexpand(scroll, true);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);
  adw_navigation_page_set_child(page, toolbar);
  return GTK_WIDGET(page);
}

} // namespace helix::software
