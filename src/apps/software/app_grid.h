#pragma once

#include "system/package_spec.h"

#include <adwaita.h>
#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace helix::software {

inline GtkWidget* packageIcon(const noctalia::packages::PackageHit& hit, noctalia::packages::Kind kind, int pixelSize = 64) {
  GtkWidget* image = nullptr;
  if (!hit.iconPath.empty()) {
    image = gtk_image_new_from_file(hit.iconPath.c_str());
  } else {
    std::string iconName = hit.iconName;
    if (iconName.empty() && kind == noctalia::packages::Kind::Fedora && !hit.name.empty()) {
      if (auto* display = gdk_display_get_default()) {
        auto* theme = gtk_icon_theme_get_for_display(display);
        if (gtk_icon_theme_has_icon(theme, hit.name.c_str())) {
          iconName = hit.name;
        }
      }
    }
    if (!iconName.empty()) {
      image = gtk_image_new_from_icon_name(iconName.c_str());
    } else if (kind == noctalia::packages::Kind::Flatpak) {
      image = gtk_image_new_from_icon_name("application-x-executable-symbolic");
    } else if (kind == noctalia::packages::Kind::Firmware) {
      image = gtk_image_new_from_icon_name("computer-symbolic");
    } else {
      image = gtk_image_new_from_icon_name("package-x-generic-symbolic");
    }
  }
  gtk_image_set_pixel_size(GTK_IMAGE(image), pixelSize);
  gtk_widget_set_size_request(image, pixelSize, pixelSize);
  gtk_widget_set_halign(image, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(image, "icon-dropshadow");
  return image;
}

inline const char* stateLabel(noctalia::packages::PackageState state) {
  switch (state) {
  case noctalia::packages::PackageState::Installed:
    return "Installed";
  case noctalia::packages::PackageState::Update:
    return "Update";
  case noctalia::packages::PackageState::Available:
    return "Available";
  case noctalia::packages::PackageState::Other:
    return "";
  }
  return "";
}

inline GtkWidget* makePackageCard(
    const noctalia::packages::PackageHit& hit, noctalia::packages::Kind kind,
    const std::function<void(noctalia::packages::PackageHit)>& onActivate
) {
  auto* button = gtk_button_new();
  gtk_widget_add_css_class(button, "card");
  gtk_widget_set_size_request(button, 156, 172);
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(box, 14);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_margin_start(box, 10);
  gtk_widget_set_margin_end(box, 10);
  gtk_box_append(GTK_BOX(box), packageIcon(hit, kind, 72));

  const auto title = hit.title.empty() ? hit.name : hit.title;
  auto* name = gtk_label_new(title.c_str());
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(name), 14);
  gtk_label_set_wrap(GTK_LABEL(name), false);
  gtk_label_set_xalign(GTK_LABEL(name), 0.5F);
  gtk_widget_add_css_class(name, "heading");
  gtk_box_append(GTK_BOX(box), name);

  const auto summaryText = hit.summary.empty() || hit.summary == hit.title ? std::string{} : hit.summary;
  if (!summaryText.empty()) {
    auto* summary = gtk_label_new(summaryText.c_str());
    gtk_label_set_ellipsize(GTK_LABEL(summary), PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(GTK_LABEL(summary), 2);
    gtk_label_set_max_width_chars(GTK_LABEL(summary), 16);
    gtk_label_set_wrap(GTK_LABEL(summary), true);
    gtk_label_set_xalign(GTK_LABEL(summary), 0.5F);
    gtk_widget_add_css_class(summary, "caption");
    gtk_widget_add_css_class(summary, "dim-label");
    gtk_box_append(GTK_BOX(box), summary);
  }

  if (hit.state == noctalia::packages::PackageState::Update) {
    std::string updateBadge = "Update";
    if (!hit.info.empty() && hit.info != "Update" && hit.info != "Installed") {
      updateBadge = hit.info;
    } else if (!hit.version.empty()) {
      updateBadge = hit.version;
    }
    auto* chip = gtk_label_new(updateBadge.c_str());
    gtk_widget_add_css_class(chip, "caption");
    gtk_widget_add_css_class(chip, "accent");
    gtk_box_append(GTK_BOX(box), chip);
  } else if (hit.verified) {
    // Keep shelf grids calm: a small emblem instead of a "Verified" caption on every tile.
    auto* chip = gtk_image_new_from_icon_name("emblem-ok-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(chip), 14);
    gtk_widget_add_css_class(chip, "accent");
    gtk_widget_set_halign(chip, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(chip, "Verified publisher");
    gtk_box_append(GTK_BOX(box), chip);
  } else if (const auto* badge = stateLabel(hit.state); badge[0] != '\0' && hit.state != noctalia::packages::PackageState::Available) {
    auto* chip = gtk_label_new(badge);
    gtk_widget_add_css_class(chip, "caption");
    gtk_widget_add_css_class(chip, "dim-label");
    gtk_box_append(GTK_BOX(box), chip);
  } else if (kind == noctalia::packages::Kind::Flatpak && !hit.repo.empty()
             && hit.state == noctalia::packages::PackageState::Available) {
    const auto remote = noctalia::packages::preferredFlatpakRemote(hit.repo);
    auto* chip = gtk_label_new(remote.empty() ? hit.repo.c_str() : remote.c_str());
    gtk_widget_add_css_class(chip, "caption");
    gtk_widget_add_css_class(chip, "dim-label");
    gtk_box_append(GTK_BOX(box), chip);
  }

  gtk_button_set_child(GTK_BUTTON(button), box);
  std::string tip = title;
  if (!hit.summary.empty() && hit.summary != title) {
    tip += "\n" + hit.summary;
  }
  if (hit.verified) {
    tip += "\nVerified publisher";
  }
  if (!hit.version.empty()) {
    tip += "\n" + hit.version;
  }
  gtk_widget_set_tooltip_text(button, tip.c_str());
  gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, title.c_str(), -1);

  struct CardData {
    noctalia::packages::PackageHit hit;
    std::function<void(noctalia::packages::PackageHit)> activate;
  };
  auto* data = new CardData{hit, onActivate};
  g_signal_connect_data(
      button, "clicked",
      G_CALLBACK(+[](gpointer, gpointer user) {
        auto& card = *static_cast<CardData*>(user);
        card.activate(card.hit);
      }),
      data, +[](gpointer user, GClosure*) { delete static_cast<CardData*>(user); }, G_CONNECT_DEFAULT
  );
  return button;
}

inline constexpr std::size_t kListPageSize = 36;

inline std::size_t listPageCount(std::size_t total, std::size_t pageSize = kListPageSize) {
  if (total == 0 || pageSize == 0) {
    return 1;
  }
  return (total + pageSize - 1) / pageSize;
}

inline void fillPackageGridPage(
    GtkFlowBox* grid, const std::vector<noctalia::packages::PackageHit>& hits, std::size_t pageIndex,
    noctalia::packages::Kind kind, const std::function<void(noctalia::packages::PackageHit)>& onActivate,
    std::size_t pageSize = kListPageSize
) {
  while (auto* child = gtk_widget_get_first_child(GTK_WIDGET(grid))) {
    gtk_flow_box_remove(grid, child);
  }
  if (hits.empty() || pageSize == 0) {
    return;
  }
  const auto pages = listPageCount(hits.size(), pageSize);
  const auto page = std::min(pageIndex, pages - 1);
  const auto start = page * pageSize;
  const auto end = std::min(start + pageSize, hits.size());
  for (std::size_t i = start; i < end; ++i) {
    gtk_flow_box_append(grid, makePackageCard(hits[i], kind, onActivate));
  }
}

// Previous / Page N of M / Next — replaces unbounded “Show more”.
inline GtkWidget* buildListPager(
    std::size_t total, std::size_t pageIndex, const std::function<void(std::size_t)>& onPage,
    std::size_t pageSize = kListPageSize
) {
  const auto pages = listPageCount(total, pageSize);
  if (total == 0 || pages <= 1) {
    return nullptr;
  }
  const auto page = std::min(pageIndex, pages - 1);
  const auto start = page * pageSize + 1;
  const auto end = std::min((page + 1) * pageSize, total);

  auto* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_halign(bar, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(bar, 4);
  gtk_widget_set_margin_bottom(bar, 4);

  auto* prev = gtk_button_new_from_icon_name("go-previous-symbolic");
  gtk_widget_add_css_class(prev, "flat");
  gtk_widget_add_css_class(prev, "circular");
  gtk_widget_set_tooltip_text(prev, "Previous page");
  gtk_widget_set_sensitive(prev, page > 0);
  gtk_accessible_update_property(GTK_ACCESSIBLE(prev), GTK_ACCESSIBLE_PROPERTY_LABEL, "Previous page", -1);

  auto* next = gtk_button_new_from_icon_name("go-next-symbolic");
  gtk_widget_add_css_class(next, "flat");
  gtk_widget_add_css_class(next, "circular");
  gtk_widget_set_tooltip_text(next, "Next page");
  gtk_widget_set_sensitive(next, page + 1 < pages);
  gtk_accessible_update_property(GTK_ACCESSIBLE(next), GTK_ACCESSIBLE_PROPERTY_LABEL, "Next page", -1);

  auto* meta = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_halign(meta, GTK_ALIGN_CENTER);
  auto* pageLabel = gtk_label_new(("Page " + std::to_string(page + 1) + " of " + std::to_string(pages)).c_str());
  gtk_widget_add_css_class(pageLabel, "heading");
  gtk_box_append(GTK_BOX(meta), pageLabel);
  auto* rangeLabel = gtk_label_new(
      ("Showing " + std::to_string(start) + "–" + std::to_string(end) + " of " + std::to_string(total)).c_str()
  );
  gtk_widget_add_css_class(rangeLabel, "caption");
  gtk_widget_add_css_class(rangeLabel, "dim-label");
  gtk_box_append(GTK_BOX(meta), rangeLabel);

  struct PagerData {
    std::size_t page = 0;
    std::size_t pages = 1;
    std::function<void(std::size_t)> onPage;
  };
  auto* prevData = new PagerData{page, pages, onPage};
  auto* nextData = new PagerData{page, pages, onPage};
  g_signal_connect_data(
      prev, "clicked",
      G_CALLBACK(+[](gpointer, gpointer user) {
        auto& pager = *static_cast<PagerData*>(user);
        if (pager.page > 0) {
          pager.onPage(pager.page - 1);
        }
      }),
      prevData, +[](gpointer user, GClosure*) { delete static_cast<PagerData*>(user); }, G_CONNECT_DEFAULT
  );
  g_signal_connect_data(
      next, "clicked",
      G_CALLBACK(+[](gpointer, gpointer user) {
        auto& pager = *static_cast<PagerData*>(user);
        if (pager.page + 1 < pager.pages) {
          pager.onPage(pager.page + 1);
        }
      }),
      nextData, +[](gpointer user, GClosure*) { delete static_cast<PagerData*>(user); }, G_CONNECT_DEFAULT
  );

  gtk_box_append(GTK_BOX(bar), prev);
  gtk_box_append(GTK_BOX(bar), meta);
  gtk_box_append(GTK_BOX(bar), next);
  return bar;
}

inline GtkFlowBox* createPackageGrid() {
  auto* grid = GTK_FLOW_BOX(gtk_flow_box_new());
  gtk_flow_box_set_selection_mode(grid, GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(grid, true);
  gtk_flow_box_set_max_children_per_line(grid, 8);
  gtk_flow_box_set_min_children_per_line(grid, 1);
  gtk_flow_box_set_column_spacing(grid, 12);
  gtk_flow_box_set_row_spacing(grid, 12);
  gtk_widget_set_hexpand(GTK_WIDGET(grid), true);
  return grid;
}

inline bool matchesQuery(const noctalia::packages::PackageHit& hit, const std::string& needle) {
  if (needle.empty()) {
    return true;
  }
  auto* foldedNeedle = g_utf8_casefold(needle.c_str(), -1);
  const std::string query = foldedNeedle ? foldedNeedle : needle;
  g_free(foldedNeedle);
  auto contains = [&](const std::string& text) {
    if (text.empty()) {
      return false;
    }
    auto* folded = g_utf8_casefold(text.c_str(), -1);
    const bool match = folded && std::string(folded).find(query) != std::string::npos;
    g_free(folded);
    return match;
  };
  if (contains(hit.title) || contains(hit.name) || contains(hit.summary) || contains(hit.repo)) {
    return true;
  }
  for (const auto& category : hit.categories) {
    if (contains(category) || contains(noctalia::packages::categoryDisplayName(category))) {
      return true;
    }
  }
  return false;
}

} // namespace helix::software
