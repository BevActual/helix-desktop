#pragma once

#include "apps/common/motion.h"
#include "apps/software/app_grid.h"
#include "apps/software/app_store.h"
#include "system/package_kit_service.h"
#include "ui/style.h"

#include <adwaita.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <tuple>

namespace helix::software {

inline void clearBox(GtkBox* box) {
  while (auto* child = gtk_widget_get_first_child(GTK_WIDGET(box))) {
    gtk_box_remove(box, child);
  }
}

inline GtkWidget* metaRow(const char* title, const std::string& value) {
  if (value.empty()) {
    return nullptr;
  }
  auto* row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), false);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), value.c_str());
  adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 4);
  return row;
}

inline void appendMeta(AdwPreferencesGroup* group, const char* title, const std::string& value) {
  if (auto* row = metaRow(title, value)) {
    adw_preferences_group_add(group, row);
  }
}

inline std::string friendlySize(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  bool digits = true;
  for (const char ch : value) {
    if (ch < '0' || ch > '9') {
      digits = false;
      break;
    }
  }
  if (!digits) {
    return value;
  }
  try {
    const auto bytes = std::stoull(value);
    char buffer[32];
    if (bytes >= 1024ull * 1024ull * 1024ull) {
      std::snprintf(buffer, sizeof(buffer), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
      return buffer;
    }
    if (bytes >= 1024ull * 1024ull) {
      std::snprintf(buffer, sizeof(buffer), "%.1f MB", bytes / (1024.0 * 1024.0));
      return buffer;
    }
    if (bytes >= 1024ull) {
      std::snprintf(buffer, sizeof(buffer), "%.0f KB", bytes / 1024.0);
      return buffer;
    }
    return value + " B";
  } catch (...) {
    return value;
  }
}

inline void appendLinkMeta(AdwPreferencesGroup* group, const char* title, const std::string& url) {
  if (url.empty() || !(url.starts_with("https://") || url.starts_with("http://"))) {
    appendMeta(group, title, url);
    return;
  }
  auto* row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), false);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), url.c_str());
  adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 2);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), true);
  auto* icon = gtk_image_new_from_icon_name("external-link-symbolic");
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), icon);
  auto* owned = new std::string(url);
  g_signal_connect_data(
      row, "activated",
      G_CALLBACK(+[](gpointer, gpointer user) {
        const auto& link = *static_cast<std::string*>(user);
        g_app_info_launch_default_for_uri(link.c_str(), nullptr, nullptr);
      }),
      owned, +[](gpointer user, GClosure*) { delete static_cast<std::string*>(user); }, G_CONNECT_DEFAULT
  );
  adw_preferences_group_add(group, row);
}

inline void loadRemoteScreenshot(GtkBox* host, const std::string& url) {
  auto* picture = gtk_picture_new();
  gtk_picture_set_can_shrink(GTK_PICTURE(picture), true);
  gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_size_request(picture, -1, 260);
  gtk_widget_set_hexpand(picture, true);
  auto* spinner = gtk_spinner_new();
  gtk_spinner_start(GTK_SPINNER(spinner));
  gtk_widget_set_size_request(spinner, -1, 260);
  gtk_widget_set_hexpand(spinner, true);
  auto* stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration(GTK_STACK(stack), Style::animNormal);
  gtk_stack_add_named(GTK_STACK(stack), spinner, "loading");
  gtk_stack_add_named(GTK_STACK(stack), picture, "image");
  gtk_stack_set_visible_child_name(GTK_STACK(stack), "loading");
  auto* frame = gtk_frame_new(nullptr);
  gtk_widget_add_css_class(frame, "card");
  gtk_frame_set_child(GTK_FRAME(frame), stack);
  gtk_box_append(host, frame);

  auto* file = g_file_new_for_uri(url.c_str());
  struct Work {
    GtkWidget* picture = nullptr;
    GtkWidget* stack = nullptr;
    GtkWidget* frame = nullptr;
  };
  auto* work = new Work{picture, stack, frame};
  g_object_ref(picture);
  g_object_ref(stack);
  g_object_ref(frame);
  g_file_read_async(
      file, G_PRIORITY_DEFAULT, nullptr,
      +[](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Work> work(static_cast<Work*>(data));
        GError* error = nullptr;
        auto* stream = g_file_read_finish(G_FILE(source), result, &error);
        if (!stream) {
          if (error) {
            g_error_free(error);
          }
          gtk_widget_set_visible(work->frame, false);
          g_object_unref(work->picture);
          g_object_unref(work->stack);
          g_object_unref(work->frame);
          return;
        }
        auto* pixbuf = gdk_pixbuf_new_from_stream_at_scale(G_INPUT_STREAM(stream), 960, 540, true, nullptr, &error);
        g_object_unref(stream);
        if (!pixbuf) {
          if (error) {
            g_error_free(error);
          }
          gtk_widget_set_visible(work->frame, false);
          g_object_unref(work->picture);
          g_object_unref(work->stack);
          g_object_unref(work->frame);
          return;
        }
        auto* pixels = gdk_pixbuf_read_pixel_bytes(pixbuf);
        auto* texture = gdk_memory_texture_new(
            gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf),
            gdk_pixbuf_get_has_alpha(pixbuf) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8, pixels,
            gdk_pixbuf_get_rowstride(pixbuf)
        );
        g_bytes_unref(pixels);
        gtk_picture_set_paintable(GTK_PICTURE(work->picture), GDK_PAINTABLE(texture));
        g_object_unref(texture);
        g_object_unref(pixbuf);
        gtk_stack_set_visible_child_name(GTK_STACK(work->stack), "image");
        g_object_unref(work->picture);
        g_object_unref(work->stack);
        g_object_unref(work->frame);
      },
      work
  );
  g_object_unref(file);
}

