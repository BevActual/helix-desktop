#pragma once

#include "apps/software/app_grid.h"
#include "ui/style.h"

#include <adwaita.h>
#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace helix::software {

inline int storeScore(const noctalia::packages::PackageHit& hit) {
  int score = 0;
  if (!hit.iconPath.empty()) {
    score += 40;
  }
  if (!hit.title.empty() && hit.title != hit.name) {
    score += 20;
  }
  if (!hit.summary.empty() && hit.summary != hit.title) {
    score += 15;
  }
  if (hit.verified) {
    score += 30;
  }
  if (hit.repo == "flathub" || noctalia::packages::preferredFlatpakRemote(hit.repo) == "flathub") {
    score += 10;
  }
  if (hit.state == noctalia::packages::PackageState::Installed || hit.state == noctalia::packages::PackageState::Update) {
    score += 5;
  }
  static constexpr const char* kBoost[] = {
      "org.mozilla.firefox",
      "org.chromium.Chromium",
      "com.spotify.Client",
      "com.discordapp.Discord",
      "org.signal.Signal",
      "org.gimp.GIMP",
      "org.blender.Blender",
      "org.videolan.VLC",
      "org.gnome.Loupe",
      "org.gnome.Calculator",
      "org.gnome.TextEditor",
      "org.gnome.Snapshot",
      "com.visualstudio.code",
      "com.jetbrains.IntelliJ-IDEA-Community",
      "org.kde.krita",
      "org.inkscape.Inkscape",
      "com.obsproject.Studio",
      "org.libreoffice.LibreOffice",
      "com.valvesoftware.Steam",
      "net.lutris.Lutris",
      "com.heroicgameslauncher.hgl",
      "org.gnome.Epiphany",
      "org.gnome.Geary",
      "org.telegram.desktop",
  };
  for (const char* id : kBoost) {
    if (hit.name == id) {
      score += 50;
      break;
    }
  }
  return score;
}

inline std::vector<noctalia::packages::PackageHit> pickFeaturedApps(
    const std::vector<noctalia::packages::PackageHit>& catalog, std::size_t limit = 7
) {
  std::vector<noctalia::packages::PackageHit> ranked = catalog;
  std::ranges::stable_sort(ranked, [](const auto& left, const auto& right) {
    return storeScore(left) > storeScore(right);
  });
  std::vector<noctalia::packages::PackageHit> featured;
  featured.reserve(limit);
  for (const auto& hit : ranked) {
    if (hit.title.empty() && hit.summary.empty()) {
      continue;
    }
    if (hit.iconPath.empty() && storeScore(hit) < 60) {
      continue;
    }
    featured.push_back(hit);
    if (featured.size() >= limit) {
      break;
    }
  }
  if (featured.size() < 3) {
    featured.clear();
    for (const auto& hit : ranked) {
      featured.push_back(hit);
      if (featured.size() >= limit) {
        break;
      }
    }
  }
  return featured;
}

inline std::vector<noctalia::packages::PackageHit> pickCategoryApps(
    const std::vector<noctalia::packages::PackageHit>& catalog, const std::string& category, std::size_t limit = 12
) {
  std::vector<noctalia::packages::PackageHit> matches;
  for (const auto& hit : catalog) {
    // Prefer iconed apps on shelves so category rows don't look like a dump of unknowns.
    if (hit.iconPath.empty() && hit.iconName.empty()) {
      continue;
    }
    bool ok = false;
    for (const auto& entry : hit.categories) {
      if (entry == category) {
        ok = true;
        break;
      }
      if (category == "Game" && entry.find("Game") != std::string::npos) {
        ok = true;
        break;
      }
      if (category == "AudioVideo"
          && (entry == "Audio" || entry == "Video" || entry == "Player" || entry == "Music")) {
        ok = true;
        break;
      }
    }
    if (ok) {
      matches.push_back(hit);
    }
  }
  std::ranges::stable_sort(matches, [](const auto& left, const auto& right) {
    return storeScore(left) > storeScore(right);
  });
  if (matches.size() > limit) {
    matches.resize(limit);
  }
  return matches;
}

