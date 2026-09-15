#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/tab.h"
#include "system/package_kit_service.h"
#include "ui/controls/scroll_view.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Button;
class ConfigService;
class Flex;
class Input;
class Label;
class Segmented;
class Spinner;
struct ControlCenterServices;

class PackagesTab : public Tab {
public:
  explicit PackagesTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setActive(bool active) override;

private:
  enum class View : std::uint8_t { Search, Updates, Installed };

  struct ResultRow {
    Label* name = nullptr;
    Label* summary = nullptr;
    Label* version = nullptr;
    Button* action = nullptr;
    std::string packageName;
    std::string installation;
    std::string remotes;
  };

  struct ViewCache {
    std::vector<noctalia::packages::PackageHit> hits;
    bool loaded = false;
  };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void setStatus(std::string text);
  void setSpinnerVisible(bool visible);
  void applySearchField(bool force);
  void onSearchChanged();
  void searchFromInput();
  void loadUpdates();
  void loadInstalled();
  void finishQuery(std::uint64_t generation, noctalia::packages::CommandResult result, View view);
  void runAction(noctalia::packages::Action action, std::string name, std::string installation, std::string remotes);
  void launchAction(
      noctalia::packages::Action action, std::string name, std::string installation, std::string remotes
  );
  void runUpdateAll();
  void launchUpdateAll();
  void rebuildResults();
  void syncFilterControl();
  void syncUpdateAllBar();
  void syncBusy();
  void syncPlaceholder();
  void refreshStatus();
  void applyFilter(std::size_t index);
  void applyView(std::size_t index);
  void applyKind(std::size_t index);
  void persistKind();
  void showView(View view, bool forceReload);
  void restoreOrLoad(View view, bool forceReload);
  void clearPendingRemove();
  void invalidateCaches();
  [[nodiscard]] std::string currentQuery() const;
  [[nodiscard]] bool matchesName(const noctalia::packages::PackageHit& hit) const;
  [[nodiscard]] bool matchesFilter(const noctalia::packages::PackageHit& hit) const;
  [[nodiscard]] bool skipPolkit(noctalia::packages::Action action, std::string_view installation) const;
  [[nodiscard]] std::vector<const noctalia::packages::PackageHit*> visibleHits() const;
  [[nodiscard]] noctalia::packages::ProgressCallback progressSink(std::uint64_t generation);
  [[nodiscard]] noctalia::packages::Kind currentKind() const;
  [[nodiscard]] ViewCache& updatesCache();
  [[nodiscard]] ViewCache& installedCache();
  [[nodiscard]] static std::size_t indexFromView(View view);
  [[nodiscard]] static View viewFromIndex(std::size_t index);
  [[nodiscard]] static std::size_t indexFromKind(noctalia::packages::Kind kind);
  [[nodiscard]] static noctalia::packages::Kind kindFromIndex(std::size_t index);

  std::function<void(std::function<void(bool)>)> m_ensurePolkit;
  ConfigService* m_config = nullptr;
  Flex* m_root = nullptr;
  Input* m_search = nullptr;
  Spinner* m_spinner = nullptr;
  Button* m_updatesButton = nullptr;
  Button* m_cancelButton = nullptr;
  Button* m_updateAll = nullptr;
  Label* m_status = nullptr;
  Segmented* m_kindSwitch = nullptr;
  Segmented* m_viewSwitch = nullptr;
  Segmented* m_filter = nullptr;
  ScrollView* m_scroll = nullptr;
  Flex* m_results = nullptr;
  ScrollViewState m_scrollState;
  std::vector<ResultRow> m_rows;
  std::vector<noctalia::packages::PackageHit> m_hits;
  ViewCache m_updatesCache;
  ViewCache m_installedCache;
  ViewCache m_flatpakUpdatesCache;
  ViewCache m_flatpakInstalledCache;
  std::optional<std::string> m_issuedQuery;
  std::optional<std::string> m_pendingRemove;
  std::uint64_t m_generation = 0;
  std::size_t m_totalHits = 0;
  std::size_t m_filterIndex = 0;
  Timer m_searchTimer;
  Timer m_removeConfirmTimer;
  View m_view = View::Updates;
  noctalia::packages::Kind m_kind = noctalia::packages::Kind::Fedora;
  bool m_loadedOnce = false;
  bool m_rerunAfterBusy = false;
  bool m_viewSyncing = false;
  bool m_kindSyncing = false;
};
