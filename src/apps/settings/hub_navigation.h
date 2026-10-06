#pragma once

#include <adwaita.h>
#include <algorithm>
#include <cctype>
#include <functional>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace helix::settings {
using Json = nlohmann::json;
using HubActivate = std::function<void()>;

struct HubPaneSpec {
  const char* id;
  const char* title;
  const char* icon;
  const char* subtitle;
  // Catalog group titles that belong on this pane. A setting matches when its
  // group equals a prefix or starts with "prefix · " / "prefix ".
  std::vector<const char*> groups;
};

inline bool groupMatchesPane(std::string_view group, const HubPaneSpec& pane) {
  for (const char* expected : pane.groups) {
    const std::string_view prefix(expected);
    if (group == prefix) return true;
    if (group.starts_with(prefix) && group.size() > prefix.size() &&
        (group[prefix.size()] == ' ' || group.substr(prefix.size(), 3) == " · "))
      return true;
  }
  return false;
}

inline bool settingMatchesPane(const Json& setting, const HubPaneSpec& pane) {
  return groupMatchesPane(setting.value("group", ""), pane);
}

inline const std::vector<HubPaneSpec>& desktopDockPanes() {
  static const std::vector<HubPaneSpec> panes = {
      {"dock", "Dock", "user-bookmarks-symbolic", "Position, icons and shape", {"Dock", "Dock Favorites", "Dock Layout", "Dock Launcher", "Dock Shape", "Dock Effects", "Dock Focus"}},
      {"bar", "Bar", "view-continuous-symbolic", "Layout, chrome and widgets", {"Bar"}},
      {"launcher", "Launcher", "system-search-symbolic", "Search, grid and pinning", {"Launcher", "Launcher Favorites"}},
      {"control-center", "Control Center", "open-menu-symbolic", "Home shortcuts and window size", {"Control Center"}},
      {"osd", "On-Screen Indicators", "multimedia-volume-control-symbolic", "Placement and events", {"On-Screen Indicators", "Indicator Events"}},
      {"hot-corners", "Hot Corners", "input-touchpad-symbolic", "Corner actions and delay", {"Hot Corners", "Hot Corner Commands", "Screen Corners"}},
      {"window-switcher", "Window Switcher", "view-restore-symbolic", "Window order", {"Window Switcher"}},
      {"capture", "Capture", "camera-photo-symbolic", "Screenshot and screen record", {"Screenshot · Output", "Screenshot · Capture", "Screenshot · Annotation", "Screen Record"}},
      {"hooks", "Automation", "emblem-system-symbolic", "Lifecycle and event commands", {"Automation"}},
      {"panels", "Panels", "window-new-symbolic", "Placement for shell panels", {"Panels"}},
      {"niri", "Niri", "preferences-desktop-display-symbolic", "Layout and window chrome", {"Niri Layout", "Niri Chrome"}},
      {"desktop", "Desktop", "user-desktop-symbolic", "Desktop widgets", {"Desktop"}},
      {"media", "Media", "audio-x-generic-symbolic", "Media player controls", {"Media"}},
  };
  return panes;
}

inline const std::vector<HubPaneSpec>& appearancePanes() {
  static const std::vector<HubPaneSpec> panes = {
      {"colors", "Colors", "color-select-symbolic", "Mode, palette and shell chrome colors", {"Colors"}},
      {"glass", "Shell chrome", "applications-graphics-symbolic", "Material, glass and panel chrome", {"Shell chrome"}},
      {"effects", "Borders & Shadows", "insert-image-symbolic", "Outlines and drop shadows", {"Borders", "Shadows"}},
      {"icons", "Icons", "application-x-executable-symbolic", "Application icon tinting", {"Icons"}},
      {"wallpaper", "Wallpaper", "preferences-desktop-wallpaper-symbolic", "Folders, fit and automation", {"Wallpaper", "Wallpaper Automation", "Wallhaven"}},
      {"video-wallpaper", "Video Wallpaper", "camera-video-symbolic", "Video folder and playback", {"Video Wallpaper"}},
      {"backdrop", "Backdrop & Rendering", "weather-fog-symbolic", "Dim wallpaper and GL options", {"Backdrop", "Rendering"}},
      {"type", "Typography & Shape", "format-text-symbolic", "Font and corner roundness", {"Typography", "Shape"}},
      {"templates", "Templates", "document-page-setup-symbolic", "Application theme templates", {"Templates"}},
  };
  return panes;
}

inline const std::vector<HubPaneSpec>& privacyPanes() {
  static const std::vector<HubPaneSpec> panes = {
      {"privacy", "Privacy", "security-high-symbolic", "Network and activity privacy", {"Network Privacy", "Activity Privacy", "Privacy Indicators", "Profile", "Greeter"}},
      {"lock-screen", "Lock Screen", "system-lock-screen-symbolic", "Lock presentation and widgets", {"Lock Screen"}},
      {"location", "Location & Weather", "mark-location-symbolic", "Coordinates and weather", {"Location", "Weather"}},
      {"clipboard", "Clipboard", "edit-copy-symbolic", "History and paste behavior", {"Clipboard"}},
      {"authentication", "Authentication", "dialog-password-symbolic", "Password and Polkit", {"Authentication"}},
      {"storage", "Storage", "drive-harddisk-symbolic", "Credential storage", {"Storage"}},
  };
  return panes;
}