inline GtkWidget* makeFeaturedCard(
    const noctalia::packages::PackageHit& hit, noctalia::packages::Kind kind,
    const std::function<void(noctalia::packages::PackageHit)>& onActivate
) {
  auto* button = gtk_button_new();
  gtk_widget_add_css_class(button, "card");
  gtk_widget_set_hexpand(button, true);
  gtk_widget_set_size_request(button, -1, 188);

  auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
  gtk_widget_set_margin_top(row, 22);
  gtk_widget_set_margin_bottom(row, 22);
  gtk_widget_set_margin_start(row, 22);
  gtk_widget_set_margin_end(row, 22);
  gtk_box_append(GTK_BOX(row), packageIcon(hit, kind, 112));

  auto* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_hexpand(text, true);
  gtk_widget_set_valign(text, GTK_ALIGN_CENTER);

  const auto title = hit.title.empty() ? hit.name : hit.title;
  auto* titleLabel = gtk_label_new(title.c_str());
  gtk_label_set_wrap(GTK_LABEL(titleLabel), true);
  gtk_label_set_xalign(GTK_LABEL(titleLabel), 0);
  gtk_label_set_ellipsize(GTK_LABEL(titleLabel), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(titleLabel), 28);
  gtk_widget_add_css_class(titleLabel, "title-2");
  gtk_box_append(GTK_BOX(text), titleLabel);

  if (hit.verified) {
    auto* verified = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(verified), gtk_image_new_from_icon_name("emblem-ok-symbolic"));
    auto* verifiedLabel = gtk_label_new("Verified publisher");
    gtk_widget_add_css_class(verifiedLabel, "caption");
    gtk_widget_add_css_class(verifiedLabel, "accent");
    gtk_box_append(GTK_BOX(verified), verifiedLabel);
    gtk_box_append(GTK_BOX(text), verified);
  }

  const auto summaryText = hit.summary.empty() || hit.summary == hit.title ? std::string{} : hit.summary;
  if (!summaryText.empty()) {
    auto* summary = gtk_label_new(summaryText.c_str());
    gtk_label_set_wrap(GTK_LABEL(summary), true);
    gtk_label_set_lines(GTK_LABEL(summary), 2);
    gtk_label_set_ellipsize(GTK_LABEL(summary), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(summary), 0);
    gtk_widget_add_css_class(summary, "body");
    gtk_widget_add_css_class(summary, "dim-label");
    gtk_box_append(GTK_BOX(text), summary);
  }
  const char* actionLabel = hit.state == noctalia::packages::PackageState::Update ? "Update"
      : hit.state == noctalia::packages::PackageState::Installed                 ? "Open"
                                                                                : "View";
  auto* action = gtk_label_new(actionLabel);
  gtk_label_set_xalign(GTK_LABEL(action), 0);
  gtk_widget_add_css_class(action, "caption");
  gtk_widget_add_css_class(action, "accent");
  gtk_box_append(GTK_BOX(text), action);

  gtk_box_append(GTK_BOX(row), text);
  gtk_button_set_child(GTK_BUTTON(button), row);

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
  gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, title.c_str(), -1);
  return button;
}

inline GtkWidget* makeStoreTile(
    const noctalia::packages::PackageHit& hit, noctalia::packages::Kind kind,
    const std::function<void(noctalia::packages::PackageHit)>& onActivate
) {
  auto* button = gtk_button_new();
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_set_size_request(button, 112, 148);
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);
  gtk_widget_set_margin_start(box, 4);
  gtk_widget_set_margin_end(box, 4);
  gtk_box_append(GTK_BOX(box), packageIcon(hit, kind, 80));

  const auto title = hit.title.empty() ? hit.name : hit.title;
  auto* name = gtk_label_new(title.c_str());
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(name), 12);
  gtk_label_set_lines(GTK_LABEL(name), 2);
  gtk_label_set_wrap(GTK_LABEL(name), true);
  gtk_label_set_justify(GTK_LABEL(name), GTK_JUSTIFY_CENTER);
  gtk_label_set_xalign(GTK_LABEL(name), 0.5F);
  gtk_widget_add_css_class(name, "caption");
  gtk_box_append(GTK_BOX(box), name);

  if (hit.state == noctalia::packages::PackageState::Update) {
    auto* chip = gtk_label_new("Update");
    gtk_label_set_xalign(GTK_LABEL(chip), 0.5F);
    gtk_widget_add_css_class(chip, "caption");
    gtk_widget_add_css_class(chip, "accent");
    gtk_box_append(GTK_BOX(box), chip);
  } else if (hit.verified) {
    auto* chip = gtk_image_new_from_icon_name("emblem-ok-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(chip), 14);
    gtk_widget_add_css_class(chip, "accent");
    gtk_widget_set_halign(chip, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(chip, "Verified publisher");
    gtk_box_append(GTK_BOX(box), chip);
  } else if (hit.state == noctalia::packages::PackageState::Installed) {
    auto* chip = gtk_label_new("Installed");
    gtk_label_set_xalign(GTK_LABEL(chip), 0.5F);
    gtk_widget_add_css_class(chip, "caption");
    gtk_widget_add_css_class(chip, "dim-label");
    gtk_box_append(GTK_BOX(box), chip);
  }

  gtk_button_set_child(GTK_BUTTON(button), box);
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
  gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, title.c_str(), -1);
  return button;
}

