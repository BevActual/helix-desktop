#include "apps/common/inspection_guard.h"
#include "apps/common/motion.h"
#include "core/motion_durations.h"
#include "apps/common/preview_export.h"
#include "apps/common/appearance_binding.h"
#include "apps/software/app_detail.h"
#include "apps/software/app_grid.h"
#include "apps/software/app_remotes.h"
#include "apps/software/app_store.h"
#include "system/package_kit_service.h"
#include "core/process/process.h"
#include "util/file_utils.h"
#include <adwaita.h>
#include <gio/gdesktopappinfo.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
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
  AdwNavigationView* navigation = nullptr;
  AdwViewStack* stack = nullptr;
  AdwViewStackPage* browsePage = nullptr;
  GtkWidget *switcher = nullptr, *bottom = nullptr, *source = nullptr, *search = nullptr, *refresh = nullptr, *cancel = nullptr, *sourcesBtn = nullptr, *listPager = nullptr;
  GtkWidget *categoriesBox = nullptr, *categoryScroll = nullptr;
  GtkProgressBar* progress = nullptr;
  AdwViewStackPage* updatesPage = nullptr;
  GtkLabel* status = nullptr;
  std::map<std::string, GtkBox*> bodies;
  std::map<std::string, GtkFlowBox*> grids;
  std::map<std::string, GtkToggleButton*> categoryButtons;
  std::map<std::string, GtkToggleButton*> rpmScopeButtons;
  GtkWidget* rpmScopeBox = nullptr;
  GtkWidget* rpmScopeScroll = nullptr;
  std::vector<Kind> sources;
  Kind selectedKind = Kind::Flatpak;
  std::vector<PackageHit> hits;
  std::vector<PackageHit> catalog;
  std::vector<PackageHit> pageCache;
  std::unordered_map<std::string, PackageHit> installedIndex;
  PackageDetails openDetails;
  std::string page = "browse", query, categoryFilter, rpmScope = "apps", listSort = "popular";
  bool busy = false, mutation = false, closing = false, creating = false, failed = false, detailing = false,
      confirming = false, replacingDetail = false;
  bool catalogLoaded = false, installedIndexReady = false, categoriesReady = false, indexingInstalled = false;
  bool switchingSource = false;
  bool exploreRevealed = false;
  Action pendingAction = Action::Install;
  PackageHit pendingHit;
  bool pendingDeleteData = false;
  struct QueuedOp {
    Action action = Action::Install;
    PackageHit hit;
    Kind kind = Kind::Flatpak;
    bool deleteData = false;
    bool bulk = false;
    std::vector<std::string> packageRefs;
  };
  struct HistoryEntry {
    std::string title;
    std::string verb;
    std::string source;
    bool success = true;
    std::string detail;
    std::int64_t when = 0;
  };
  std::deque<QueuedOp> opQueue;
  std::vector<HistoryEntry> history;
  std::vector<FlatpakRemote> flatpakRemotes;
  std::string pendingOpenApp;
  bool remotesPageOpen = false, remotesLoading = false, pendingRemoteMutation = false;
  FlatpakRemoteAction pendingRemoteAction = FlatpakRemoteAction::Enable;
  unsigned listPage = 0, catalogGeneration = 0, listGeneration = 0;
  guint searchTimer = 0, badgeTimer = 0, pendingAppTimer = 0;
  explicit Software(AdwApplication* application) : app(application) { loadHistory(); }
  ~Software() {
    if (searchTimer) g_source_remove(searchTimer);
    if (badgeTimer) g_source_remove(badgeTimer);
    if (pendingAppTimer) g_source_remove(pendingAppTimer);
    appearance.reset();
    if (window) g_object_unref(window);
  }
  Kind kind() const { return selectedKind; }
  void toast(const char* text) { if (!closing) adw_toast_overlay_add_toast(toasts, adw_toast_new(text)); }
  void toastWithAction(const std::string& text, const char* button, Callback onClick) {
    if (closing) return;
    auto* item = adw_toast_new(text.c_str());
    if (button && onClick) {
      adw_toast_set_button_label(item, button);
      auto* owned = new Callback(std::move(onClick));
      g_signal_connect_data(
          item, "button-clicked",
          G_CALLBACK(+[](AdwToast*, gpointer data) { (*static_cast<Callback*>(data))(); }), owned,
          +[](gpointer data, GClosure*) { delete static_cast<Callback*>(data); }, G_CONNECT_DEFAULT
      );
    }
    adw_toast_overlay_add_toast(toasts, item);
  }
  void clearCatalogCache() {
    ++catalogGeneration;
    catalog.clear();
    catalogLoaded = false;
    categoriesReady = false;
    categoryFilter.clear();
    syncCategoryButtons();
  }
  void clearInstalledIndex() {
    installedIndex.clear();
    installedIndexReady = false;
    pageCache.clear();
  }
  // List/catalog fetches block UI; package mutations keep Explore/Installed browsable from cache.
  bool listBusy() const { return busy && !indexingInstalled && !mutation; }
  bool operationBusy() const { return mutation || noctalia::packages::busy(); }
  bool canFilterCachedCatalog() const {
    return page == "browse" && kind() == Kind::Flatpak && catalogLoaded && !detailing;
  }
  bool canFilterPageCache() const {
    return (page == "installed" || page == "updates") && !pageCache.empty() && !detailing;
  }
  static std::string friendlyPackageError(const CommandResult& result) {
    if (result.cancelled) return "Package operation canceled.";
    if (result.error.empty()) return "Package operation failed. Refresh and retry.";
    std::string err = result.error;
    if (err.find("User aborted selection") != std::string::npos) {
      return "Several packages match that name. Open the package again and retry.";
    }
    if (err.find("not authorized") != std::string::npos || err.find("Not authorized") != std::string::npos
        || err.find("authentication") != std::string::npos || err.find("Authentication") != std::string::npos) {
      return "Authentication was canceled or denied.";
    }
    if (err.find("Package not found") != std::string::npos || err.find("No packages were found") != std::string::npos) {
      return "That package could not be found. Refresh and try again.";
    }
    constexpr std::size_t kMax = 140;
    if (err.size() > kMax) err = err.substr(0, kMax - 1) + "…";
    return err;
  }
  static std::filesystem::path historyPath() {
    const auto base = FileUtils::stateDir();
    if (base.empty()) return {};
    return std::filesystem::path(base) / "software-history.json";
  }
  void loadHistory() {
    history.clear();
    const auto path = historyPath();
    if (path.empty()) return;
    std::ifstream in(path);
    if (!in) return;
    nlohmann::json json;
    try {
      in >> json;
    } catch (...) {
      return;
    }
    if (!json.is_array()) return;
    for (const auto& entry : json) {
      if (!entry.is_object()) continue;
      HistoryEntry item;
      item.title = entry.value("title", "");
      item.verb = entry.value("verb", "");
      item.source = entry.value("source", "");
      item.success = entry.value("success", true);
      item.detail = entry.value("detail", "");
      item.when = entry.value("when", std::int64_t{0});
      if (!item.title.empty()) history.push_back(std::move(item));
      if (history.size() >= 20) break;
    }
  }
  void saveHistory() const {
    const auto path = historyPath();
    if (path.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    nlohmann::json json = nlohmann::json::array();
    for (const auto& entry : history) {
      json.push_back({
          {"title", entry.title},
          {"verb", entry.verb},
          {"source", entry.source},
          {"success", entry.success},
          {"detail", entry.detail},
          {"when", entry.when},
      });
    }
    std::ofstream out(path);
    if (out) out << json.dump(2);
  }
  void recordHistory(bool success, const std::string& detail = {}) {
    HistoryEntry entry;
    entry.title = pendingHit.title.empty() ? pendingHit.name : pendingHit.title;
    if (entry.title.empty()) entry.title = "Updates";
    if (pendingRemoteMutation) {
      entry.verb = pendingRemoteAction == FlatpakRemoteAction::AddFlathub ? "Added source"
          : pendingRemoteAction == FlatpakRemoteAction::Delete             ? "Removed source"
          : pendingRemoteAction == FlatpakRemoteAction::Enable             ? "Enabled source"
                                                                           : "Disabled source";
      entry.source = "Flatpak";
    } else {
      entry.verb = pendingAction == Action::Install ? "Installed"
          : pendingAction == Action::Remove         ? "Removed"
                                                    : "Updated";
      entry.source = kind() == Kind::Flatpak ? "Flatpak" : kind() == Kind::Firmware ? "Firmware" : "Fedora";
    }
    entry.success = success;
    entry.detail = detail;
    entry.when = static_cast<std::int64_t>(std::time(nullptr));
    history.insert(history.begin(), std::move(entry));
    if (history.size() > 20) history.resize(20);
    saveHistory();
  }
  std::vector<helix::software::RecentOp> recentOpsForUi() const {
    std::vector<helix::software::RecentOp> ops;
    ops.reserve(std::min<std::size_t>(history.size(), 6));
    for (std::size_t i = 0; i < history.size() && ops.size() < 6; ++i) {
      const auto& entry = history[i];
      helix::software::RecentOp op;
      op.title = entry.title;
      op.success = entry.success;
      op.summary = entry.verb + " · " + entry.source;
      if (!entry.success && !entry.detail.empty()) op.summary += " · " + entry.detail;
      ops.push_back(std::move(op));
    }
    return ops;
  }
  std::string queueStatusText() const {
    if (opQueue.empty()) return {};
    if (opQueue.size() == 1) {
      const auto& next = opQueue.front();
      const auto title = next.bulk ? "updates" : (next.hit.title.empty() ? next.hit.name : next.hit.title);
      return "1 queued after this · next: " + title;
    }
    return std::to_string(opQueue.size()) + " queued after this";
  }
  void refreshMutationProgress(const std::string& statusText, int percent = -1) {
    auto text = statusText;
    if (const auto queued = queueStatusText(); !queued.empty()) {
      text += " · " + queued;
    }
    gtk_label_set_text(status, text.c_str());
    setProgress(text, percent);
  }
  bool enqueueOp(QueuedOp op) {
    if (opQueue.size() >= 8) {
      toast("The install queue is full. Wait for a few operations to finish.");
      return false;
    }
    for (const auto& existing : opQueue) {
      if (!op.bulk && !existing.bulk && existing.hit.name == op.hit.name && existing.action == op.action
          && existing.kind == op.kind) {
        toast("That package is already queued.");
        return false;
      }
    }
    const auto title = op.bulk ? "Updates" : (op.hit.title.empty() ? op.hit.name : op.hit.title);
    opQueue.push_back(std::move(op));
    toast(("Queued " + title + " (" + std::to_string(opQueue.size()) + " waiting)").c_str());
    if (mutation) refreshMutationProgress(gtk_label_get_text(status));
    return true;
  }
  ProgressCallback mutationProgressCallback() {
    auto self = shared_from_this();
    return [self](PkconProgress value) {
      queue([self, value] {
        if (self->closing || !self->mutation) return;
        const auto text = value.status + (value.percent < 0 ? "" : " · " + std::to_string(value.percent) + "%");
        self->refreshMutationProgress(text, value.percent);
      });
    };
  }
  void startQueuedOp(QueuedOp op) {
    pendingAction = op.action;
    pendingHit = op.hit;
    pendingDeleteData = op.deleteData;
    selectedKind = op.kind;
    // Keep source dropdown aligned when draining a queued op from another source.
    if (source && !sources.empty()) {
      if (const auto it = std::ranges::find(sources, op.kind); it != sources.end()) {
        creating = true;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(source), guint(it - sources.begin()));
        creating = false;
      }
    }
    begin(true);
    if (op.bulk) {
      if (!updatePackages(std::move(op.packageRefs), completion(), mutationProgressCallback(), op.kind)) {
        busy = false;
        mutation = false;
        g_application_release(G_APPLICATION(app));
        clearProgress();
        toast(noctalia::packages::busy() ? "Another software operation is already running."
                                         : "Could not start the update operation.");
        controls();
        drainQueue();
      }
      return;
    }
    if (!mutate(op.action, op.hit, completion(), mutationProgressCallback(), op.kind, op.deleteData)) {
      busy = false;
      mutation = false;
      g_application_release(G_APPLICATION(app));
      clearProgress();
      toast(noctalia::packages::busy() ? "Another software operation is already running."
                                       : "Could not start this package operation.");
      if (detailing) replaceOpenDetailPage();
      controls();
      drainQueue();
    }
  }
  void drainQueue() {
    if (closing || mutation || noctalia::packages::busy() || opQueue.empty()) {
      tryOpenPendingApp();
      return;
    }
    auto next = std::move(opQueue.front());
    opQueue.pop_front();
    startQueuedOp(std::move(next));
  }
  void afterMutationSettled() {
    queue([self = shared_from_this()] {
      if (!self->closing) self->drainQueue();
    });
  }
  void openAppById(std::string id) {
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) id.pop_back();
    if (id.empty()) return;
    pendingOpenApp = std::move(id);
    tryOpenPendingApp();
  }
  void schedulePendingAppRetry() {
    if (pendingAppTimer || pendingOpenApp.empty() || closing) return;
    pendingAppTimer = g_timeout_add(250, +[](gpointer data) -> gboolean {
      auto* software = static_cast<Software*>(data);
      software->pendingAppTimer = 0;
      if (software->closing || software->pendingOpenApp.empty()) return G_SOURCE_REMOVE;
      if (software->kind() == Kind::Flatpak && software->page == "browse" && !software->catalogLoaded
          && !software->listBusy() && !software->operationBusy() && !software->creating) {
        software->load();
      }
      software->tryOpenPendingApp();
      if (!software->pendingOpenApp.empty() && !software->detailing) {
        software->schedulePendingAppRetry();
      }
      return G_SOURCE_REMOVE;
    }, this);
  }
  void tryOpenPendingApp() {
    if (pendingOpenApp.empty() || closing || detailing || replacingDetail) return;
    if (listBusy() || operationBusy()) {
      schedulePendingAppRetry();
      return;
    }
    const auto id = pendingOpenApp;
    PackageHit hit;
    bool found = false;
    auto match = [&](const PackageHit& candidate) {
      if (candidate.name == id) {
        hit = candidate;
        found = true;
        return true;
      }
      return false;
    };
    for (const auto& candidate : catalog) {
      if (match(candidate)) break;
    }
    if (!found) {
      if (const auto it = installedIndex.find(id); it != installedIndex.end()) {
        hit = it->second;
        found = true;
      }
    }
    if (!found) {
      for (const auto& candidate : hits) {
        if (match(candidate)) break;
      }
    }
    if (!found) {
      hit.name = id;
      if (isValidFlatpakAppId(id)) {
        if (std::ranges::find(sources, Kind::Flatpak) == sources.end()) {
          toast("Flatpak is unavailable.");
          pendingOpenApp.clear();
          return;
        }
        if (kind() != Kind::Flatpak && source) {
          const auto it = std::ranges::find(sources, Kind::Flatpak);
          creating = true;
          gtk_drop_down_set_selected(GTK_DROP_DOWN(source), guint(it - sources.begin()));
          creating = false;
          selectedKind = Kind::Flatpak;
          schedulePendingAppRetry();
          return;  // source change reloads; try again when catalog arrives
        }
      } else if (isValidRpmName(id)) {
        if (std::ranges::find(sources, Kind::Fedora) == sources.end()) {
          toast("Fedora packages are unavailable.");
          pendingOpenApp.clear();
          return;
        }
        if (kind() != Kind::Fedora && source) {
          const auto it = std::ranges::find(sources, Kind::Fedora);
          creating = true;
          gtk_drop_down_set_selected(GTK_DROP_DOWN(source), guint(it - sources.begin()));
          creating = false;
          selectedKind = Kind::Fedora;
          schedulePendingAppRetry();
          return;
        }
      } else if (isValidFwupdDeviceId(id)) {
        if (std::ranges::find(sources, Kind::Firmware) == sources.end()) {
          toast("Firmware tools are unavailable.");
          pendingOpenApp.clear();
          return;
        }
      } else {
        toast("Unknown application id.");
        pendingOpenApp.clear();
        return;
      }
    }
    page = "browse";
    creating = true;
    adw_view_stack_set_visible_child_name(stack, "browse");
    creating = false;
    // Keep pending until details finish loading; openDetail may no-op if busy.
    if (!openDetail(std::move(hit))) schedulePendingAppRetry();
  }
  void scheduleBadgeRefresh() {
    if (badgeTimer) g_source_remove(badgeTimer);
    badgeTimer = g_timeout_add(400, +[](gpointer data) -> gboolean {
      auto* software = static_cast<Software*>(data);
      software->badgeTimer = 0;
      if (software->closing) return G_SOURCE_REMOVE;
      if (noctalia::packages::busy()) {
        software->scheduleBadgeRefresh();
        return G_SOURCE_REMOVE;
      }
      if (!refreshPendingUpdateCount([software] {
            queue([software] {
              if (!software->closing) software->setUpdatesBadge(pendingUpdateCount());
            });
          })) {
        software->setUpdatesBadge(pendingUpdateCount());
      }
      return G_SOURCE_REMOVE;
    }, this);
  }
  void setProgress(const std::string& text, int percent = -1) {
    if (closing || !progress) return;
    gtk_widget_set_margin_top(GTK_WIDGET(progress), 8);
    gtk_widget_set_margin_bottom(GTK_WIDGET(progress), 4);
    helix::apps::revealProgress(GTK_WIDGET(progress), true);
    if (percent >= 0) {
      gtk_progress_bar_set_fraction(progress, std::clamp(percent, 0, 100) / 100.0);
    } else {
      gtk_progress_bar_pulse(progress);
    }
    gtk_progress_bar_set_text(progress, text.c_str());
    gtk_progress_bar_set_show_text(progress, true);
  }
  void clearProgress() {
    if (!progress) return;
    helix::apps::revealProgress(GTK_WIDGET(progress), false);
    gtk_progress_bar_set_fraction(progress, 0);
    gtk_progress_bar_set_text(progress, "");
    gtk_widget_set_margin_top(GTK_WIDGET(progress), 0);
    gtk_widget_set_margin_bottom(GTK_WIDGET(progress), 0);
  }
  void setUpdatesBadge(std::size_t count) {
    if (!updatesPage) return;
    adw_view_stack_page_set_badge_number(updatesPage, count);
    adw_view_stack_page_set_needs_attention(updatesPage, count > 0);
  }
  void syncCategoryButtons() {
    if (!categoriesBox) return;
    // Store home uses category rows; chips appear for search/category browsing.
    const bool show = page == "browse" && kind() == Kind::Flatpak && catalogLoaded && categoriesReady
        && !storeHomeActive();
    if (categoryScroll) gtk_widget_set_visible(categoryScroll, show);
    gtk_widget_set_visible(categoriesBox, show);
    for (auto& [id, button] : categoryButtons) {
      const bool active = (id.empty() && categoryFilter.empty()) || id == categoryFilter;
      const bool visible = !show || id.empty() || active || categoryHasMatches(id);
      gtk_widget_set_visible(GTK_WIDGET(button), visible);
      if (gtk_toggle_button_get_active(button) != active) {
        creating = true;
        gtk_toggle_button_set_active(button, active);
        creating = false;
      }
    }
  }
  void syncRpmScopeButtons() {
    const bool show = page == "installed" && kind() == Kind::Fedora && !pageCache.empty();
    if (rpmScopeScroll) gtk_widget_set_visible(rpmScopeScroll, show);
    if (rpmScopeBox) gtk_widget_set_visible(rpmScopeBox, show);
    for (auto& [id, button] : rpmScopeButtons) {
      const bool active = id == rpmScope;
      if (gtk_toggle_button_get_active(button) != active) {
        creating = true;
        gtk_toggle_button_set_active(button, active);
        creating = false;
      }
    }
  }
  void applyRpmScopeFilter() {
    query = search ? gtk_editable_get_text(GTK_EDITABLE(search)) : query;
    hits.clear();
    for (const auto& hit : pageCache) {
      if (!matchesRpmScope(hit.name, rpmScope)) continue;
      if (!helix::software::matchesQuery(hit, query)) continue;
      hits.push_back(hit);
    }
    listPage = 0;
    results();
    setStatusReady();
    syncRpmScopeButtons();
  }
  void selectRpmScope(const std::string& scope) {
    if (creating || closing) return;
    rpmScope = scope.empty() ? "apps" : scope;
    if (page == "installed" && kind() == Kind::Fedora && !pageCache.empty()) applyRpmScopeFilter();
    else syncRpmScopeButtons();
  }
  void syncSearchPlaceholder() {
    if (!search) return;
    if (page == "installed") {
      gtk_search_entry_set_placeholder_text(
          GTK_SEARCH_ENTRY(search), kind() == Kind::Fedora ? "Search installed packages" : "Search installed"
      );
    } else if (page == "updates") {
      gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search), "Search updates");
    } else if (kind() == Kind::Fedora) {
      gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search), "Search Fedora packages");
    } else {
      gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search), "Search apps");
    }
  }
  bool canLaunchPackage(const PackageHit& hit) const {
    if (hit.name.empty() || kind() == Kind::Firmware) return false;
    if (kind() == Kind::Flatpak) return isValidFlatpakAppId(hit.name);
    if (auto* info = g_desktop_app_info_new((hit.name + ".desktop").c_str())) {
      g_object_unref(info);
      return true;
    }
    return false;
  }
  bool matchesCategory(const PackageHit& hit, const std::string& filter) const {
    if (filter.empty()) return true;
    for (const auto& category : hit.categories) {
      if (category == filter) return true;
      if (filter == "Game" && category.find("Game") != std::string::npos) return true;
      if (filter == "AudioVideo" && (category == "Audio" || category == "Video" || category == "Player" || category == "Music")) return true;
    }
    return false;
  }
  bool matchesCategory(const PackageHit& hit) const { return matchesCategory(hit, categoryFilter); }
  bool categoryHasMatches(const std::string& filter) const {
    if (filter.empty()) return true;
    for (const auto& hit : catalog) {
      if (matchesCategory(hit, filter)) return true;
    }
    return false;
  }
  void overlayInstalled(std::vector<PackageHit>& list) const {
    if (!installedIndexReady || kind() != Kind::Flatpak) return;
    for (auto& hit : list) {
      const auto it = installedIndex.find(hit.name);
      if (it == installedIndex.end()) continue;
      if (hit.state == PackageState::Update) continue;
      hit.state = PackageState::Installed;
      hit.info = "Installed";
      if (hit.installation.empty()) hit.installation = it->second.installation;
      if (hit.version.empty()) hit.version = it->second.version;
      if (hit.release.empty()) hit.release = it->second.release;
      if (hit.iconPath.empty()) hit.iconPath = it->second.iconPath;
    }
  }
  void rememberInstalled(const std::vector<PackageHit>& list) {
    installedIndex.clear();
    for (const auto& hit : list) {
      auto [it, inserted] = installedIndex.emplace(hit.name, hit);
      if (!inserted && (hit.installation == "user" || it->second.installation.empty())) {
        it->second = hit;
      }
    }
    installedIndexReady = true;
  }
  void setStatusReady() {
    if (!status) return;
    if (storeHomeActive()) {
      gtk_widget_set_visible(GTK_WIDGET(status), false);
      gtk_widget_set_margin_top(GTK_WIDGET(status), 0);
      gtk_widget_set_margin_bottom(GTK_WIDGET(status), 0);
      return;
    }
    gtk_widget_set_visible(GTK_WIDGET(status), true);
    gtk_widget_set_margin_top(GTK_WIDGET(status), 0);
    gtk_widget_set_margin_bottom(GTK_WIDGET(status), 6);
    if (hits.empty()) {
      if (failed) {
        gtk_label_set_text(status, "The software backend could not complete this request. Refresh and retry.");
      } else if (page == "browse" && (!query.empty() || !categoryFilter.empty())) {
        gtk_label_set_text(status, "No matching software. Try another search or category.");
      } else if (page == "browse" && kind() == Kind::Fedora) {
        gtk_label_set_text(status, "Search Fedora packages, or pick a popular package above.");
      } else {
        gtk_label_set_text(status, "Software list is ready.");
      }
      return;
    }
    const auto pageSize = helix::software::kListPageSize;
    const auto pages = helix::software::listPageCount(hits.size(), pageSize);
    const auto pageIndex = std::min<std::size_t>(listPage, pages - 1);
    const auto start = pageIndex * pageSize + 1;
    const auto end = std::min((pageIndex + 1) * pageSize, hits.size());
    std::string text = "Showing " + std::to_string(start) + "–" + std::to_string(end) + " of "
        + std::to_string(hits.size());
    if (page == "browse" && kind() == Kind::Flatpak && catalogLoaded) {
      text += " apps";
      if (!categoryFilter.empty()) text += " in " + categoryDisplayName(categoryFilter);
      if (!query.empty()) text += " matching “" + query + "”";
    } else if (page == "updates") text += " updates";
    else if (page == "installed") {
      text += " installed items";
      if (kind() == Kind::Fedora && rpmScope != "all") {
        text += rpmScope == "apps" ? " (apps)" : rpmScope == "libs" ? " (libraries)" : " (devel)";
      }
    } else text += " results";
    if (pages > 1) text += " · Page " + std::to_string(pageIndex + 1) + " of " + std::to_string(pages);
    text += ".";
    gtk_label_set_text(status, text.c_str());
  }
  void controls() {
    if (closing) return;
    const bool backendReady = !sources.empty();
    const bool filterOnly = canFilterCachedCatalog() || canFilterPageCache();
    const bool blocked = listBusy();
    const bool mutating = mutation;
    gtk_widget_set_sensitive(source, backendReady && !blocked && !mutating);
    gtk_widget_set_sensitive(search, backendReady && (!blocked || filterOnly || mutating) && (page == "browse" || page == "installed" || page == "updates"));
    gtk_widget_set_sensitive(refresh, backendReady && !blocked && !mutating);
    gtk_widget_set_sensitive(switcher, backendReady && !blocked);
    gtk_widget_set_sensitive(bottom, backendReady && !blocked);
    if (sourcesBtn) {
      gtk_widget_set_sensitive(sourcesBtn, flatpakAvailable() && !blocked && !mutating && !remotesLoading);
      gtk_widget_set_visible(sourcesBtn, flatpakAvailable());
    }
    if (browsePage) adw_view_stack_page_set_visible(browsePage, selectedKind != Kind::Firmware);
    gtk_widget_set_visible(cancel, busy && !indexingInstalled);
    gtk_widget_set_sensitive(GTK_WIDGET(stack), (!blocked || mutating) && !detailing);
    if (listPager) gtk_widget_set_sensitive(listPager, (!blocked || mutating) && !detailing);
    if (categoriesBox) gtk_widget_set_sensitive(categoriesBox, backendReady && (!blocked || filterOnly || mutating) && !detailing);
    if (rpmScopeBox) gtk_widget_set_sensitive(rpmScopeBox, backendReady && (!blocked || filterOnly || mutating) && !detailing);
    syncCategoryButtons();
    syncRpmScopeButtons();
    syncSearchPlaceholder();
    if (!mutation) clearProgress();
  }
  bool storeHomeActive() const {
    return helix::software::shouldShowStoreHome(page, kind(), catalogLoaded, categoriesReady, query, categoryFilter);
  }
  void scrollListToTop() {
    auto* body = bodies.at(page);
    auto* clamp = gtk_widget_get_parent(GTK_WIDGET(body));
    auto* scroll = clamp ? gtk_widget_get_parent(clamp) : nullptr;
    if (GTK_IS_SCROLLED_WINDOW(scroll)) {
      if (auto* vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll))) {
        gtk_adjustment_set_value(vadj, 0);
      }
    }
  }
  void goListPage(std::size_t pageIndex) {
    if (closing) return;
    listPage = static_cast<unsigned>(pageIndex);
    results();
    setStatusReady();
    scrollListToTop();
  }
  void results() {
    if (closing) return;
    auto* body = bodies.at(page);
    auto* grid = grids.at(page);
    clearBoxExceptGrid(body, grid);
    listPager = nullptr;

    if (storeHomeActive()) {
      gtk_widget_set_visible(GTK_WIDGET(grid), false);
      auto activate = [this](PackageHit hit) { openDetail(std::move(hit)); };
      auto seeAll = [this](const std::string& category) { selectCategory(category); };
      const bool animateHome = !exploreRevealed;
      // Bazaar-shaped Explore: Featured → Popular → Fresh → Categories → Library → search → trust.
      if (auto* featured = helix::software::buildFeaturedCarousel(
              helix::software::pickFeaturedApps(catalog), kind(), activate
          )) {
        gtk_box_append(body, featured);
      }
      if (auto* section = helix::software::buildCategorySection(
              "", "Popular", helix::software::pickPopularApps(catalog), kind(), activate, seeAll, 3, false
          )) {
        gtk_box_append(body, section);
      }
      if (auto* section = helix::software::buildCategorySection(
              "", "New & noteworthy", helix::software::pickFreshApps(catalog), kind(), activate, seeAll, 3, false
          )) {
        gtk_box_append(body, section);
      }
      gtk_box_append(body, helix::software::buildCategoryDirectory(catalog, seeAll));
      if (installedIndexReady) {
        auto installed = helix::software::pickInstalledSpotlight(installedIndex);
        if (auto* section = helix::software::buildCategorySection(
                "installed", "Your library", installed, kind(), activate,
                [this](const std::string&) {
                  page = "installed";
                  creating = true;
                  adw_view_stack_set_visible_child_name(stack, "installed");
                  creating = false;
                  load();
                },
                1
            )) {
          gtk_box_append(body, section);
        }
      }
      gtk_box_append(body, helix::software::buildSearchSuggestionPills([this](const std::string& text) {
        listPage = 0;
        gtk_editable_set_text(GTK_EDITABLE(search), text.c_str());
        applyCatalogFilter();
      }));
      if (auto* section = helix::software::buildCategorySection(
              "", "Verified publishers", helix::software::pickVerifiedApps(catalog), kind(), activate, seeAll, 3, false
          )) {
        gtk_box_append(body, section);
      }
      if (auto* recent = helix::software::buildRecentActivity(recentOpsForUi())) {
        gtk_box_append(body, recent);
      }
      if (animateHome) {
        helix::apps::staggerRevealChildren(GTK_WIDGET(body), 40, motion::kNormal, 20);
        exploreRevealed = true;
      }
      setStatusReady();
      return;
    }

    if (hits.empty()) {
      gtk_widget_set_visible(GTK_WIDGET(grid), false);
      const bool browsingFedora = page == "browse" && query.empty() && kind() == Kind::Fedora && !busy && !failed
          && !sources.empty();
      if (browsingFedora) {
        gtk_box_append(
            body, helix::software::buildFedoraExploreHome([this](const std::string& text) {
              listPage = 0;
              gtk_editable_set_text(GTK_EDITABLE(search), text.c_str());
              load();
            })
        );
        if (auto* recent = helix::software::buildRecentActivity(recentOpsForUi())) {
          gtk_box_append(body, recent);
        }
        if (!exploreRevealed) {
          helix::apps::staggerRevealChildren(GTK_WIDGET(body), 40, motion::kNormal, 20);
          exploreRevealed = true;
        }
        setStatusReady();
        return;
      }
      exploreRevealed = false;
      auto* empty = adw_status_page_new();
      const bool browsingFlatpak = page == "browse" && query.empty() && kind() == Kind::Flatpak;
      const bool firmwareSource = kind() == Kind::Firmware;
      const bool filteredEmpty = (!query.empty() || !categoryFilter.empty() || (page == "installed" && kind() == Kind::Fedora && rpmScope != "all"))
          && (page == "browse" || page == "installed" || page == "updates");
      const bool noInstalled = page == "installed" && query.empty();
      const bool loading = busy || switchingSource;
      if (loading) {
        helix::apps::applyLoadingStatusPage(
            ADW_STATUS_PAGE(empty),
            switchingSource ? "Switching Source" : "Loading Software",
            switchingSource ? "Loading the selected software source"
                            : "Fetching the selected software source"
        );
      } else {
        adw_status_page_set_icon_name(
            ADW_STATUS_PAGE(empty),
            firmwareSource ? "computer-symbolic" : "system-software-install-symbolic"
        );
        adw_status_page_set_title(
            ADW_STATUS_PAGE(empty),
            failed             ? "Software List Unavailable"
                : sources.empty() ? "Software Backends Unavailable"
                : browsingFlatpak ? "Explore Apps"
                : filteredEmpty   ? "No Matching Software"
                : page == "updates" && firmwareSource ? "Firmware Up to Date"
                : page == "updates" ? "You're Up to Date"
                : noInstalled && firmwareSource ? "No Firmware Devices"
                : noInstalled     ? "Library is Empty"
                                  : "No Results"
        );
        adw_status_page_set_description(
            ADW_STATUS_PAGE(empty),
            failed ? "The selected backend could not provide a software list. Refresh to retry."
                : sources.empty() ? "Install Flatpak or PackageKit to browse supported software sources."
                : browsingFlatpak ? "Featured apps and categories will appear here once the catalog is ready."
                : filteredEmpty && page == "installed" && kind() == Kind::Fedora
                    ? "Try another library filter, or choose All to browse every package."
                : filteredEmpty   ? "Try another search or category, or clear the search field."
                : page == "updates" && firmwareSource
                    ? "No firmware updates are available for devices on this computer."
                : page == "updates" ? "No updates are available from this source right now."
                : noInstalled && firmwareSource
                    ? "Connected devices with firmware support will appear here."
                : noInstalled     ? "Install something from Explore and it will show up in your library."
                                  : "Refresh or try another source or search."
        );
      }
      if (filteredEmpty && page == "browse" && kind() == Kind::Fedora && !query.empty()) {
        gtk_box_append(body, empty);
        gtk_box_append(
            body, helix::software::buildSearchSuggestionPills(
                      [this](const std::string& text) {
                        listPage = 0;
                        gtk_editable_set_text(GTK_EDITABLE(search), text.c_str());
                        load();
                      },
                      Kind::Fedora
                  )
        );
      } else {
        gtk_box_append(body, empty);
      }
      if (page == "updates" && !loading && !failed) {
        const auto flatpakN = pendingUpdateCountFor(Kind::Flatpak);
        const auto rpmN = pendingUpdateCountFor(Kind::Fedora);
        const auto firmwareN = pendingUpdateCountFor(Kind::Firmware);
        std::string other;
        if (kind() != Kind::Flatpak && flatpakN > 0) {
          other += (other.empty() ? "" : " · ") + std::to_string(flatpakN) + " Flatpak";
        }
        if (kind() != Kind::Fedora && rpmN > 0) {
          other += (other.empty() ? "" : " · ") + std::to_string(rpmN) + " Fedora";
        }
        if (kind() != Kind::Firmware && firmwareN > 0) {
          other += (other.empty() ? "" : " · ") + std::to_string(firmwareN) + " firmware";
        }
        if (!other.empty()) {
          auto* hint = gtk_label_new(("Also available: " + other + ". Switch source above to review.").c_str());
          gtk_label_set_wrap(GTK_LABEL(hint), true);
          gtk_label_set_xalign(GTK_LABEL(hint), 0);
          gtk_widget_add_css_class(hint, "caption");
          gtk_widget_add_css_class(hint, "dim-label");
          gtk_box_append(body, hint);
        }
      }
      return;
    }

    exploreRevealed = false;

    const auto pages = helix::software::listPageCount(hits.size());
    if (listPage >= pages) {
      listPage = static_cast<unsigned>(pages - 1);
    }

    if (page == "browse" && !categoryFilter.empty() && kind() == Kind::Flatpak) {
      auto* headingRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      auto* back = gtk_button_new_from_icon_name("go-previous-symbolic");
      gtk_widget_add_css_class(back, "flat");
      gtk_widget_set_tooltip_text(back, "Back to Explore");
      signal(back, "clicked", [this] { selectCategory(""); });
      gtk_box_append(GTK_BOX(headingRow), back);
      auto* heading = gtk_label_new(categoryDisplayName(categoryFilter).c_str());
      gtk_label_set_xalign(GTK_LABEL(heading), 0);
      gtk_widget_set_hexpand(heading, true);
      gtk_widget_add_css_class(heading, "title-2");
      gtk_box_append(GTK_BOX(headingRow), heading);
      gtk_box_append(body, headingRow);
      auto* subtitle = gtk_label_new((std::to_string(hits.size()) + " applications").c_str());
      gtk_label_set_xalign(GTK_LABEL(subtitle), 0);
      gtk_widget_add_css_class(subtitle, "dim-label");
      gtk_box_append(body, subtitle);
      // Featured carousel only on the first page so later pages stay a clean grid.
      if (listPage == 0 && categoriesReady) {
        auto featured = helix::software::pickFeaturedApps(hits, 5);
        if (auto* carousel = helix::software::buildFeaturedCarousel(featured, kind(), [this](PackageHit hit) {
              openDetail(std::move(hit));
            })) {
          gtk_box_append(body, carousel);
        }
      }
    } else if (page == "browse" && !query.empty()) {
      auto* heading = gtk_label_new("Search results");
      gtk_label_set_xalign(GTK_LABEL(heading), 0);
      gtk_widget_add_css_class(heading, "title-2");
      gtk_box_append(body, heading);
      auto* subtitle = gtk_label_new(
          (std::to_string(hits.size()) + " matches for “" + query + "”").c_str()
      );
      gtk_label_set_xalign(GTK_LABEL(subtitle), 0);
      gtk_widget_add_css_class(subtitle, "dim-label");
      gtk_box_append(body, subtitle);
    }

    if ((page == "browse" && (!query.empty() || !categoryFilter.empty()))
        || page == "installed") {
      gtk_box_append(
          body, helix::software::buildListSortBar(listSort, [this](const std::string& sort) { selectListSort(sort); })
      );
    }

    if (page == "updates") {
      if (!hits.empty()) {
        gtk_box_append(body, button("Update All Listed", [this] { confirmAll(); }));
      }
      const auto flatpakN = pendingUpdateCountFor(Kind::Flatpak);
      const auto rpmN = pendingUpdateCountFor(Kind::Fedora);
      const auto firmwareN = pendingUpdateCountFor(Kind::Firmware);
      std::string other;
      if (kind() != Kind::Flatpak && flatpakN > 0) {
        other += (other.empty() ? "" : " · ") + std::to_string(flatpakN) + " Flatpak";
      }
      if (kind() != Kind::Fedora && rpmN > 0) {
        other += (other.empty() ? "" : " · ") + std::to_string(rpmN) + " Fedora";
      }
      if (kind() != Kind::Firmware && firmwareN > 0) {
        other += (other.empty() ? "" : " · ") + std::to_string(firmwareN) + " firmware";
      }
      if (!other.empty()) {
        auto* hint = gtk_label_new(("Also available: " + other + ". Switch source above to review.").c_str());
        gtk_label_set_wrap(GTK_LABEL(hint), true);
        gtk_label_set_xalign(GTK_LABEL(hint), 0);
        gtk_widget_add_css_class(hint, "caption");
        gtk_widget_add_css_class(hint, "dim-label");
        gtk_box_append(body, hint);
      }
    }

    gtk_widget_set_visible(GTK_WIDGET(grid), true);
    // Headers were appended after the grid; move the grid to the end (above the pager).
    g_object_ref(GTK_WIDGET(grid));
    gtk_box_remove(body, GTK_WIDGET(grid));
    gtk_box_append(body, GTK_WIDGET(grid));
    g_object_unref(GTK_WIDGET(grid));
    helix::software::fillPackageGridPage(
        grid, hits, listPage, kind(), [this](PackageHit hit) { openDetail(std::move(hit)); }
    );
    if (auto* pager = helix::software::buildListPager(
            hits.size(), listPage, [this](std::size_t next) { goListPage(next); }
        )) {
      listPager = pager;
      gtk_box_append(body, pager);
    }
  }
  void clearBoxExceptGrid(GtkBox* body, GtkFlowBox* grid) {
    std::vector<GtkWidget*> remove;
    for (auto* child = gtk_widget_get_first_child(GTK_WIDGET(body)); child; child = gtk_widget_get_next_sibling(child)) {
      if (child != GTK_WIDGET(grid)) remove.push_back(child);
    }
    for (auto* child : remove) gtk_box_remove(body, child);
  }
  void begin(bool changing, bool bumpListGeneration = true) {
    busy = true; mutation = changing; failed = false; switchingSource = false;
    // Detail fetches must not invalidate in-flight catalog/search generations.
    if (!changing && bumpListGeneration) ++listGeneration;
    g_application_hold(G_APPLICATION(app)); controls();
    gtk_label_set_text(status, changing ? "Working on the selected package…" : "Loading software…");
    if (changing) {
      setProgress("Working on the selected package…");
      if (detailing && openDetails.hit.name == pendingHit.name) replaceOpenDetailPage();
    }
    if (!changing && hits.empty()) results();
  }
  void invalidateListRequests() {
    ++listGeneration;
    if (busy && !mutation) {
      cancelCurrent();
    }
  }
  void popToHome() {
    while (adw_navigation_view_get_navigation_stack(navigation)
           && g_list_model_get_n_items(adw_navigation_view_get_navigation_stack(navigation)) > 1) {
      adw_navigation_view_pop(navigation);
    }
    detailing = false;
  }
  void applyPendingState() {
    if (pendingHit.name.empty()) return;
    PackageHit updated = pendingHit;
    if (openDetails.hit.name == pendingHit.name) updated = openDetails.hit;
    if (pendingAction == Action::Remove) {
      updated.state = PackageState::Available;
      updated.info = "Available";
      if (updated.repo.starts_with("installed:")) {
        updated.repo = updated.repo.substr(std::string_view("installed:").size());
      } else if (updated.repo == "installed") {
        updated.repo.clear();
      }
      installedIndex.erase(pendingHit.name);
    } else {
      updated.state = PackageState::Installed;
      updated.info = "Installed";
      if (!updated.repo.empty() && !updated.repo.starts_with("installed")) {
        updated.repo = "installed:" + updated.repo;
      } else if (updated.repo.empty()) {
        updated.repo = "installed";
      }
      installedIndex[pendingHit.name] = updated;
      installedIndexReady = true;
    }
    pendingHit = updated;
    if (openDetails.hit.name == pendingHit.name) openDetails.hit = updated;
    for (auto& hit : catalog) {
      if (hit.name == pendingHit.name) hit = updated;
    }
    for (auto& hit : hits) {
      if (hit.name == pendingHit.name) hit = updated;
    }
    for (auto& hit : pageCache) {
      if (hit.name == pendingHit.name) hit = updated;
    }
  }
  void replaceOpenDetailPage() {
    if (closing || !detailing) return;
    auto* page = helix::software::buildDetailPage(
        openDetails, kind(),
        [self = shared_from_this()](PackageHit package, Action action) { self->confirm(package, action); },
        [self = shared_from_this()](const PackageHit& package) { self->launchPackage(package); },
        !mutation && !busy, canLaunchPackage(openDetails.hit)
    );
    replacingDetail = true;
    if (auto* visible = adw_navigation_view_get_visible_page(navigation); visible
        && g_list_model_get_n_items(adw_navigation_view_get_navigation_stack(navigation)) > 1) {
      adw_navigation_view_pop(navigation);
    }
    adw_navigation_view_push(navigation, page);
    replacingDetail = false;
    detailing = true;
  }
  void finish(CommandResult result, unsigned generation = 0, Kind expectedKind = Kind::Flatpak, std::string expectedPage = {}) {
    const bool changing = mutation;
    if (!changing && generation != 0
        && (generation != listGeneration || kind() != expectedKind || (!expectedPage.empty() && page != expectedPage))) {
      // Stale list response after a source/tab change — keep the newer in-flight request.
      g_application_release(G_APPLICATION(app));
      if (busy && !mutation) {
        return;
      }
      busy = false;
      controls();
      return;
    }
    busy = false; mutation = false;
    if (!closing) {
      if (changing && result.success) {
        clearProgress();
        if (pendingRemoteMutation) {
          recordHistory(true);
          const auto title = pendingHit.title.empty() ? pendingHit.name : pendingHit.title;
          const auto remoteAction = pendingRemoteAction;
          pendingRemoteMutation = false;
          if (remoteAction == FlatpakRemoteAction::AddFlathub) {
            toast("Flathub added.");
          } else if (remoteAction == FlatpakRemoteAction::Delete) {
            toast((title + " removed").c_str());
          } else if (remoteAction == FlatpakRemoteAction::Enable) {
            toast((title + " enabled").c_str());
          } else {
            toast((title + " disabled").c_str());
          }
          clearCatalogCache();
          clearInstalledIndex();
          controls();
          g_application_release(G_APPLICATION(app));
          loadRemotes();
          scheduleBadgeRefresh();
          afterMutationSettled();
          return;
        }
        const bool bulkUpdate = pendingHit.name.empty() || pendingHit.name == "updates";
        recordHistory(true);
        if (bulkUpdate) {
          toast("Updates completed.");
          gtk_label_set_text(status, "Listed updates finished. Refresh if a package still looks outdated.");
          clearInstalledIndex();
          if (detailing) popToHome();
          if (page != "updates") {
            page = "updates";
            creating = true;
            adw_view_stack_set_visible_child_name(stack, "updates");
            creating = false;
          }
          controls();
          g_application_release(G_APPLICATION(app));
          load();
          scheduleBadgeRefresh();
          afterMutationSettled();
          return;
        }
        applyPendingState();
        const auto title = pendingHit.title.empty() ? pendingHit.name : pendingHit.title;
        if (pendingAction == Action::Install) {
          if (canLaunchPackage(pendingHit)) {
            toastWithAction(title + " installed", "Open", [self = shared_from_this(), hit = pendingHit] {
              self->launchPackage(hit);
            });
            gtk_label_set_text(status, (title + " is ready to open.").c_str());
          } else {
            toast((title + " installed").c_str());
            gtk_label_set_text(status, (title + " was installed.").c_str());
          }
        } else if (pendingAction == Action::Remove) {
          toast((title + " removed").c_str());
          gtk_label_set_text(status, (title + " was removed.").c_str());
        } else {
          toast((title + " updated").c_str());
          gtk_label_set_text(status, (title + " is up to date.").c_str());
        }
        if (detailing && openDetails.hit.name == pendingHit.name) {
          replaceOpenDetailPage();
        } else if (page == "installed" || page == "updates") {
          clearInstalledIndex();
          load();
        } else if (catalogLoaded && page == "browse") {
          overlayInstalled(catalog);
          // Rebuild store home or search grid so Installed state and shelves stay current.
          if (storeHomeActive()) applyCatalogFilter();
          else results();
          setStatusReady();
        }
        refreshInstalledIndex();
        controls();
        g_application_release(G_APPLICATION(app));
        scheduleBadgeRefresh();
        afterMutationSettled();
        return;
      }
      if (changing && !result.success) {
        const auto message = friendlyPackageError(result);
        recordHistory(false, message);
        toast(message.c_str());
        gtk_label_set_text(status, message.c_str());
        if (pendingRemoteMutation) {
          pendingRemoteMutation = false;
          if (remotesPageOpen) replaceRemotesPage();
        } else if (detailing && openDetails.hit.name == pendingHit.name) {
          replaceOpenDetailPage();
        }
        afterMutationSettled();
      } else if (!result.success && !changing) {
        failed = !result.cancelled;
        results();
        const auto message = friendlyPackageError(result);
        gtk_label_set_text(status, message.c_str());
        if (!result.cancelled && !result.error.empty()) toast(message.c_str());
      } else if (result.success && !changing) {
        hits = std::move(result.hits);
        listPage = 0;
        if (page == "updates") {
          annotateUpdateVersions(hits);
          pageCache = hits;
          notePendingUpdates(kind(), hits.size());
          setUpdatesBadge(pendingUpdateCount());
          scheduleBadgeRefresh();
        }
        if (page == "browse" && kind() == Kind::Flatpak) {
          catalog = hits;
          catalogLoaded = true;
          overlayInstalled(catalog);
          applyCatalogFilter();
          refreshCategoryIndex();
          if (!installedIndexReady) refreshInstalledIndex();
          scheduleBadgeRefresh();
          tryOpenPendingApp();
        } else {
          if (page == "installed") {
            if (kind() == Kind::Fedora) {
              std::ranges::stable_sort(hits, [](const PackageHit& left, const PackageHit& right) {
                const auto& leftKey = left.title.empty() ? left.name : left.title;
                const auto& rightKey = right.title.empty() ? right.name : right.title;
                return leftKey < rightKey;
              });
            } else {
              helix::software::sortByStoreScore(hits);
              std::ranges::stable_sort(hits, [](const PackageHit& left, const PackageHit& right) {
                const auto& leftKey = left.title.empty() ? left.name : left.title;
                const auto& rightKey = right.title.empty() ? right.name : right.title;
                return leftKey < rightKey;
              });
            }
            rememberInstalled(hits);
            pageCache = hits;
          } else if (page != "updates") {
            pageCache = hits;
          }
          overlayInstalled(hits);
          query = gtk_editable_get_text(GTK_EDITABLE(search));
          if (page == "installed" && kind() == Kind::Fedora) {
            applyRpmScopeFilter();
          } else if (!query.empty() && (page == "installed" || page == "updates")) {
            applyPageCacheFilter();
          } else {
            results();
            setStatusReady();
          }
        }
      } else {
        gtk_label_set_text(status, result.cancelled ? "Request canceled. Refresh to check the current package state." : "The software backend could not complete this request. Refresh and retry.");
      }
      clearProgress();
      controls();
      if (!opQueue.empty() && !noctalia::packages::busy()) {
        afterMutationSettled();
      } else {
        tryOpenPendingApp();
      }
    }
    g_application_release(G_APPLICATION(app));
  }
  CommandCompletion completion() {
    auto self = shared_from_this();
    const auto generation = listGeneration;
    const auto expectedKind = kind();
    const auto expectedPage = page;
    const auto expectedQuery = query;
    return [self, generation, expectedKind, expectedPage, expectedQuery](CommandResult result) {
      if (result.success) {
        if (expectedKind == Kind::Fedora && expectedPage == "browse" && !expectedQuery.empty()) {
          rankRpmSearchHits(expectedQuery, result.hits);
        } else {
          sortHits(result.hits);
        }
      }
      queue([self, result = std::move(result), generation, expectedKind, expectedPage]() mutable {
        self->finish(std::move(result), generation, expectedKind, expectedPage);
      });
    };
  }
  void refreshInstalledIndex() {
    // Overlay indexing must not take g_busy — details/mutations stay usable while browse is open.
    if (closing || kind() != Kind::Flatpak || !flatpakAvailable() || indexingInstalled) return;
    indexingInstalled = true;
    controls();
    auto self = shared_from_this();
    process::RunOptions options;
    options.timeout = std::chrono::seconds(20);
    options.maxOutputBytes = 4U << 20;
    if (!process::runAsync(
            {std::string("flatpak"), "list", "--app", "--columns",
             "application:f,name:f,version:f,branch:f,origin:f,installation:f"},
            process::RunCallbacks{
                .onExit =
                    [self](process::RunResult result) {
                      CommandResult out;
                      out.success = result.exitCode == 0 && !result.timedOut;
                      if (out.success) {
                        out.hits = parseFlatpakListOutput(result.out, FlatpakListKind::Installed);
                        attachFlatpakIcons(out.hits);
                      }
                      queue([self, out = std::move(out)]() mutable {
                        self->indexingInstalled = false;
                        if (self->closing) return;
                        if (out.success) {
                          self->rememberInstalled(out.hits);
                          if (self->catalogLoaded) self->overlayInstalled(self->catalog);
                          self->overlayInstalled(self->hits);
                          if (self->page == "browse" && self->catalogLoaded) self->applyCatalogFilter();
                          else if (self->page == "browse" || self->page == "installed") {
                            self->results();
                            self->setStatusReady();
                          }
                        }
                        self->controls();
                      });
                    },
            },
            options
        )) {
      indexingInstalled = false;
      controls();
    }
  }
  void annotateUpdateVersions(std::vector<PackageHit>& list) const {
    if (!installedIndexReady) return;
    for (auto& hit : list) {
      const auto it = installedIndex.find(hit.name);
      if (it == installedIndex.end()) continue;
      if (!it->second.version.empty() && !hit.version.empty() && it->second.version != hit.version) {
        hit.info = it->second.version + " → " + hit.version;
      } else if (!hit.version.empty()) {
        hit.info = hit.version;
      }
    }
  }
  void selectListSort(const std::string& sort) {
    if (creating || closing) return;
    const auto next = sort == "name" ? "name" : "popular";
    if (listSort == next) return;
    listSort = next;
    listPage = 0;
    helix::software::applyListSort(hits, listSort);
    results();
    setStatusReady();
  }
  void applyCatalogFilter() {
    query = gtk_editable_get_text(GTK_EDITABLE(search));
    hits.clear();
    for (const auto& hit : catalog) {
      if (!matchesCategory(hit)) continue;
      if (helix::software::matchesQuery(hit, query)) hits.push_back(hit);
    }
    helix::software::applyListSort(hits, listSort);
    overlayInstalled(hits);
    listPage = 0;
    failed = false;
    results();
    setStatusReady();
    syncCategoryButtons();
  }
  void applyPageCacheFilter() {
    query = gtk_editable_get_text(GTK_EDITABLE(search));
    if (page == "installed" && kind() == Kind::Fedora) {
      applyRpmScopeFilter();
      return;
    }
    hits.clear();
    for (const auto& hit : pageCache) {
      if (helix::software::matchesQuery(hit, query)) hits.push_back(hit);
    }
    helix::software::applyListSort(hits, listSort);
    listPage = 0;
    failed = false;
    results();
    setStatusReady();
  }
  void scheduleCatalogFilter() {
    if (searchTimer) g_source_remove(searchTimer);
    auto* self = this;
    searchTimer = g_timeout_add(180, +[](gpointer data) -> gboolean {
      auto* software = static_cast<Software*>(data);
      software->searchTimer = 0;
      if (software->closing) return G_SOURCE_REMOVE;
      if (software->canFilterCachedCatalog()) software->applyCatalogFilter();
      else if (software->canFilterPageCache()) software->applyPageCacheFilter();
      return G_SOURCE_REMOVE;
    }, self);
  }
  void refreshCategoryIndex() {
    if (closing || kind() != Kind::Flatpak || !catalogLoaded || categoriesReady) return;
    gtk_label_set_text(status, "Preparing store categories…");
    const auto generation = catalogGeneration;
    auto self = shared_from_this();
    struct TaskData {
      std::shared_ptr<Software> self;
      unsigned generation = 0;
    };
    auto* task = g_task_new(nullptr, nullptr, +[](GObject*, GAsyncResult* result, gpointer) {
      auto* task = G_TASK(result);
      auto* index = static_cast<FlatpakCatalogIndex*>(g_task_propagate_pointer(task, nullptr));
      auto* data = static_cast<TaskData*>(g_task_get_task_data(task));
      if (!index || !data || !data->self || data->self->closing || data->self->catalogGeneration != data->generation) {
        delete index;
        return;
      }
      auto& software = *data->self;
      attachFlatpakCatalogIndex(software.catalog, *index);
      attachFlatpakCatalogIndex(software.hits, *index);
      software.categoriesReady = true;
      delete index;
      if (software.page == "browse" && software.kind() == Kind::Flatpak) {
        software.applyCatalogFilter();
      }
      software.setStatusReady();
      software.tryOpenPendingApp();
    }, nullptr);
    g_task_set_task_data(task, new TaskData{self, generation}, +[](gpointer p) { delete static_cast<TaskData*>(p); });
    g_task_run_in_thread(task, +[](GTask* task, gpointer, gpointer, GCancellable*) {
      g_task_return_pointer(
          task, new FlatpakCatalogIndex(loadFlatpakCatalogIndex()),
          +[](gpointer p) { delete static_cast<FlatpakCatalogIndex*>(p); }
      );
    });
    g_object_unref(task);
  }
  void selectCategory(const std::string& category) {
    if (creating || closing) return;
    categoryFilter = category;
    listPage = 0;
    if (canFilterCachedCatalog()) applyCatalogFilter();
    else syncCategoryButtons();
  }
  void load() {
    if (creating || closing || sources.empty() || detailing) return;
    if (listBusy()) return;
    if (operationBusy()) {
      if (page == "browse" && kind() == Kind::Flatpak && catalogLoaded) {
        applyCatalogFilter();
        return;
      }
      if (canFilterPageCache()) {
        applyPageCacheFilter();
        return;
      }
      if (page == "browse" && query.empty() && kind() == Kind::Fedora) {
        hits.clear();
        results();
        return;
      }
      toast("Wait for the current software operation to finish.");
      return;
    }
    if (selectedKind == Kind::Firmware && page == "browse") {
      page = "updates"; creating = true;
      adw_view_stack_set_visible_child_name(stack, page.c_str());
      creating = false;
    }
    query = gtk_editable_get_text(GTK_EDITABLE(search));

    if (page == "browse" && kind() == Kind::Flatpak && catalogLoaded) {
      applyCatalogFilter();
      return;
    }
    if (page == "browse" && query.empty() && kind() == Kind::Fedora) {
      switchingSource = false;
      hits.clear();
      pageCache.clear();
      results();
      return;
    }
    if (page == "browse" && !query.empty() && kind() != Kind::Flatpak && !isSafeSearchQuery(query)) {
      toast("Use a short search without control characters or shell punctuation.");
      return;
    }
    begin(false);
    bool accepted = false;
    if (page == "installed") accepted = listInstalled(completion(), kind());
    else if (page == "updates") accepted = listUpdates(completion(), kind());
    else if (kind() == Kind::Flatpak) {
      accepted = listCatalog(completion(), kind());
      if (accepted) clearCatalogCache();
    } else accepted = noctalia::packages::search(query, completion(), kind());
    if (!accepted) finish({.error = "Backend unavailable or busy"});
  }
  bool openDetail(PackageHit hit) {
    if (listBusy() || closing || detailing || replacingDetail || sources.empty()) return false;
    if (operationBusy()) {
      toast("Wait for the current software operation to finish.");
      return false;
    }
    detailing = true; controls();
    auto* loading = helix::software::buildLoadingDetailPage(hit);
    adw_navigation_view_push(navigation, loading);
    begin(false, false);
    gtk_label_set_text(status, "Loading package details…");
    auto self = shared_from_this();
    const auto selected = kind();
    const auto requestedId = hit.name;
    if (!getDetails(hit, [self, selected, loading, requestedId](CommandResult result) {
      queue([self, selected, loading, requestedId, result = std::move(result)]() mutable {
        self->busy = false; self->mutation = false;
        g_application_release(G_APPLICATION(self->app));
        if (self->closing) return;
        if (!result.success) {
          self->detailing = false;
          self->toast(result.cancelled ? "Detail request canceled." : "Could not load package details.");
          if (adw_navigation_view_get_visible_page(self->navigation) == loading) {
            self->replacingDetail = true;
            adw_navigation_view_pop(self->navigation);
            self->replacingDetail = false;
          }
          self->controls();
          gtk_label_set_text(self->status, result.cancelled ? "Request canceled. Refresh to check the current package state." : "Package details unavailable.");
          if (result.cancelled && self->pendingOpenApp.empty()) {
            self->pendingOpenApp = requestedId;
          }
          if (!self->pendingOpenApp.empty()) self->tryOpenPendingApp();
          return;
        }
        self->openDetails = std::move(result.details);
        if (self->installedIndexReady) {
          std::vector<PackageHit> one{self->openDetails.hit};
          self->overlayInstalled(one);
          self->openDetails.hit = one.front();
        }
        auto* page = helix::software::buildDetailPage(
            self->openDetails, selected,
            [self](PackageHit package, Action action) { self->confirm(package, action); },
            [self](const PackageHit& package) { self->launchPackage(package); },
            !self->mutation && !self->busy, self->canLaunchPackage(self->openDetails.hit)
        );
        if (adw_navigation_view_get_visible_page(self->navigation) == loading) {
          self->replacingDetail = true;
          adw_navigation_view_pop(self->navigation);
          adw_navigation_view_push(self->navigation, page);
          self->replacingDetail = false;
        } else {
          adw_navigation_view_push(self->navigation, page);
        }
        if (self->pendingOpenApp == requestedId || self->pendingOpenApp == self->openDetails.hit.name) {
          self->pendingOpenApp.clear();
        }
        self->detailing = true;
        gtk_label_set_text(self->status, "Package details are ready.");
        self->controls();
      });
    }, selected)) {
      busy = false; detailing = false; g_application_release(G_APPLICATION(app));
      toast("Package details unavailable.");
      replacingDetail = true;
      adw_navigation_view_pop(navigation);
      replacingDetail = false;
      controls();
      return false;
    }
    return true;
  }
  void launchPackage(const PackageHit& hit) {
    if (closing || hit.name.empty()) return;
    if (kind() == Kind::Flatpak && isValidFlatpakAppId(hit.name)) {
      std::vector<std::string> args = {std::string("flatpak"), "run"};
      if (hit.installation == "system") args.emplace_back("--system");
      else args.emplace_back("--user");
      args.push_back(hit.name);
      if (process::runAsync(args)) {
        toast("Opening application…");
        return;
      }
    }
    const auto desktopId = hit.name + ".desktop";
    if (auto* info = g_desktop_app_info_new(desktopId.c_str())) {
      GError* error = nullptr;
      g_app_info_launch(G_APP_INFO(info), nullptr, nullptr, &error);
      g_object_unref(info);
      if (!error) {
        toast("Opening application…");
        return;
      }
      g_error_free(error);
    }
    toast("Could not open this application.");
  }
  void confirmAll() {
    if (listBusy() || confirming || hits.empty()) return;
    confirming = true;
    const auto selected = kind();
    std::vector<std::string> packageRefs;
    packageRefs.reserve(hits.size());
    for (const auto& hit : hits) {
      if (selected == Kind::Fedora) {
        if (auto ref = pkconPackageRef(hit); !ref.empty()) packageRefs.push_back(std::move(ref));
      } else {
        packageRefs.push_back(hit.name);
      }
    }
    if (packageRefs.empty()) {
      confirming = false;
      toast("No updatable packages in this list.");
      return;
    }
    const auto description = std::to_string(packageRefs.size()) + " listed updates from the selected source will be applied.";
    auto* dialog = adw_alert_dialog_new("Apply All Listed Updates?", description.c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "apply", "Update", nullptr);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "apply", ADW_RESPONSE_SUGGESTED);
    struct Request { std::shared_ptr<Software> self; std::vector<std::string> packageRefs; Kind kind; };
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window), nullptr, +[](GObject* source, GAsyncResult* result, gpointer data) {
      std::unique_ptr<Request> request(static_cast<Request*>(data));
      auto self = request->self;
      self->confirming = false;
      if (std::string(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result)) != "apply" || self->closing) return;
      QueuedOp op;
      op.action = Action::Update;
      op.hit.title = "Updates";
      op.hit.name = "updates";
      op.kind = request->kind;
      op.bulk = true;
      op.packageRefs = std::move(request->packageRefs);
      if (self->mutation || noctalia::packages::busy()) {
        self->enqueueOp(std::move(op));
        return;
      }
      self->startQueuedOp(std::move(op));
    }, new Request{shared_from_this(), std::move(packageRefs), selected});
  }
  void replaceRemotesPage() {
    if (closing || !remotesPageOpen) return;
    auto* page = helix::software::buildRemotesPage(
        flatpakRemotes, remotesLoading, !operationBusy() && !listBusy(),
        [self = shared_from_this()] { self->loadRemotes(); },
        [self = shared_from_this()] { self->confirmRemoteAction({}, FlatpakRemoteAction::AddFlathub); },
        [self = shared_from_this()](FlatpakRemote remote, FlatpakRemoteAction action) {
          self->confirmRemoteAction(std::move(remote), action);
        }
    );
    if (auto* visible = adw_navigation_view_get_visible_page(navigation); visible
        && g_list_model_get_n_items(adw_navigation_view_get_navigation_stack(navigation)) > 1) {
      adw_navigation_view_pop(navigation);
    }
    adw_navigation_view_push(navigation, ADW_NAVIGATION_PAGE(page));
    remotesPageOpen = true;
  }
  void loadRemotes() {
    if (closing || remotesLoading || !flatpakAvailable()) return;
    if (operationBusy() || listBusy()) {
      toast("Wait for the current software operation to finish.");
      return;
    }
    remotesLoading = true;
    if (remotesPageOpen) replaceRemotesPage();
    begin(false, false);
    gtk_label_set_text(status, "Loading software sources…");
    auto self = shared_from_this();
    if (!listFlatpakRemotes([self](CommandResult result) {
          queue([self, result = std::move(result)]() mutable {
            self->busy = false;
            self->mutation = false;
            self->remotesLoading = false;
            g_application_release(G_APPLICATION(self->app));
            if (self->closing) return;
            if (!result.success) {
              const auto message = result.cancelled ? std::string("Sources request canceled.") : friendlyPackageError(result);
              self->toast(message.c_str());
              if (self->remotesPageOpen) self->replaceRemotesPage();
              self->controls();
              return;
            }
            self->flatpakRemotes = std::move(result.remotes);
            if (self->remotesPageOpen) self->replaceRemotesPage();
            gtk_label_set_text(self->status, "Software sources are ready.");
            self->controls();
            // Remote changes clear the catalog; reload Explore once sources are listed.
            if (self->page == "browse" && self->kind() == Kind::Flatpak && !self->catalogLoaded
                && !self->operationBusy()) {
              self->load();
            }
          });
        })) {
      remotesLoading = false;
      busy = false;
      g_application_release(G_APPLICATION(app));
      toast("Could not list Flatpak remotes.");
      controls();
    }
  }
  void openSourcesPage() {
    if (closing || !flatpakAvailable()) return;
    if (operationBusy() || listBusy()) {
      toast("Wait for the current software operation to finish.");
      return;
    }
    if (detailing) popToHome();
    remotesPageOpen = true;
    remotesLoading = flatpakRemotes.empty();
    replaceRemotesPage();
    if (flatpakRemotes.empty()) loadRemotes();
  }
  void confirmRemoteAction(FlatpakRemote remote, FlatpakRemoteAction action) {
    if (closing || confirming || operationBusy() || listBusy()) {
      if (operationBusy()) toast("Another software operation is already running.");
      return;
    }
    confirming = true;
    std::string heading;
    std::string detail;
    const char* verb = "Apply";
    if (action == FlatpakRemoteAction::AddFlathub) {
      heading = "Add Flathub?";
      detail = "Adds the Flathub remote for your user account so Explore can install apps from flathub.org.";
      verb = "Add";
    } else if (action == FlatpakRemoteAction::Delete) {
      heading = "Remove " + (remote.title.empty() ? remote.name : remote.title) + "?";
      detail = remote.name + " (" + remote.installation + ")\nApps from this remote may disappear from Explore until it is added again.";
      verb = "Remove";
    } else if (action == FlatpakRemoteAction::Enable) {
      heading = "Enable " + (remote.title.empty() ? remote.name : remote.title) + "?";
      detail = remote.name + " (" + remote.installation + ")\nExplore will include apps from this remote after refresh.";
      verb = "Enable";
    } else {
      heading = "Disable " + (remote.title.empty() ? remote.name : remote.title) + "?";
      detail = remote.name + " (" + remote.installation + ")\nExplore will hide apps that come only from this remote.";
      verb = "Disable";
    }
    auto* dialog = adw_alert_dialog_new(heading.c_str(), detail.c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "apply", verb, nullptr);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_response_appearance(
        ADW_ALERT_DIALOG(dialog), "apply",
        action == FlatpakRemoteAction::Delete ? ADW_RESPONSE_DESTRUCTIVE : ADW_RESPONSE_SUGGESTED
    );
    struct Request {
      std::shared_ptr<Software> self;
      FlatpakRemote remote;
      FlatpakRemoteAction action = FlatpakRemoteAction::Enable;
    };
    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window), nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<Request> request(static_cast<Request*>(data));
          auto self = request->self;
          self->confirming = false;
          if (std::string(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result)) != "apply"
              || self->closing) {
            if (self->remotesPageOpen) self->replaceRemotesPage();
            return;
          }
          if (self->operationBusy() || self->listBusy()) {
            self->toast("Another software operation is already running.");
            return;
          }
          self->pendingAction = Action::Update;
          self->pendingRemoteMutation = true;
          self->pendingRemoteAction = request->action;
          self->pendingHit = {};
          self->pendingHit.name = request->remote.name.empty() ? "flathub" : request->remote.name;
          self->pendingHit.title = request->action == FlatpakRemoteAction::AddFlathub
              ? "Flathub"
              : (request->remote.title.empty() ? request->remote.name : request->remote.title);
          self->begin(true);
          if (!mutateFlatpakRemote(
                  request->action, request->remote, self->completion(), self->mutationProgressCallback()
              )) {
            self->busy = false;
            self->mutation = false;
            self->pendingRemoteMutation = false;
            g_application_release(G_APPLICATION(self->app));
            self->clearProgress();
            self->toast("Could not start the software source operation.");
            self->controls();
            if (self->remotesPageOpen) self->replaceRemotesPage();
          }
        },
        new Request{shared_from_this(), std::move(remote), action}
    );
  }
  void confirm(const PackageHit& hit, Action action) {
    if (listBusy() || confirming || sources.empty()) return;
    if (kind() == Kind::Fedora && action == Action::Remove && isProtectedPackage(hit.name)) {
      toast("This system package is protected and cannot be removed.");
      return;
    }
    confirming = true;
    const auto selectedKind = kind();
    const char* verb = action == Action::Install ? "Install" : action == Action::Remove ? "Remove" : "Update";
    const auto title = hit.title.empty() ? hit.name : hit.title;
    const auto heading = std::string(verb) + " " + title + "?";
    std::string detail = hit.name;
    detail += selectedKind == Kind::Flatpak ? "\nFlatpak application"
        : selectedKind == Kind::Firmware   ? "\nDevice firmware"
                                           : "\nFedora system package";
    if (!hit.version.empty()) detail += "\nVersion: " + hit.version;
    if (openDetails.hit.name == hit.name) {
      if (!openDetails.verification.empty()) detail += "\n" + openDetails.verification;
      if (const auto size = helix::software::friendlySize(openDetails.downloadSize); !size.empty()) {
        detail += "\nDownload size: " + size;
      }
      if (const auto installed = helix::software::friendlySize(openDetails.installedSize); !installed.empty()
          && (selectedKind == Kind::Fedora || action == Action::Remove)) {
        detail += "\nInstalled size: " + installed;
      }
      if (!openDetails.runtime.empty()) detail += "\nRuntime: " + openDetails.runtime;
      if (selectedKind == Kind::Flatpak && action == Action::Install) {
        if (const auto safety = helix::software::permissionSafetySummary(openDetails.permissions); !safety.empty()) {
          detail += "\n" + safety;
        }
      }
    }
    if (selectedKind == Kind::Fedora && action == Action::Install) {
      detail += "\nMay install required dependencies and ask for authentication.";
    } else if (selectedKind == Kind::Fedora && action == Action::Remove) {
      detail += "\nRemoving a system package can affect other software.";
    } else if (selectedKind == Kind::Flatpak && action == Action::Remove) {
      detail += "\nChoose Remove All Data to also delete this app’s saved settings and files.";
    }
    auto* dialog = adw_alert_dialog_new(heading.c_str(), detail.c_str());
    if (selectedKind == Kind::Flatpak && action == Action::Remove) {
      adw_alert_dialog_add_responses(
          ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "apply", "Remove", "delete", "Remove All Data", nullptr
      );
      adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "apply", ADW_RESPONSE_DESTRUCTIVE);
      adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
    } else {
      adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "apply", verb, nullptr);
      adw_alert_dialog_set_response_appearance(
          ADW_ALERT_DIALOG(dialog), "apply", action == Action::Remove ? ADW_RESPONSE_DESTRUCTIVE : ADW_RESPONSE_SUGGESTED
      );
    }
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    struct Request { std::shared_ptr<Software> self; PackageHit hit; Action action; Kind kind; };
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window), nullptr, +[](GObject* source, GAsyncResult* result, gpointer data) {
      std::unique_ptr<Request> request(static_cast<Request*>(data));
      auto self = request->self;
      self->confirming = false;
      const auto response = std::string(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result));
      if (self->closing || (response != "apply" && response != "delete")) return;
      QueuedOp op;
      op.action = request->action;
      op.hit = request->hit;
      op.kind = request->kind;
      op.deleteData = response == "delete";
      if (self->mutation || noctalia::packages::busy()) {
        self->enqueueOp(std::move(op));
        return;
      }
      self->startQueuedOp(std::move(op));
    }, new Request{shared_from_this(), hit, action, selectedKind});
  }
  void create() {
    creating = true; closing = false;
    if (window) g_object_unref(window);
    window = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app))); g_object_ref(window);
    appearance = std::make_unique<helix::apps::AppearanceBinding>(GTK_WIDGET(window));
    gtk_window_set_title(GTK_WINDOW(window), "Helix Software");
    gtk_window_set_default_size(GTK_WINDOW(window), 1180, 820);
    gtk_widget_set_size_request(GTK_WIDGET(window), 320, 360);
    g_signal_connect(window, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean { static_cast<Software*>(data)->closing = true; return false; }), this);
    toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    navigation = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
    auto* toolbar = adw_toolbar_view_new();
    auto* header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    auto* options = gtk_string_list_new(nullptr); sources.clear();
    if (flatpakAvailable()) { sources.push_back(Kind::Flatpak); gtk_string_list_append(options, "Flatpak"); }
    if (backendAvailable() && process::commandExists("rpm")) { sources.push_back(Kind::Fedora); gtk_string_list_append(options, "Fedora"); }
    if (firmwareAvailable()) { sources.push_back(Kind::Firmware); gtk_string_list_append(options, "Firmware"); }
    if (sources.empty()) gtk_string_list_append(options, "Unavailable");
    source = gtk_drop_down_new(G_LIST_MODEL(options), nullptr); g_object_unref(options);
    if (!sources.empty()) {
      const auto selected = std::ranges::find(sources, selectedKind);
      const auto index = selected == sources.end() ? 0u : guint(selected - sources.begin());
      selectedKind = sources[index]; gtk_drop_down_set_selected(GTK_DROP_DOWN(source), index);
    }
    gtk_widget_set_valign(source, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(source, 120, -1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(source), GTK_ACCESSIBLE_PROPERTY_LABEL, "Software source", -1);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), source);

    search = gtk_search_entry_new();
    gtk_widget_set_hexpand(search, true);
    gtk_widget_set_size_request(search, 240, -1);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search), "Search apps");
    gtk_accessible_update_property(GTK_ACCESSIBLE(search), GTK_ACCESSIBLE_PROPERTY_LABEL, "Search software", -1);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), search);
    refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh, "Refresh");
    signal(refresh, "clicked", [this] { clearCatalogCache(); clearInstalledIndex(); load(); });
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), refresh);
    cancel = gtk_button_new_from_icon_name("process-stop-symbolic");
    gtk_widget_set_tooltip_text(cancel, "Cancel the current install, update, or refresh");
    signal(cancel, "clicked", [this] {
      const auto queued = opQueue.size();
      opQueue.clear();
      cancelCurrent();
      if (queued > 0) {
        toast(queued == 1 ? "Canceled. Cleared 1 queued operation."
                          : ("Canceled. Cleared " + std::to_string(queued) + " queued operations.").c_str());
      }
    });
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), cancel);
    sourcesBtn = gtk_button_new_from_icon_name("network-server-symbolic");
    gtk_widget_set_tooltip_text(sourcesBtn, "Software sources");
    gtk_accessible_update_property(GTK_ACCESSIBLE(sourcesBtn), GTK_ACCESSIBLE_PROPERTY_LABEL, "Software sources", -1);
    signal(sourcesBtn, "clicked", [this] { openSourcesPage(); });
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), sourcesBtn);

    categoriesBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(categoriesBox, 12); gtk_widget_set_margin_end(categoriesBox, 12);
    gtk_widget_set_hexpand(categoriesBox, true);
    categoryScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(categoryScroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(categoryScroll), categoriesBox);
    gtk_widget_set_visible(categoryScroll, false);
    categoryButtons.clear();
    GtkToggleButton* group = nullptr;
    for (const auto& [id, label] : std::vector<std::pair<const char*, const char*>>{
             {"", "All"}, {"Game", "Games"}, {"Development", "Development"}, {"Graphics", "Graphics"},
             {"Office", "Office"}, {"Network", "Network"}, {"AudioVideo", "Media"}, {"Utility", "Utilities"},
             {"Education", "Education"}, {"System", "System"}, {"Science", "Science"}, {"Security", "Security"}}) {
      auto* chip = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(label));
      if (!group) group = chip;
      else gtk_toggle_button_set_group(chip, group);
      gtk_widget_add_css_class(GTK_WIDGET(chip), "pill");
      const std::string key = id;
      categoryButtons[key] = chip;
      signal(GTK_WIDGET(chip), "toggled", [this, key, chip] {
        if (creating || !gtk_toggle_button_get_active(chip)) return;
        selectCategory(key);
      });
      gtk_box_append(GTK_BOX(categoriesBox), GTK_WIDGET(chip));
    }
    gtk_box_append(GTK_BOX(root), categoryScroll);
    rpmScopeBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(rpmScopeBox, 12); gtk_widget_set_margin_end(rpmScopeBox, 12);
    gtk_widget_set_hexpand(rpmScopeBox, true);
    rpmScopeScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(rpmScopeScroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(rpmScopeScroll), rpmScopeBox);
    gtk_widget_set_visible(rpmScopeScroll, false);
    rpmScopeButtons.clear();
    GtkToggleButton* rpmGroup = nullptr;
    for (const auto& [id, label] : std::vector<std::pair<const char*, const char*>>{
             {"apps", "Apps"}, {"libs", "Libraries"}, {"devel", "Devel"}, {"all", "All"}}) {
      auto* chip = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(label));
      if (!rpmGroup) rpmGroup = chip;
      else gtk_toggle_button_set_group(chip, rpmGroup);
      gtk_widget_add_css_class(GTK_WIDGET(chip), "pill");
      const std::string key = id;
      rpmScopeButtons[key] = chip;
      if (key == rpmScope) gtk_toggle_button_set_active(chip, true);
      signal(GTK_WIDGET(chip), "toggled", [this, key, chip] {
        if (creating || !gtk_toggle_button_get_active(chip)) return;
        selectRpmScope(key);
      });
      gtk_box_append(GTK_BOX(rpmScopeBox), GTK_WIDGET(chip));
    }
    gtk_box_append(GTK_BOX(root), rpmScopeScroll);
    status = GTK_LABEL(gtk_label_new("Explore apps from your software sources."));
    gtk_label_set_wrap(status, true); gtk_label_set_xalign(status, 0);
    gtk_widget_add_css_class(GTK_WIDGET(status), "dim-label");
    gtk_widget_set_margin_start(GTK_WIDGET(status), 18); gtk_widget_set_margin_end(GTK_WIDGET(status), 18);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(status));
    stack = ADW_VIEW_STACK(adw_view_stack_new()); gtk_widget_set_vexpand(GTK_WIDGET(stack), true); bodies.clear(); grids.clear();
    for (const auto& [id, label, icon] : std::vector<std::tuple<const char*, const char*, const char*>>{
             {"browse", "Explore", "system-software-install-symbolic"},
             {"installed", "Library", "application-x-executable-symbolic"},
             {"updates", "Updates", "software-update-available-symbolic"}}) {
      auto* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
      for (auto side : {"margin-start", "margin-end", "margin-top", "margin-bottom"}) g_object_set(body, side, 18, nullptr);
      auto* grid = helix::software::createPackageGrid();
      gtk_box_append(GTK_BOX(body), GTK_WIDGET(grid));
      auto* clamp = adw_clamp_new();
      adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 1120);
      adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 720);
      adw_clamp_set_child(ADW_CLAMP(clamp), body);
      auto* scroll = gtk_scrolled_window_new();
      gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
      gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
      auto* view = adw_view_stack_add_titled_with_icon(stack, scroll, id, label, icon);
      if (std::string_view(id) == "browse") browsePage = view;
      if (std::string_view(id) == "updates") updatesPage = view;
      bodies[id] = GTK_BOX(body); grids[id] = grid;
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
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(stack)); adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    auto* home = adw_navigation_page_new(toolbar, "Helix Software");
    adw_navigation_view_push(navigation, home);
    // Keep the operation banner above the navigation stack so installs stay visible on detail pages.
    auto* shell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    progress = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_widget_set_margin_start(GTK_WIDGET(progress), 18);
    gtk_widget_set_margin_end(GTK_WIDGET(progress), 18);
    gtk_widget_set_visible(GTK_WIDGET(progress), false);
    gtk_box_append(GTK_BOX(shell), GTK_WIDGET(progress));
    gtk_widget_set_vexpand(GTK_WIDGET(navigation), true);
    gtk_box_append(GTK_BOX(shell), GTK_WIDGET(navigation));
    adw_toast_overlay_set_child(toasts, shell);
    adw_application_window_set_content(window, GTK_WIDGET(toasts));
    {
      auto* onPopped = new Callback([this] {
        if (replacingDetail) return;
        auto* stackModel = adw_navigation_view_get_navigation_stack(navigation);
        if (!stackModel || g_list_model_get_n_items(stackModel) <= 1) {
          detailing = false;
          remotesPageOpen = false;
          controls();
          // If a catalog load was abandoned while browsing detail, recover Explore.
          if (kind() == Kind::Flatpak && page == "browse" && !catalogLoaded && !listBusy() && !closing) {
            load();
          } else if (kind() == Kind::Flatpak && page == "browse" && catalogLoaded && storeHomeActive()) {
            applyCatalogFilter();
          }
          tryOpenPendingApp();
        }
      });
      g_signal_connect_data(
          navigation, "popped",
          G_CALLBACK(+[](AdwNavigationView*, AdwNavigationPage*, gpointer data) { (*static_cast<Callback*>(data))(); }),
          onPopped, +[](gpointer data, GClosure*) { delete static_cast<Callback*>(data); }, G_CONNECT_DEFAULT
      );
    }
    signal(source, "notify::selected", [this] {
      if (creating || sources.empty()) return;
      selectedKind = sources.at(gtk_drop_down_get_selected(GTK_DROP_DOWN(source)));
      invalidateListRequests();
      clearCatalogCache();
      clearInstalledIndex();
      pageCache.clear();
      hits.clear();
      listPage = 0;
      rpmScope = "apps";
      switchingSource = true;
      failed = false;
      syncSearchPlaceholder();
      results();
      if (busy && !mutation) {
        // Wait for cancel to clear backend busy, then load the new source.
        g_timeout_add(200, +[](gpointer data) -> gboolean {
          auto* software = static_cast<Software*>(data);
          if (software->closing) return G_SOURCE_REMOVE;
          if (noctalia::packages::busy()) return G_SOURCE_CONTINUE;
          software->busy = false;
          software->switchingSource = false;
          software->controls();
          software->load();
          return G_SOURCE_REMOVE;
        }, this);
        return;
      }
      switchingSource = false;
      load();
    }, true);
    signal(stack, "notify::visible-child-name", [this] {
      const auto next = std::string(adw_view_stack_get_visible_child_name(stack));
      if (next != page) {
        // Tab changes start a fresh search context so Explore queries don't hide Installed/Updates.
        gtk_editable_set_text(GTK_EDITABLE(search), "");
        query.clear();
        if (next != "browse") {
          hits.clear();
          pageCache.clear();
          listPage = 0;
        }
      }
      page = next;
      gtk_widget_set_visible(search, true);
      gtk_widget_set_sensitive(search, page == "browse" || page == "installed" || page == "updates");
      syncSearchPlaceholder();
      if (!creating) {
        if (listBusy()) {
          // Retry once the in-flight list/detail request finishes.
          g_timeout_add(250, +[](gpointer data) -> gboolean {
            auto* software = static_cast<Software*>(data);
            if (software->closing || software->listBusy()) return G_SOURCE_CONTINUE;
            software->load();
            return G_SOURCE_REMOVE;
          }, this);
          results();
        } else {
          load();
        }
      }
    }, true);
    signal(search, "activate", [this] {
      if (canFilterCachedCatalog()) applyCatalogFilter();
      else if (canFilterPageCache()) applyPageCacheFilter();
      else load();
    });
    signal(search, "search-changed", [this] {
      if (canFilterCachedCatalog() || canFilterPageCache()) scheduleCatalogFilter();
    });
    auto* shortcuts = gtk_shortcut_controller_new();
    gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(shortcuts), GTK_SHORTCUT_SCOPE_LOCAL);
    auto addShortcut = [&](guint key, GdkModifierType mods, gboolean (*handler)(GtkWidget*, GVariant*, gpointer)) {
      gtk_shortcut_controller_add_shortcut(
          GTK_SHORTCUT_CONTROLLER(shortcuts),
          gtk_shortcut_new(gtk_keyval_trigger_new(key, mods), gtk_callback_action_new(handler, this, nullptr))
      );
    };
    auto popDetail = +[](GtkWidget*, GVariant*, gpointer data) -> gboolean {
      auto* software = static_cast<Software*>(data);
      if (!software || software->closing || !software->navigation) return false;
      auto* stackModel = adw_navigation_view_get_navigation_stack(software->navigation);
      if (stackModel && g_list_model_get_n_items(stackModel) > 1) {
        adw_navigation_view_pop(software->navigation);
        return true;
      }
      return false;
    };
    addShortcut(GDK_KEY_Escape, static_cast<GdkModifierType>(0), popDetail);
    addShortcut(GDK_KEY_Left, GDK_ALT_MASK, popDetail);
    addShortcut(GDK_KEY_f, GDK_CONTROL_MASK, +[](GtkWidget*, GVariant*, gpointer data) -> gboolean {
      auto* software = static_cast<Software*>(data);
      if (!software || software->closing || !software->search || !gtk_widget_get_visible(software->search)) return false;
      gtk_widget_grab_focus(software->search);
      return true;
    });
    addShortcut(GDK_KEY_F5, static_cast<GdkModifierType>(0), +[](GtkWidget*, GVariant*, gpointer data) -> gboolean {
      auto* software = static_cast<Software*>(data);
      if (!software || software->closing || software->listBusy() || software->detailing) return false;
      software->clearCatalogCache();
      software->clearInstalledIndex();
      software->load();
      return true;
    });
    gtk_widget_add_controller(GTK_WIDGET(window), shortcuts);

    creating = false; controls(); results();
    if (kind() == Kind::Flatpak && page == "browse") load();
    else scheduleBadgeRefresh();
  }
};
}
int main(int argc, char** argv) {
  auto* app = adw_application_new("org.helix.Software", G_APPLICATION_HANDLES_COMMAND_LINE);
  helix::apps::guardInspection(G_APPLICATION(app));
  auto software = std::make_shared<Software>(app);
  for (const auto& [id, description] : std::vector<std::pair<const char*, const char*>>{
           {"page", "Open browse, installed or updates"},
           {"source", "Select flatpak, packages or firmware"},
           {"search", "Search software"},
           {"app", "Open a Flatpak app id or Fedora package by name"},
           {"save-preview", "Save a PNG of the running window"}})
    g_application_add_main_option(G_APPLICATION(app), id, 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, description, "VALUE");
  g_application_add_main_option(G_APPLICATION(app), "inspect-layout", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, "Print running window state", nullptr);
  g_signal_connect(app, "activate", G_CALLBACK(+[](GApplication*, gpointer data) { auto& self = *static_cast<Software*>(data); if (!self.window || self.closing) self.create(); gtk_window_present(GTK_WINDOW(self.window)); }), software.get());
  g_signal_connect(app, "command-line", G_CALLBACK((+[](GApplication* application, GApplicationCommandLine* command, gpointer data) -> int {
    auto& self = *static_cast<Software*>(data); auto* options = g_application_command_line_get_options_dict(command); const gchar *page = nullptr, *source = nullptr, *query = nullptr, *appId = nullptr, *preview = nullptr; gboolean inspect = false;
    g_variant_dict_lookup(options, "page", "&s", &page); g_variant_dict_lookup(options, "source", "&s", &source); g_variant_dict_lookup(options, "search", "&s", &query); g_variant_dict_lookup(options, "app", "&s", &appId); g_variant_dict_lookup(options, "save-preview", "&s", &preview); g_variant_dict_lookup(options, "inspect-layout", "b", &inspect);
    if (preview) return self.window && helix::apps::savePreview(GTK_WIDGET(self.window), preview) ? 0 : 1;
    if (inspect && (!self.window || self.closing)) { g_application_command_line_printerr(command, "Open Software before inspecting its layout\n"); return 1; }
    if (inspect && self.window) {
      auto* navStack = self.navigation
          ? adw_navigation_view_get_navigation_stack(self.navigation)
          : nullptr;
      nlohmann::json state = {
          {"page", self.page},
          {"busy", self.busy},
          {"mutation", self.mutation},
          {"detailing", self.detailing},
          {"detail", self.detailing ? self.openDetails.hit.name : ""},
          {"navDepth", navStack ? int(g_list_model_get_n_items(navStack)) : 0},
          {"queue", self.opQueue.size()},
          {"history", self.history.size()},
          {"results", self.hits.size()},
          {"listPage", self.listPage},
          {"pageSize", helix::software::kListPageSize},
          {"pages", helix::software::listPageCount(self.hits.size())},
          {"catalog", self.catalog.size()},
          {"categoriesReady", self.categoriesReady},
          {"storeHome", self.storeHomeActive()},
          {"installedIndex", self.installedIndex.size()},
          {"query", self.query},
          {"category", self.categoryFilter},
          {"rpmScope", self.rpmScope},
          {"pageCache", self.pageCache.size()},
          {"pendingApp", self.pendingOpenApp},
          {"width", gtk_widget_get_width(GTK_WIDGET(self.window))},
          {"height", gtk_widget_get_height(GTK_WIDGET(self.window))},
          {"source", self.kind() == Kind::Flatpak ? "flatpak" : self.kind() == Kind::Firmware ? "firmware" : "packages"},
      };
      g_application_command_line_print(command, "%s\n", state.dump().c_str());
      return 0;
    }
    if ((page && std::string(page) != "browse" && std::string(page) != "installed" && std::string(page) != "updates") || (source && std::string(source) != "flatpak" && std::string(source) != "packages" && std::string(source) != "firmware")) { g_application_command_line_printerr(command, "Unknown software page or source\n"); return 2; }
    if (query && ((source && std::string(source) == "firmware") || (!source && self.selectedKind == Kind::Firmware))) { g_application_command_line_printerr(command, "Firmware does not support software search; open installed or updates\n"); return 2; }
    if (self.listBusy() && (page || source || query) && !appId) {
      g_application_command_line_printerr(command, "A software operation is in progress\n");
      return 1;
    }
    g_application_activate(application); self.creating = true;
    if (source) {
      const auto kind = std::string(source) == "flatpak" ? Kind::Flatpak : std::string(source) == "firmware" ? Kind::Firmware : Kind::Fedora;
      const auto it = std::ranges::find(self.sources, kind);
      if (it == self.sources.end()) {
        self.creating = false;
        g_application_command_line_printerr(command, "Software source unavailable\n");
        return 1;
      }
      gtk_drop_down_set_selected(GTK_DROP_DOWN(self.source), guint(it - self.sources.begin()));
      self.selectedKind = kind;
    }
    if (query) {
      gtk_editable_set_text(GTK_EDITABLE(self.search), query);
      self.query = query;
      self.page = "browse";
    } else if (page) {
      if (std::string(page) != self.page) {
        gtk_editable_set_text(GTK_EDITABLE(self.search), "");
        self.query.clear();
        self.categoryFilter.clear();
      }
      self.page = page;
    }
    adw_view_stack_set_visible_child_name(self.stack, self.page.c_str());
    gtk_widget_set_visible(self.search, true);
    self.syncSearchPlaceholder();
    self.creating = false;
    if (page || source || query) {
      self.invalidateListRequests();
      self.hits.clear();
      self.pageCache.clear();
      if (self.busy && !self.mutation) {
        g_timeout_add(200, +[](gpointer data) -> gboolean {
          auto* software = static_cast<Software*>(data);
          if (software->closing) return G_SOURCE_REMOVE;
          if (noctalia::packages::busy()) return G_SOURCE_CONTINUE;
          software->busy = false;
          software->controls();
          software->load();
          return G_SOURCE_REMOVE;
        }, &self);
        self.results();
      } else {
        self.load();
      }
    }
    if (appId) {
      self.openAppById(appId);
    }
    return 0;
  })), software.get());
  const int result = g_application_run(G_APPLICATION(app), argc, argv); software.reset(); g_object_unref(app); return result;
}