inline AdwNavigationPage* buildDetailPage(
    const noctalia::packages::PackageDetails& details, noctalia::packages::Kind kind,
    const std::function<void(noctalia::packages::PackageHit, noctalia::packages::Action)>& onAction,
    const std::function<void(const noctalia::packages::PackageHit&)>& onLaunch = {}, bool actionsEnabled = true,
    bool launchable = true
) {
  const auto& hit = details.hit;
  const auto title = hit.title.empty() ? hit.name : hit.title;
  auto* pageBody = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
  for (auto side : {"margin-start", "margin-end", "margin-top", "margin-bottom"}) {
    g_object_set(pageBody, side, 8, nullptr);
  }

  // Calm hero: icon, identity, and one primary action — native buttons, not store chrome.
  auto* hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
  gtk_widget_set_margin_bottom(hero, 4);
  gtk_box_append(GTK_BOX(hero), packageIcon(hit, kind, 128));
  auto* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_hexpand(text, true);
  gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
  auto* titleLabel = gtk_label_new(title.c_str());
  gtk_label_set_wrap(GTK_LABEL(titleLabel), true);
  gtk_label_set_xalign(GTK_LABEL(titleLabel), 0);
  gtk_widget_add_css_class(titleLabel, "title-1");
  gtk_box_append(GTK_BOX(text), titleLabel);
  std::string subtitle;
  if (!details.developer.empty()) {
    subtitle = details.developer;
  } else if (!details.categories.empty()) {
    subtitle = noctalia::packages::categoryDisplayName(details.categories.front());
  } else if (!hit.summary.empty()) {
    subtitle = hit.summary;
  } else if (kind == noctalia::packages::Kind::Flatpak) {
    subtitle = "App";
  } else if (kind == noctalia::packages::Kind::Fedora) {
    subtitle = "System Package";
  }
  if (!subtitle.empty()) {
    auto* summary = gtk_label_new(subtitle.c_str());
    gtk_label_set_wrap(GTK_LABEL(summary), true);
    gtk_label_set_lines(GTK_LABEL(summary), 2);
    gtk_label_set_ellipsize(GTK_LABEL(summary), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(summary), 0);
    gtk_widget_add_css_class(summary, "body");
    gtk_widget_add_css_class(summary, "dim-label");
    gtk_box_append(GTK_BOX(text), summary);
  }
  if (!details.verification.empty()) {
    auto* verified = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(verified), gtk_image_new_from_icon_name("emblem-ok-symbolic"));
    auto* verifiedLabel = gtk_label_new(details.verification.c_str());
    gtk_label_set_xalign(GTK_LABEL(verifiedLabel), 0);
    gtk_widget_add_css_class(verifiedLabel, "caption");
    gtk_widget_add_css_class(verifiedLabel, "accent");
    gtk_box_append(GTK_BOX(verified), verifiedLabel);
    gtk_box_append(GTK_BOX(text), verified);
  }
  if (const auto safety = permissionSafetySummary(details.permissions); !safety.empty()) {
    auto* safetyLabel = gtk_label_new(safety.c_str());
    gtk_label_set_xalign(GTK_LABEL(safetyLabel), 0);
    gtk_widget_add_css_class(safetyLabel, "caption");
    gtk_widget_add_css_class(safetyLabel, "dim-label");
    gtk_box_append(GTK_BOX(text), safetyLabel);
  }
  gtk_box_append(GTK_BOX(hero), text);

  auto* actionCol = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_valign(actionCol, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(actionCol, GTK_ALIGN_END);
  const auto primaryAction = hit.state == noctalia::packages::PackageState::Update
      ? noctalia::packages::Action::Update
      : hit.state == noctalia::packages::PackageState::Installed ? noctalia::packages::Action::Remove
                                                                : noctalia::packages::Action::Install;
  const bool canAct =
      !(kind == noctalia::packages::Kind::Firmware && hit.state != noctalia::packages::PackageState::Update);
  const bool canLaunch =
      onLaunch && launchable && kind != noctalia::packages::Kind::Firmware
      && (hit.state == noctalia::packages::PackageState::Installed || hit.state == noctalia::packages::PackageState::Update);

  auto wireAction = [&](GtkWidget* button, noctalia::packages::Action act) {
    const bool locked = kind == noctalia::packages::Kind::Fedora && act == noctalia::packages::Action::Remove
        && noctalia::packages::isProtectedPackage(hit.name);
    if (locked) {
      gtk_button_set_label(GTK_BUTTON(button), "Protected");
      gtk_widget_set_sensitive(button, false);
      gtk_widget_set_tooltip_text(button, "This system package is protected.");
      return;
    }
    if (!actionsEnabled) {
      gtk_widget_set_sensitive(button, false);
      gtk_widget_set_tooltip_text(button, "Wait for the current software operation to finish.");
      return;
    }
    struct ActionData {
      noctalia::packages::PackageHit hit;
      noctalia::packages::Action action = noctalia::packages::Action::Install;
      std::function<void(noctalia::packages::PackageHit, noctalia::packages::Action)> onAction;
    };
    auto* data = new ActionData{hit, act, onAction};
    g_signal_connect_data(
        button, "clicked",
        G_CALLBACK(+[](gpointer, gpointer user) {
          auto& request = *static_cast<ActionData*>(user);
          request.onAction(request.hit, request.action);
        }),
        data, +[](gpointer user, GClosure*) { delete static_cast<ActionData*>(user); }, G_CONNECT_DEFAULT
    );
  };

  if (canLaunch) {
    auto* launch = gtk_button_new_with_label("Open");
    gtk_widget_add_css_class(launch, "pill");
    gtk_widget_add_css_class(launch, "suggested-action");
    gtk_widget_set_sensitive(launch, actionsEnabled);
    struct LaunchData {
      noctalia::packages::PackageHit hit;
      std::function<void(const noctalia::packages::PackageHit&)> onLaunch;
    };
    auto* data = new LaunchData{hit, onLaunch};
    g_signal_connect_data(
        launch, "clicked",
        G_CALLBACK(+[](gpointer, gpointer user) {
          auto& request = *static_cast<LaunchData*>(user);
          request.onLaunch(request.hit);
        }),
        data, +[](gpointer user, GClosure*) { delete static_cast<LaunchData*>(user); }, G_CONNECT_DEFAULT
    );
    gtk_box_append(GTK_BOX(actionCol), launch);
  }
  if (canAct) {
    if (primaryAction == noctalia::packages::Action::Update) {
      auto* update = gtk_button_new_with_label("Update");
      gtk_widget_add_css_class(update, "pill");
      if (!canLaunch) gtk_widget_add_css_class(update, "suggested-action");
      wireAction(update, noctalia::packages::Action::Update);
      gtk_box_append(GTK_BOX(actionCol), update);
      if (kind != noctalia::packages::Kind::Firmware) {
        auto* remove = gtk_button_new_with_label("Remove");
        gtk_widget_add_css_class(remove, "pill");
        gtk_widget_add_css_class(remove, "flat");
        wireAction(remove, noctalia::packages::Action::Remove);
        gtk_box_append(GTK_BOX(actionCol), remove);
      }
    } else if (primaryAction == noctalia::packages::Action::Install) {
      auto* install = gtk_button_new_with_label("Install");
      gtk_widget_add_css_class(install, "pill");
      gtk_widget_add_css_class(install, "suggested-action");
      wireAction(install, noctalia::packages::Action::Install);
      gtk_box_append(GTK_BOX(actionCol), install);
    } else {
      auto* remove = gtk_button_new_with_label("Remove");
      gtk_widget_add_css_class(remove, "pill");
      gtk_widget_add_css_class(remove, "destructive-action");
      wireAction(remove, noctalia::packages::Action::Remove);
      gtk_box_append(GTK_BOX(actionCol), remove);
    }
  }
  gtk_box_append(GTK_BOX(hero), actionCol);
  gtk_box_append(GTK_BOX(pageBody), hero);

  if (!details.screenshotUrls.empty()) {
    auto* shots = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    auto* shotsLabel = gtk_label_new("Preview");
    gtk_label_set_xalign(GTK_LABEL(shotsLabel), 0);
    gtk_widget_add_css_class(shotsLabel, "heading");
    gtk_box_append(GTK_BOX(shots), shotsLabel);
    auto* carousel = adw_carousel_new();
    adw_carousel_set_allow_mouse_drag(ADW_CAROUSEL(carousel), true);
    adw_carousel_set_allow_scroll_wheel(ADW_CAROUSEL(carousel), true);
    adw_carousel_set_reveal_duration(ADW_CAROUSEL(carousel), Style::animSlow);
    for (std::size_t i = 0; i < std::min<std::size_t>(details.screenshotUrls.size(), 6); ++i) {
      auto* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_set_hexpand(page, true);
      loadRemoteScreenshot(GTK_BOX(page), details.screenshotUrls[i]);
      adw_carousel_append(ADW_CAROUSEL(carousel), page);
    }
    gtk_widget_set_size_request(carousel, -1, 300);
    gtk_box_append(GTK_BOX(shots), carousel);
    if (details.screenshotUrls.size() > 1) {
      auto* dots = adw_carousel_indicator_dots_new();
      adw_carousel_indicator_dots_set_carousel(ADW_CAROUSEL_INDICATOR_DOTS(dots), ADW_CAROUSEL(carousel));
      gtk_widget_set_halign(dots, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(shots), dots);
    }
    gtk_box_append(GTK_BOX(pageBody), shots);
  }

  if (!details.releaseNotes.empty()) {
    auto* notesLabel = gtk_label_new(
        hit.state == noctalia::packages::PackageState::Update ? "What's New" : "Latest Release"
    );
    gtk_label_set_xalign(GTK_LABEL(notesLabel), 0);
    gtk_widget_add_css_class(notesLabel, "heading");
    gtk_box_append(GTK_BOX(pageBody), notesLabel);
    auto* notes = gtk_label_new(details.releaseNotes.c_str());
    gtk_label_set_wrap(GTK_LABEL(notes), true);
    gtk_label_set_xalign(GTK_LABEL(notes), 0);
    gtk_label_set_selectable(GTK_LABEL(notes), true);
    gtk_widget_add_css_class(notes, "body");
    gtk_box_append(GTK_BOX(pageBody), notes);
  }

  if (!details.description.empty()) {
    auto* descLabel = gtk_label_new("Description");
    gtk_label_set_xalign(GTK_LABEL(descLabel), 0);
    gtk_widget_add_css_class(descLabel, "heading");
    gtk_box_append(GTK_BOX(pageBody), descLabel);
    auto* description = gtk_label_new(details.description.c_str());
    gtk_label_set_wrap(GTK_LABEL(description), true);
    gtk_label_set_xalign(GTK_LABEL(description), 0);
    gtk_label_set_selectable(GTK_LABEL(description), true);
    gtk_box_append(GTK_BOX(pageBody), description);
  }

  auto* meta = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(meta, "Details");
  appendMeta(meta, "Developer", details.developer);
  if (!details.categories.empty()) {
    std::string joined;
    for (const auto& category : details.categories) {
      if (!joined.empty()) joined += " · ";
      joined += noctalia::packages::categoryDisplayName(category);
    }
    appendMeta(meta, "Categories", joined);
  }
  appendMeta(meta, "Version", hit.version);
  appendMeta(meta, "Download size", friendlySize(details.downloadSize));
  appendMeta(meta, "Installed size", friendlySize(details.installedSize));
  appendMeta(meta, "Content rating", details.contentRating);
  appendMeta(meta, "License", details.license);
  appendLinkMeta(meta, "Homepage", details.homepage);
  appendLinkMeta(meta, "Help", details.helpUrl);
  appendLinkMeta(meta, "Bug tracker", details.bugtracker);
  appendLinkMeta(meta, "Donate", details.donation);
  appendMeta(meta, "Runtime", details.runtime);
  appendMeta(meta, "Origin", hit.repo);
  appendMeta(meta, "Installation", hit.installation);
  appendMeta(meta, "App ID", hit.name);
  appendMeta(meta, "Architecture", hit.arch);
  appendMeta(meta, "Branch", hit.release);
  gtk_box_append(GTK_BOX(pageBody), GTK_WIDGET(meta));

  if ((kind == noctalia::packages::Kind::Flatpak && noctalia::packages::isValidFlatpakAppId(hit.name))
      || (kind == noctalia::packages::Kind::Fedora && noctalia::packages::isValidRpmName(hit.name))) {
    auto* tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto* copyInstall = gtk_button_new_with_label("Copy Install Command");
    gtk_widget_add_css_class(copyInstall, "flat");
    auto* command = new std::string(
        kind == noctalia::packages::Kind::Flatpak
            ? ("flatpak install " + (noctalia::packages::preferredFlatpakRemote(hit.repo).empty()
                                         ? std::string("flathub")
                                         : noctalia::packages::preferredFlatpakRemote(hit.repo))
                  + " " + hit.name)
            : ("sudo dnf install " + hit.name)
    );
    g_signal_connect_data(
        copyInstall, "clicked",
        G_CALLBACK(+[](gpointer widget, gpointer user) {
          auto* display = gtk_widget_get_display(GTK_WIDGET(widget));
          auto* clipboard = gdk_display_get_clipboard(display);
          gdk_clipboard_set_text(clipboard, static_cast<std::string*>(user)->c_str());
          gtk_widget_set_tooltip_text(GTK_WIDGET(widget), "Copied");
        }),
        command, +[](gpointer user, GClosure*) { delete static_cast<std::string*>(user); }, G_CONNECT_DEFAULT
    );
    gtk_box_append(GTK_BOX(tools), copyInstall);
    gtk_box_append(GTK_BOX(pageBody), tools);
  }

  if (!details.permissions.empty()) {
    // Bazaar/Flathub feedback: summary first, details on demand.
    auto* permGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(permGroup, "Permissions");
    auto* expander = adw_expander_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(expander), false);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(expander), "What this app can access");
    const auto safety = permissionSafetySummary(details.permissions);
    if (!safety.empty()) {
      adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(expander), safety.c_str());
    }
    adw_expander_row_add_prefix(
        ADW_EXPANDER_ROW(expander), gtk_image_new_from_icon_name("security-medium-symbolic")
    );
    std::size_t start = 0;
    unsigned count = 0;
    while (start <= details.permissions.size() && count < 16) {
      const auto end = details.permissions.find('\n', start);
      auto line = details.permissions.substr(start, end == std::string::npos ? std::string::npos : end - start);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
      }
      if (!line.empty()) {
        auto* row = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), false);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), line.c_str());
        adw_expander_row_add_row(ADW_EXPANDER_ROW(expander), row);
        ++count;
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
    adw_preferences_group_add(permGroup, GTK_WIDGET(expander));
    gtk_box_append(GTK_BOX(pageBody), GTK_WIDGET(permGroup));
  }

  helix::apps::staggerRevealChildren(pageBody, 35, Style::animNormal, 0);

  auto* clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 960);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 640);
  adw_clamp_set_child(ADW_CLAMP(clamp), pageBody);
  auto* scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
  auto* toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);
  return adw_navigation_page_new(toolbar, title.c_str());
}

inline AdwNavigationPage* buildLoadingDetailPage(const noctalia::packages::PackageHit& hit) {
  const auto title = hit.title.empty() ? hit.name : hit.title;
  auto* status = adw_status_page_new();
  helix::apps::applyLoadingStatusPage(ADW_STATUS_PAGE(status), title.c_str(), "Loading package details");
  helix::apps::revealWidget(status, 0, Style::animFast);
  auto* toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), status);
  return adw_navigation_page_new(toolbar, title.c_str());
}

} // namespace helix::software