inline GtkWidget* buildFeaturedCarousel(
    const std::vector<noctalia::packages::PackageHit>& featured, noctalia::packages::Kind kind,
    const std::function<void(noctalia::packages::PackageHit)>& onActivate
) {
  if (featured.empty()) {
    return nullptr;
  }
  auto* shell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  auto* heading = gtk_label_new("Featured");
  gtk_label_set_xalign(GTK_LABEL(heading), 0);
  gtk_widget_add_css_class(heading, "title-1");
  gtk_box_append(GTK_BOX(shell), heading);
  auto* blurb = gtk_label_new("Discover apps worth opening first.");
  gtk_label_set_xalign(GTK_LABEL(blurb), 0);
  gtk_widget_add_css_class(blurb, "caption");
  gtk_widget_add_css_class(blurb, "dim-label");
  gtk_box_append(GTK_BOX(shell), blurb);

  auto* carousel = adw_carousel_new();
  adw_carousel_set_allow_scroll_wheel(ADW_CAROUSEL(carousel), true);
  adw_carousel_set_allow_mouse_drag(ADW_CAROUSEL(carousel), true);
  adw_carousel_set_reveal_duration(ADW_CAROUSEL(carousel), Style::animSlow);
  for (const auto& hit : featured) {
    auto* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(page, 2);
    gtk_widget_set_margin_end(page, 2);
    gtk_box_append(GTK_BOX(page), makeFeaturedCard(hit, kind, onActivate));
    adw_carousel_append(ADW_CAROUSEL(carousel), page);
  }
  gtk_box_append(GTK_BOX(shell), carousel);

  auto* dots = adw_carousel_indicator_dots_new();
  adw_carousel_indicator_dots_set_carousel(ADW_CAROUSEL_INDICATOR_DOTS(dots), ADW_CAROUSEL(carousel));
  gtk_widget_set_halign(dots, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(shell), dots);

  // Auto-advance featured apps when motion is enabled (matches common store carousels).
  if (featured.size() > 1 && adw_get_enable_animations(carousel)) {
    struct CarouselAdvance {
      AdwCarousel* carousel = nullptr;
      guint source = 0;
    };
    auto* state = new CarouselAdvance{ADW_CAROUSEL(carousel), 0};
    state->source = g_timeout_add_seconds(6, +[](gpointer data) -> gboolean {
      auto* advance = static_cast<CarouselAdvance*>(data);
      if (!ADW_IS_CAROUSEL(advance->carousel)) {
        advance->source = 0;
        return G_SOURCE_REMOVE;
      }
      const auto n = adw_carousel_get_n_pages(advance->carousel);
      if (n < 2) {
        return G_SOURCE_CONTINUE;
      }
      const auto position = static_cast<guint>(adw_carousel_get_position(advance->carousel) + 0.5);
      const auto next = (position + 1) % n;
      if (auto* page = adw_carousel_get_nth_page(advance->carousel, next)) {
        adw_carousel_scroll_to(advance->carousel, page, true);
      }
      return G_SOURCE_CONTINUE;
    }, state);
    g_signal_connect_data(
        carousel, "destroy",
        G_CALLBACK(+[](gpointer, gpointer data) {
          auto* advance = static_cast<CarouselAdvance*>(data);
          if (advance->source) {
            g_source_remove(advance->source);
          }
          delete advance;
        }),
        state, nullptr, G_CONNECT_DEFAULT
    );
  }
  return shell;
}