inline const std::vector<HubPaneSpec>& networkPanes() {
  static const std::vector<HubPaneSpec> panes = {
      {"wifi-prefs", "Wi-Fi Preferences", "network-wireless-symbolic", "Bands and hidden networks", {"Wi-Fi Preferences"}},
      {"dns", "DNS", "network-workgroup-symbolic", "Custom DNS and NextDNS", {"DNS"}},
      {"tailscale", "Tailscale", "network-vpn-symbolic", "Peer status and admin", {"Tailscale"}},
  };
  return panes;
}

inline const std::vector<HubPaneSpec>& systemPanes() {
  static const std::vector<HubPaneSpec> panes = {
      {"clock", "Clock", "preferences-system-time-symbolic", "Time and date formats", {"Clock"}},
      {"calendar", "Calendar", "x-office-calendar-symbolic", "Events and refresh", {"Calendar"}},
      {"localsend", "LocalSend", "folder-download-symbolic", "Nearby file sharing", {"LocalSend"}},
      {"language", "Language", "preferences-desktop-locale-symbolic", "Shell language", {"Language"}},
      {"setup", "Setup", "emblem-system-symbolic", "First-run wizard", {"Setup"}},
      {"advanced", "Advanced", "applications-system-symbolic", "Show advanced Settings controls", {"Settings"}},
  };
  return panes;
}

// Sound keeps Sounds inline under the live mixer (HIG single-page tab).
inline const std::vector<HubPaneSpec>* panesForPage(std::string_view page) {
  if (page == "desktop-dock") return &desktopDockPanes();
  if (page == "appearance") return &appearancePanes();
  if (page == "privacy-security") return &privacyPanes();
  if (page == "network") return &networkPanes();
  if (page == "system") return &systemPanes();
  return nullptr;
}

inline bool isHubPage(std::string_view page) { return panesForPage(page) != nullptr; }

inline const HubPaneSpec* findPane(std::string_view page, std::string_view paneId) {
  const auto* panes = panesForPage(page);
  if (!panes) return nullptr;
  for (const auto& pane : *panes) if (pane.id == paneId) return &pane;
  return nullptr;
}

inline const HubPaneSpec* paneForSettingGroup(std::string_view page, std::string_view group) {
  const auto* panes = panesForPage(page);
  if (!panes) return nullptr;
  for (const auto& pane : *panes) if (groupMatchesPane(group, pane)) return &pane;
  return nullptr;
}

inline bool paneHasSettings(const Json& catalog, std::string_view page, const HubPaneSpec& pane) {
  for (const auto& setting : catalog) {
    if (setting.value("page", "") != page) continue;
    if (settingMatchesPane(setting, pane)) return true;
  }
  return false;
}

inline int matchingGroupCount(const Json& catalog, std::string_view page, const HubPaneSpec& pane) {
  std::vector<std::string> seen;
  for (const auto& setting : catalog) {
    if (setting.value("page", "") != page || !settingMatchesPane(setting, pane)) continue;
    const auto name = setting.value("group", "");
    if (std::ranges::find(seen, name) == seen.end()) seen.push_back(name);
  }
  return int(seen.size());
}

inline GtkWidget* makeNavRow(const char* icon, const char* title, const char* subtitle, HubActivate activate) {
  auto* row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), false);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  if (subtitle && *subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  if (icon && *icon) {
    auto* image = gtk_image_new_from_icon_name(icon);
    gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), image);
  }
  auto* chevron = gtk_image_new_from_icon_name("go-next-symbolic");
  gtk_widget_set_valign(chevron, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), chevron);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), true);
  auto* function = new HubActivate(std::move(activate));
  g_signal_connect_data(row, "activated", G_CALLBACK(+[](AdwActionRow*, gpointer data) {
    (*static_cast<HubActivate*>(data))();
  }), function, [](gpointer data, GClosure*) { delete static_cast<HubActivate*>(data); }, G_CONNECT_DEFAULT);
  return GTK_WIDGET(row);
}

struct HubNavCluster {
  const char* title;
  std::vector<const char*> paneIds;
};

inline const std::vector<HubNavCluster>* navClustersForPage(std::string_view page) {
  if (page == "appearance") {
    static const std::vector<HubNavCluster> clusters = {
        {"Colors & Chrome", {"colors", "glass", "effects", "icons"}},
        {"Wallpaper", {"wallpaper", "video-wallpaper", "backdrop"}},
        {"Type & Templates", {"type", "templates"}},
    };
    return &clusters;
  }
  if (page == "desktop-dock") {
    static const std::vector<HubNavCluster> clusters = {
        {"Layout", {"dock", "bar", "launcher", "desktop", "media"}},
        {"Shell", {"control-center", "osd", "hot-corners", "window-switcher", "panels"}},
        {"Capture & Automation", {"capture", "hooks", "niri"}},
    };
    return &clusters;
  }
  if (page == "network") {
    static const std::vector<HubNavCluster> clusters = {
        {"Preferences", {"wifi-prefs", "dns", "tailscale"}},
    };
    return &clusters;
  }
  if (page == "system") {
    static const std::vector<HubNavCluster> clusters = {
        {"Time & Language", {"clock", "calendar", "language"}},
        {"Sharing & Setup", {"localsend", "setup", "advanced"}},
    };
    return &clusters;
  }
  return nullptr;
}