inline GtkWidget* buildCategorySection(
    const std::string& categoryId, const std::string& title,
    const std::vector<noctalia::packages::PackageHit>& apps, noctalia::packages::Kind kind,
    const std::function<void(noctalia::packages::PackageHit)>& onActivate,
    const std::function<void(const std::string&)>& onSeeAll, std::size_t minimum = 3,
    bool showSeeAll = true
) {
  if (apps.size() < minimum) {
    return nullptr;
  }
  auto* section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(section, 4);
  auto* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  auto* heading = gtk_label_new(title.c_str());
  gtk_label_set_xalign(GTK_LABEL(heading), 0);
  gtk_widget_set_hexpand(heading, true);
  gtk_widget_add_css_class(heading, "title-3");
  gtk_box_append(GTK_BOX(header), heading);

  if (showSeeAll && !categoryId.empty()) {
    auto* seeAll = gtk_button_new_with_label("See all");
    gtk_widget_add_css_class(seeAll, "flat");
    struct SeeAllData {
      std::string category;
      std::function<void(const std::string&)> onSeeAll;
    };
    auto* seeData = new SeeAllData{categoryId, onSeeAll};
    g_signal_connect_data(
        seeAll, "clicked",
        G_CALLBACK(+[](gpointer, gpointer user) {
          auto& request = *static_cast<SeeAllData*>(user);
          request.onSeeAll(request.category);
        }),
        seeData, +[](gpointer user, GClosure*) { delete static_cast<SeeAllData*>(user); }, G_CONNECT_DEFAULT
    );
    gtk_box_append(GTK_BOX(header), seeAll);
  }
  gtk_box_append(GTK_BOX(section), header);

  auto* strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  for (const auto& hit : apps) {
    gtk_box_append(GTK_BOX(strip), makeStoreTile(hit, kind, onActivate));
  }
  auto* scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), strip);
  gtk_widget_set_size_request(scroll, -1, 158);
  gtk_box_append(GTK_BOX(section), scroll);
  return section;
}

inline bool shouldShowStoreHome(
    const std::string& page, noctalia::packages::Kind kind, bool catalogLoaded, bool categoriesReady,
    const std::string& query, const std::string& categoryFilter
) {
  return page == "browse" && kind == noctalia::packages::Kind::Flatpak && catalogLoaded && categoriesReady
      && query.empty() && categoryFilter.empty();
}

inline void sortByStoreScore(std::vector<noctalia::packages::PackageHit>& hits) {
  std::ranges::stable_sort(hits, [](const auto& left, const auto& right) {
    const int leftScore = storeScore(left);
    const int rightScore = storeScore(right);
    if (leftScore != rightScore) {
      return leftScore > rightScore;
    }
    const auto& leftKey = left.title.empty() ? left.name : left.title;
    const auto& rightKey = right.title.empty() ? right.name : right.title;
    return leftKey < rightKey;
  });
}

inline std::vector<noctalia::packages::PackageHit> pickVerifiedApps(
    const std::vector<noctalia::packages::PackageHit>& catalog, std::size_t limit = 12
) {
  std::vector<noctalia::packages::PackageHit> matches;
  for (const auto& hit : catalog) {
    if (hit.verified) {
      matches.push_back(hit);
    }
  }
  sortByStoreScore(matches);
  if (matches.size() > limit) {
    matches.resize(limit);
  }
  return matches;
}

inline std::vector<noctalia::packages::PackageHit> pickEditorsPicks(
    const std::vector<noctalia::packages::PackageHit>& catalog, std::size_t limit = 12
) {
  auto featured = pickFeaturedApps(catalog, limit + 4);
  std::unordered_map<std::string, bool> seen;
  for (const auto& hit : featured) {
    seen[hit.name] = true;
  }
  std::vector<noctalia::packages::PackageHit> picks;
  for (const auto& hit : catalog) {
    if (seen.contains(hit.name)) {
      continue;
    }
    if (!hit.verified && storeScore(hit) < 70) {
      continue;
    }
    picks.push_back(hit);
  }
  sortByStoreScore(picks);
  if (picks.size() > limit) {
    picks.resize(limit);
  }
  if (picks.size() < 3) {
    picks.clear();
    for (const auto& hit : catalog) {
      if (seen.contains(hit.name)) {
        continue;
      }
      picks.push_back(hit);
      if (picks.size() >= limit) {
        break;
      }
    }
    sortByStoreScore(picks);
  }
  return picks;
}

inline std::vector<noctalia::packages::PackageHit> pickPopularApps(
    const std::vector<noctalia::packages::PackageHit>& catalog, std::size_t limit = 12
) {
  // Bazaar/Flathub-style “Popular”: strong icons + score, not a raw dump.
  auto featured = pickFeaturedApps(catalog, 8);
  std::unordered_map<std::string, bool> skip;
  for (const auto& hit : featured) {
    skip[hit.name] = true;
  }
  std::vector<noctalia::packages::PackageHit> ranked = catalog;
  sortByStoreScore(ranked);
  std::vector<noctalia::packages::PackageHit> out;
  for (const auto& hit : ranked) {
    if (skip.contains(hit.name)) {
      continue;
    }
    if (hit.iconPath.empty() && hit.iconName.empty()) {
      continue;
    }
    if (hit.title.empty() && hit.summary.empty()) {
      continue;
    }
    out.push_back(hit);
    if (out.size() >= limit) {
      break;
    }
  }
  return out;
}

inline std::vector<noctalia::packages::PackageHit> pickFreshApps(
    const std::vector<noctalia::packages::PackageHit>& catalog, std::size_t limit = 12
) {
  // “New & noteworthy” without Flathub stats: verified apps outside the popular shelf.
  auto popular = pickPopularApps(catalog, 16);
  auto featured = pickFeaturedApps(catalog, 8);
  std::unordered_map<std::string, bool> skip;
  for (const auto& hit : popular) {
    skip[hit.name] = true;
  }
  for (const auto& hit : featured) {
    skip[hit.name] = true;
  }
  std::vector<noctalia::packages::PackageHit> matches;
  for (const auto& hit : catalog) {
    if (!hit.verified || skip.contains(hit.name)) {
      continue;
    }
    if (hit.iconPath.empty() && hit.iconName.empty()) {
      continue;
    }
    matches.push_back(hit);
  }
  sortByStoreScore(matches);
  if (matches.size() > limit) {
    matches.resize(limit);
  }
  if (matches.size() < 4) {
    return pickEditorsPicks(catalog, limit);
  }
  return matches;
}

inline std::vector<noctalia::packages::PackageHit> pickInstalledSpotlight(
    const std::unordered_map<std::string, noctalia::packages::PackageHit>& installed, std::size_t limit = 12
) {
  std::vector<noctalia::packages::PackageHit> apps;
  apps.reserve(installed.size());
  for (const auto& [_, hit] : installed) {
    apps.push_back(hit);
  }
  sortByStoreScore(apps);
  if (apps.size() > limit) {
    apps.resize(limit);
  }
  return apps;
}

inline std::size_t countCategoryApps(
    const std::vector<noctalia::packages::PackageHit>& catalog, const std::string& category
) {
  std::size_t count = 0;
  for (const auto& hit : catalog) {
    for (const auto& entry : hit.categories) {
      if (entry == category
          || (category == "Game" && entry.find("Game") != std::string::npos)
          || (category == "AudioVideo"
              && (entry == "Audio" || entry == "Video" || entry == "Player" || entry == "Music"))) {
        ++count;
        break;
      }
    }
  }
  return count;
}