// Build clustered (or single) hub navigation groups. `makeGroup` creates each titled group;
// `onRow` receives every visible nav row for focus restore.
inline void populateHubNav(const Json& catalog, std::string_view page, const char* fallbackTitle,
                           const std::function<AdwPreferencesGroup*(const char* title)>& makeGroup,
                           const std::function<void(const HubPaneSpec&)>& open,
                           const std::function<void(const HubPaneSpec&, GtkWidget*)>& onRow) {
  const auto* panes = panesForPage(page);
  if (!panes) return;
  std::set<std::string_view> shown;
  auto addPane = [&](AdwPreferencesGroup*& host, const char* title, const HubPaneSpec& pane) {
    if (!host) host = makeGroup(title);
    auto* row = makeNavRow(pane.icon, pane.title, pane.subtitle, [open, pane] { open(pane); });
    adw_preferences_group_add(host, row);
    shown.insert(pane.id);
    onRow(pane, row);
  };
  if (const auto* clusters = navClustersForPage(page)) {
    for (const auto& cluster : *clusters) {
      AdwPreferencesGroup* host = nullptr;
      for (const char* id : cluster.paneIds) {
        const auto* pane = findPane(page, id);
        if (!pane || !paneHasSettings(catalog, page, *pane)) continue;
        addPane(host, cluster.title, *pane);
      }
    }
  }
  AdwPreferencesGroup* leftover = nullptr;
  for (const auto& pane : *panes) {
    if (shown.contains(pane.id) || !paneHasSettings(catalog, page, pane)) continue;
    addPane(leftover, fallbackTitle, pane);
  }
}

// Map legacy --section ids onto hub pane ids.
inline std::string_view paneIdForSection(std::string_view section) {
  if (section == "screenshot" || section == "screen-record") return "capture";
  if (section == "roundness") return "type";
  if (section == "wallpaper" || section == "theme") return "wallpaper";
  if (section == "wifi" || section == "wifi-prefs") return "wifi-prefs";
  if (section == "location" || section == "weather") return "location";
  if (section == "lockscreen" || section == "lock-screen") return "lock-screen";
  if (section == "sounds" || section == "audio") return "sounds";
  if (section == "widgets" || section == "desktop") return "desktop";
  if (section == "firewall" || section == "security") return "privacy";
  return section;
}

inline bool equalsFold(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

// Drop or shorten group titles that already repeat the pane/chip title (HIG).
inline std::string paneGroupTitle(std::string_view group, const HubPaneSpec& pane, int distinctGroups = -1) {
  if (distinctGroups == 1) return {};

  const std::string_view id(pane.id);
  if (id == "capture") {
    constexpr std::string_view shot = "Screenshot · ";
    if (group.starts_with(shot)) return std::string(group.substr(shot.size()));
    if (group == "Screen Record") return "Screen Record";
  }
  if (id == "osd" && group == "Indicator Events") return "Events";
  if (id == "hot-corners") {
    if (group == "Hot Corner Commands") return "Commands";
    if (group == "Screen Corners") return "Screen Corners";
  }
  if (id == "privacy") {
    if (group == "Network Privacy") return "Network";
    if (group == "Activity Privacy") return "Activity";
    if (group == "Privacy Indicators") return "Indicators";
  }
  if (id == "wallpaper" && group == "Wallpaper Automation") return "Automation";
  if (id == "niri") {
    if (group == "Niri Layout") return "Layout";
    if (group == "Niri Chrome") return "Chrome";
  }
  if (id == "dock" && group.starts_with("Dock ")) return std::string(group.substr(5));
  if (id == "launcher" && group.starts_with("Launcher ")) return std::string(group.substr(9));

  const std::string_view title(pane.title);
  if (equalsFold(group, title)) return distinctGroups > 1 ? "General" : "";
  if (group.starts_with(title) && group.size() > title.size()) {
    auto rest = group.substr(title.size());
    if (rest.starts_with(" · ")) return std::string(rest.substr(3));
    if (rest.starts_with(' ')) return std::string(rest.substr(1));
  }
  // Compound pane titles ("Borders & Shadows"): keep the matching half as the group title.
  if (title.find(" & ") != std::string_view::npos) {
    size_t start = 0;
    while (start <= title.size()) {
      const auto sep = title.find(" & ", start);
      const auto part = title.substr(start, sep == std::string_view::npos ? std::string_view::npos : sep - start);
      if (equalsFold(group, part)) return std::string(part);
      if (sep == std::string_view::npos) break;
      start = sep + 3;
    }
  }
  return std::string(group);
}

inline bool usesChipHub(std::string_view page) { return page == "privacy-security"; }
}