// Compact category directory — opens a paged list instead of stacking every shelf on Explore.
inline GtkWidget* buildCategoryDirectory(
    const std::vector<noctalia::packages::PackageHit>& catalog,
    const std::function<void(const std::string&)>& onCategory
) {
  static constexpr std::tuple<const char*, const char*, const char*> kCategories[] = {
      {"Game", "Games", "applications-games-symbolic"},
      {"Development", "Development", "applications-engineering-symbolic"},
      {"Graphics", "Graphics", "applications-graphics-symbolic"},
      {"AudioVideo", "Media", "applications-multimedia-symbolic"},
      {"Office", "Office", "applications-office-symbolic"},
      {"Network", "Network", "network-workgroup-symbolic"},
      {"Utility", "Utilities", "applications-utilities-symbolic"},
      {"Education", "Education", "accessories-dictionary-symbolic"},
      {"Science", "Science", "applications-science-symbolic"},
      {"System", "System", "applications-system-symbolic"},
      {"Security", "Security", "security-medium-symbolic"},
  };
  auto* shell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  auto* heading = gtk_label_new("Categories");
  gtk_label_set_xalign(GTK_LABEL(heading), 0);
  gtk_widget_add_css_class(heading, "title-3");
  gtk_box_append(GTK_BOX(shell), heading);
  auto* blurb = gtk_label_new("Browse by kind — each category opens a paged list.");
  gtk_label_set_xalign(GTK_LABEL(blurb), 0);
  gtk_widget_add_css_class(blurb, "caption");
  gtk_widget_add_css_class(blurb, "dim-label");
  gtk_box_append(GTK_BOX(shell), blurb);

  auto* flow = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), true);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 4);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 2);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 8);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 8);

  for (const auto& [id, label, icon] : kCategories) {
    const auto count = countCategoryApps(catalog, id);
    if (count == 0) {
      continue;
    }
    auto* button = gtk_button_new();
    gtk_widget_add_css_class(button, "card");
    auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(row, 12);
    gtk_widget_set_margin_end(row, 12);
    gtk_widget_set_margin_top(row, 12);
    gtk_widget_set_margin_bottom(row, 12);
    auto* image = gtk_image_new_from_icon_name(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(image), 28);
    gtk_box_append(GTK_BOX(row), image);
    auto* col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(col, true);
    auto* name = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_widget_add_css_class(name, "heading");
    gtk_box_append(GTK_BOX(col), name);
    auto* meta = gtk_label_new((std::to_string(count) + " apps").c_str());
    gtk_label_set_xalign(GTK_LABEL(meta), 0);
    gtk_widget_add_css_class(meta, "caption");
    gtk_widget_add_css_class(meta, "dim-label");
    gtk_box_append(GTK_BOX(col), meta);
    gtk_box_append(GTK_BOX(row), col);
    gtk_button_set_child(GTK_BUTTON(button), row);
    struct CatData {
      std::string category;
      std::function<void(const std::string&)> onCategory;
    };
    auto* data = new CatData{id, onCategory};
    g_signal_connect_data(
        button, "clicked",
        G_CALLBACK(+[](gpointer, gpointer user) {
          auto& request = *static_cast<CatData*>(user);
          request.onCategory(request.category);
        }),
        data, +[](gpointer user, GClosure*) { delete static_cast<CatData*>(user); }, G_CONNECT_DEFAULT
    );
    gtk_accessible_update_property(
        GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, (std::string(label) + ", " + std::to_string(count) + " apps").c_str(),
        -1
    );
    gtk_flow_box_append(GTK_FLOW_BOX(flow), button);
  }
  gtk_box_append(GTK_BOX(shell), flow);
  return shell;
}

inline GtkWidget* buildSearchSuggestionPills(
    const std::function<void(const std::string&)>& onSearch, noctalia::packages::Kind kind = noctalia::packages::Kind::Flatpak
) {
  auto* shell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  auto* heading = gtk_label_new(kind == noctalia::packages::Kind::Fedora ? "Popular searches" : "Quick search");
  gtk_label_set_xalign(GTK_LABEL(heading), 0);
  gtk_widget_add_css_class(heading, "title-3");
  gtk_box_append(GTK_BOX(shell), heading);

  const auto pills = kind == noctalia::packages::Kind::Fedora
      ? std::vector<std::pair<const char*, const char*>>{
            {"htop", "htop"}, {"git", "git"}, {"vim", "vim"}, {"firefox", "Firefox"}, {"vlc", "VLC"},
            {"curl", "curl"}, {"python3", "Python"}, {"gcc", "GCC"}, {"neovim", "Neovim"}, {"cowsay", "cowsay"}}
      : std::vector<std::pair<const char*, const char*>>{
            {"browser", "Browsers"}, {"game", "Games"}, {"office", "Office"}, {"music", "Music"},
            {"photo", "Photos"}, {"video", "Video"}, {"chat", "Chat"}, {"editor", "Editors"},
            {"password", "Passwords"}, {"terminal", "Terminals"}};

  auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  for (const auto& [query, label] : pills) {
    auto* pill = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(pill, "pill");
    struct PillData {
      std::string query;
      std::function<void(const std::string&)> onSearch;
    };
    auto* data = new PillData{query, onSearch};
    g_signal_connect_data(
        pill, "clicked",
        G_CALLBACK(+[](gpointer, gpointer user) {
          auto& request = *static_cast<PillData*>(user);
          request.onSearch(request.query);
        }),
        data, +[](gpointer user, GClosure*) { delete static_cast<PillData*>(user); }, G_CONNECT_DEFAULT
    );
    gtk_box_append(GTK_BOX(row), pill);
  }
  auto* scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), row);
  gtk_box_append(GTK_BOX(shell), scroll);
  return shell;
}

inline GtkWidget* buildFedoraSearchGroup(
    const char* title, const std::vector<std::pair<const char*, const char*>>& pills,
    const std::function<void(const std::string&)>& onSearch
) {
  auto* shell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  auto* heading = gtk_label_new(title);
  gtk_label_set_xalign(GTK_LABEL(heading), 0);
  gtk_widget_add_css_class(heading, "title-3");
  gtk_box_append(GTK_BOX(shell), heading);
  auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  for (const auto& [query, label] : pills) {
    auto* pill = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(pill, "pill");
    struct PillData {
      std::string query;
      std::function<void(const std::string&)> onSearch;
    };
    auto* data = new PillData{query, onSearch};
    g_signal_connect_data(
        pill, "clicked",
        G_CALLBACK(+[](gpointer, gpointer user) {
          auto& request = *static_cast<PillData*>(user);
          request.onSearch(request.query);
        }),
        data, +[](gpointer user, GClosure*) { delete static_cast<PillData*>(user); }, G_CONNECT_DEFAULT
    );
    gtk_box_append(GTK_BOX(row), pill);
  }
  auto* scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), row);
  gtk_box_append(GTK_BOX(shell), scroll);
  return shell;
}

inline GtkWidget* buildFedoraExploreHome(const std::function<void(const std::string&)>& onSearch) {
  auto* shell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  auto* hero = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  auto* title = gtk_label_new("Fedora packages");
  gtk_label_set_xalign(GTK_LABEL(title), 0);
  gtk_widget_add_css_class(title, "title-1");
  gtk_box_append(GTK_BOX(hero), title);
  auto* blurb = gtk_label_new(
      "Search system repositories for apps, libraries, and tools. Installs use PackageKit and may ask for authentication."
  );
  gtk_label_set_wrap(GTK_LABEL(blurb), true);
  gtk_label_set_xalign(GTK_LABEL(blurb), 0);
  gtk_widget_add_css_class(blurb, "body");
  gtk_widget_add_css_class(blurb, "dim-label");
  gtk_box_append(GTK_BOX(hero), blurb);
  gtk_box_append(GTK_BOX(shell), hero);
  gtk_box_append(
      GTK_BOX(shell),
      buildFedoraSearchGroup(
          "Essentials",
          {{"firefox", "Firefox"}, {"vlc", "VLC"}, {"gimp", "GIMP"}, {"libreoffice", "LibreOffice"}, {"htop", "htop"}},
          onSearch
      )
  );
  gtk_box_append(
      GTK_BOX(shell),
      buildFedoraSearchGroup(
          "Development",
          {{"git", "git"}, {"gcc", "GCC"}, {"python3", "Python"}, {"neovim", "Neovim"}, {"nodejs", "Node.js"},
           {"rust", "Rust"}},
          onSearch
      )
  );
  gtk_box_append(
      GTK_BOX(shell),
      buildFedoraSearchGroup(
          "More to try", {{"curl", "curl"}, {"cowsay", "cowsay"}, {"ffmpeg", "ffmpeg"}, {"podman", "Podman"}}, onSearch
      )
  );
  return shell;
}

struct RecentOp {
  std::string title;
  std::string summary;
  bool success = true;
};

inline GtkWidget* buildRecentActivity(const std::vector<RecentOp>& ops, std::size_t limit = 6) {
  if (ops.empty()) {
    return nullptr;
  }
  auto* shell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  auto* heading = gtk_label_new("Recent activity");
  gtk_label_set_xalign(GTK_LABEL(heading), 0);
  gtk_widget_add_css_class(heading, "title-3");
  gtk_box_append(GTK_BOX(shell), heading);

  auto* list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");
  const auto end = std::min(ops.size(), limit);
  for (std::size_t i = 0; i < end; ++i) {
    const auto& op = ops[i];
    auto* row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), op.title.c_str());
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), op.summary.c_str());
    if (op.success) {
      adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("emblem-ok-symbolic"));
    } else {
      adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("dialog-warning-symbolic"));
    }
    gtk_list_box_append(GTK_LIST_BOX(list), GTK_WIDGET(row));
  }
  gtk_box_append(GTK_BOX(shell), list);
  return shell;
}

// Popular | Name — Bazaar-style control without forcing a settings menu.
inline GtkWidget* buildListSortBar(
    const std::string& current, const std::function<void(const std::string&)>& onSort
) {
  auto* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(bar, GTK_ALIGN_START);
  auto* label = gtk_label_new("Sort");
  gtk_widget_add_css_class(label, "caption");
  gtk_widget_add_css_class(label, "dim-label");
  gtk_box_append(GTK_BOX(bar), label);
  GtkToggleButton* group = nullptr;
  for (const auto& [id, title] : std::vector<std::pair<const char*, const char*>>{
           {"popular", "Popular"}, {"name", "Name"}}) {
    auto* chip = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(title));
    if (!group) {
      group = chip;
    } else {
      gtk_toggle_button_set_group(chip, group);
    }
    gtk_widget_add_css_class(GTK_WIDGET(chip), "pill");
    if (current == id) {
      gtk_toggle_button_set_active(chip, true);
    }
    struct SortData {
      std::string id;
      std::function<void(const std::string&)> onSort;
    };
    auto* data = new SortData{id, onSort};
    g_signal_connect_data(
        GTK_WIDGET(chip), "toggled",
        G_CALLBACK(+[](GtkToggleButton* button, gpointer user) {
          if (!gtk_toggle_button_get_active(button)) {
            return;
          }
          auto& request = *static_cast<SortData*>(user);
          request.onSort(request.id);
        }),
        data, +[](gpointer user, GClosure*) { delete static_cast<SortData*>(user); }, G_CONNECT_DEFAULT
    );
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(chip));
  }
  return bar;
}

inline void applyListSort(std::vector<noctalia::packages::PackageHit>& hits, const std::string& sort) {
  if (sort == "name") {
    std::ranges::stable_sort(hits, [](const auto& left, const auto& right) {
      const auto& leftKey = left.title.empty() ? left.name : left.title;
      const auto& rightKey = right.title.empty() ? right.name : right.title;
      return leftKey < rightKey;
    });
    return;
  }
  sortByStoreScore(hits);
}

inline std::string permissionSafetySummary(std::string_view permissions) {
  if (permissions.empty()) {
    return {};
  }
  const bool host = permissions.find("all system files") != std::string_view::npos
      || permissions.find("Can access all devices") != std::string_view::npos;
  const bool home = permissions.find("home folder") != std::string_view::npos;
  const bool network = permissions.find("network access") != std::string_view::npos;
  unsigned lines = 0;
  for (char ch : permissions) {
    if (ch == '\n') {
      ++lines;
    }
  }
  if (!permissions.empty()) {
    ++lines;
  }
  if (host) {
    return "Needs broad system access";
  }
  if (home && network) {
    return "Can use the network and personal files";
  }
  if (home) {
    return "Can access personal files";
  }
  if (network) {
    return "Sandboxed with network access";
  }
  if (lines <= 2) {
    return "Limited sandbox permissions";
  }
  return "Review " + std::to_string(lines) + " permissions below";
}

} // namespace helix::software
