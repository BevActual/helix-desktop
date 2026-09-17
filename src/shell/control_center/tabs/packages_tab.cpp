#include "shell/control_center/tabs/packages_tab.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/spinner.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <utility>

using namespace control_center;

namespace {

  constexpr std::size_t kMaxRows = 250;
  constexpr auto kSearchDebounce = std::chrono::milliseconds(280);
  constexpr auto kRemoveConfirmTimeout = std::chrono::seconds(4);

  std::string compactVersion(const noctalia::packages::PackageHit& hit) {
    std::string version = hit.version;
    if (const auto plus = version.find('+'); plus != std::string::npos) {
      version.resize(plus);
    }
    if (!hit.epoch.empty()) {
      return hit.epoch + ':' + version;
    }
    return version;
  }

  std::string detailLabel(const noctalia::packages::PackageHit& hit) {
    std::string text;
    if (hit.state == noctalia::packages::PackageState::Installed) {
      text = i18n::tr("control-center.packages.state-installed");
    } else if (hit.state == noctalia::packages::PackageState::Available) {
      text = i18n::tr("control-center.packages.state-available");
    } else if (!hit.info.empty()) {
      text = hit.info;
    }
    const std::string version = compactVersion(hit);
    if (!text.empty() && !version.empty()) {
      text += " · ";
    }
    text += version;
    if (!hit.repo.empty()) {
      text += " · ";
      text += hit.repo;
    }
    if (!hit.installation.empty()) {
      text += " · ";
      text += hit.installation;
    }
    return text;
  }

  std::string displayName(const noctalia::packages::PackageHit& hit) {
    return hit.title.empty() ? hit.name : hit.title;
  }

  std::string fullDetailLabel(const noctalia::packages::PackageHit& hit) {
    std::string version;
    if (!hit.epoch.empty()) {
      version += hit.epoch;
      version += ':';
    }
    version += hit.version;
    version += '-';
    version += hit.release;
    if (!hit.arch.empty()) {
      version += '.';
      version += hit.arch;
    }
    std::string text = hit.info;
    if (!text.empty() && !version.empty()) {
      text += " · ";
    }
    text += version;
    if (!hit.repo.empty()) {
      text += " · ";
      text += hit.repo;
    }
    if (!hit.installation.empty()) {
      text += " · ";
      text += hit.installation;
    }
    return text;
  }

} // namespace

PackagesTab::PackagesTab(const ControlCenterServices& services)
    : m_ensurePolkit(services.ensurePolkit), m_config(services.config) {
  rebuildKindOrder();
  if (m_config != nullptr) {
    if (const std::optional<std::string> source = m_config->stateString("packages_tab", "source")) {
      if (*source == "flatpak" && noctalia::packages::flatpakAvailable()) {
        m_kind = noctalia::packages::Kind::Flatpak;
      } else if (*source == "firmware" && noctalia::packages::firmwareAvailable()) {
        m_kind = noctalia::packages::Kind::Firmware;
      }
    }
  }
}

std::unique_ptr<Flex> PackagesTab::create() {
  const float scale = contentScale();
  const float controlHeight = Style::controlHeight * scale;

  auto tab = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto searchCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  rebuildKindOrder();
  std::vector<ui::SegmentedOption> kindOptions;
  kindOptions.reserve(m_kindOrder.size());
  for (const auto kind : m_kindOrder) {
    const char* tooltipKey = "control-center.packages.source-fedora-tooltip";
    const char* glyph = "package";
    if (kind == noctalia::packages::Kind::Flatpak) {
      tooltipKey = "control-center.packages.source-flatpak-tooltip";
      glyph = "box";
    } else if (kind == noctalia::packages::Kind::Firmware) {
      tooltipKey = "control-center.packages.source-firmware-tooltip";
      glyph = "cpu";
    }
    kindOptions.push_back({.label = "", .glyph = glyph, .tooltip = i18n::tr(tooltipKey)});
  }
  const bool showKindSwitch = m_kindOrder.size() > 1;
  searchCard->addChild(
      ui::segmented({
          .out = &m_kindSwitch,
          .options = std::move(kindOptions),
          .selectedIndex = indexFromKind(m_kind),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .surfaceRole = ColorRole::Surface,
          .visible = showKindSwitch,
          .participatesInLayout = showKindSwitch,
          .onChange = [this](std::size_t index) { applyKind(index); },
      })
  );
  searchCard->addChild(
      ui::segmented({
          .out = &m_viewSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = i18n::tr("control-center.packages.view-search"),
                      .tooltip = i18n::tr("control-center.packages.view-search-tooltip"),
                  },
                  {
                      .label = i18n::tr("control-center.packages.view-updates"),
                      .tooltip = i18n::tr("control-center.packages.view-updates-tooltip"),
                  },
                  {
                      .label = i18n::tr("control-center.packages.view-installed"),
                      .tooltip = i18n::tr("control-center.packages.view-installed-tooltip"),
                  },
              },
          .selectedIndex = indexFromView(m_view),
          .fontSize = Style::fontSizeCaption,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .surfaceRole = ColorRole::Surface,
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyView(index); },
      })
  );
  auto searchRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });
  searchRow->addChild(
      ui::glyph({
          .glyph = "search",
          .glyphSize = Style::baseGlyphSize * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  searchRow->addChild(
      ui::input({
          .out = &m_search,
          .placeholder = i18n::tr("control-center.packages.search-placeholder"),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = controlHeight,
          .horizontalPadding = Style::spaceMd * scale,
          .clearButtonEnabled = true,
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0F,
          .onChange = [this](const std::string&) { onSearchChanged(); },
          .onSubmit = [this](const std::string&) { applySearchField(true); },
      })
  );
  searchRow->addChild(
      ui::spinner({
          .out = &m_spinner,
          .color = colorSpecFromRole(ColorRole::Primary),
          .spinnerSize = Style::baseGlyphSize * scale,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  searchCard->addChild(std::move(searchRow));
  auto statusRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });
  statusRow->addChild(
      ui::label({
          .out = &m_status,
          .text = i18n::tr("control-center.packages.hint"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
          .flexGrow = 1.0F,
      })
  );
  statusRow->addChild(
      ui::button({
          .out = &m_updateAll,
          .text = i18n::tr("control-center.packages.update-all"),
          .fontSize = Style::fontSizeCaption * scale,
          .contentAlign = ButtonContentAlign::Center,
          .variant = ButtonVariant::Primary,
          .minHeight = Style::controlHeightSm * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
          .visible = false,
          .participatesInLayout = false,
          .onClick = [this]() { runUpdateAll(); },
      })
  );
  searchCard->addChild(std::move(statusRow));
  searchCard->addChild(
      ui::segmented({
          .out = &m_filter,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = i18n::tr("control-center.packages.filter-all"),
                      .tooltip = i18n::tr("control-center.packages.filter-all-tooltip"),
                  },
                  {
                      .label = i18n::tr("control-center.packages.filter-security"),
                      .tooltip = i18n::tr("control-center.packages.filter-security-tooltip"),
                  },
                  {
                      .label = i18n::tr("control-center.packages.filter-other"),
                      .tooltip = i18n::tr("control-center.packages.filter-other-tooltip"),
                  },
              },
          .selectedIndex = 0,
          .fontSize = Style::fontSizeCaption,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .surfaceRole = ColorRole::Surface,
          .equalSegmentWidths = true,
          .visible = false,
          .participatesInLayout = false,
          .onChange = [this](std::size_t index) { applyFilter(index); },
      })
  );
  tab->addChild(std::move(searchCard));

  auto scroll = ui::scrollView({
      .out = &m_scroll,
      .state = &m_scrollState,
      .contentScale = scale,
      .scrollbarVisible = true,
      .flexGrow = 1.0F,
      .configure =
          [](ScrollView& scrollView) {
            scrollView.clearFill();
            scrollView.clearBorder();
          },
  });
  m_results = scroll->content();
  if (m_results != nullptr) {
    m_results->setDirection(FlexDirection::Vertical);
    m_results->setAlign(FlexAlign::Stretch);
    m_results->setGap(Style::spaceSm * scale);
  }
  tab->addChild(std::move(scroll));
  return tab;
}

std::unique_ptr<Flex> PackagesTab::createHeaderActions() {
  const float scale = contentScale();
  return ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .out = &m_cancelButton,
          .glyph = "player-stop",
          .tooltip = i18n::tr("control-center.packages.cancel"),
          .visible = false,
          .participatesInLayout = false,
          .onClick = []() { noctalia::packages::cancelCurrent(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      }),
      ui::button({
          .out = &m_updatesButton,
          .glyph = "refresh",
          .tooltip = i18n::tr("control-center.packages.refresh"),
          .onClick = [this]() { showView(m_view, true); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
}

void PackagesTab::onClose() {
  m_searchTimer.stop();
  m_removeConfirmTimer.stop();
  m_root = nullptr;
  m_search = nullptr;
  m_spinner = nullptr;
  m_updatesButton = nullptr;
  m_cancelButton = nullptr;
  m_updateAll = nullptr;
  m_status = nullptr;
  m_kindSwitch = nullptr;
  m_viewSwitch = nullptr;
  m_filter = nullptr;
  m_scroll = nullptr;
  m_results = nullptr;
  m_rows.clear();
  m_hits.clear();
  m_updatesCache = {};
  m_installedCache = {};
  m_flatpakUpdatesCache = {};
  m_flatpakInstalledCache = {};
  m_firmwareUpdatesCache = {};
  m_firmwareInstalledCache = {};
  m_totalHits = 0;
  m_filterIndex = 0;
  m_loadedOnce = false;
  m_rerunAfterBusy = false;
  m_viewSyncing = false;
  m_kindSyncing = false;
  m_view = View::Updates;
  m_pendingRemove.reset();
  m_issuedQuery.reset();
}

void PackagesTab::setActive(bool active) {
  if (!active || m_loadedOnce) {
    return;
  }
  m_loadedOnce = true;
  showView(m_view, false);
}

void PackagesTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);

  const float scale = contentScale();
  const float cardPad = Style::spaceMd * scale * 2.0F;
  const float gap = Style::spaceSm * scale;
  if (m_status != nullptr) {
    float statusMax = std::max(1.0F, contentWidth - cardPad);
    if (m_updateAll != nullptr && m_updateAll->visible()) {
      statusMax = std::max(1.0F, statusMax - m_updateAll->width() - gap);
    }
    m_status->setMaxWidth(statusMax);
  }
  for (const auto& row : m_rows) {
    if (row.name == nullptr) {
      continue;
    }
    const float actionWidth = row.action != nullptr ? row.action->width() : 0.0F;
    const float nameMax = std::max(1.0F, contentWidth - actionWidth - gap - cardPad);
    row.name->setMaxWidth(nameMax);
    if (row.summary != nullptr) {
      row.summary->setMaxWidth(std::max(1.0F, contentWidth - cardPad));
    }
    if (row.version != nullptr) {
      row.version->setMaxWidth(std::max(1.0F, contentWidth - cardPad));
    }
  }
  m_root->layout(renderer);
}

void PackagesTab::setStatus(std::string text) {
  if (m_status != nullptr) {
    m_status->setText(std::move(text));
  }
}

void PackagesTab::setSpinnerVisible(bool visible) {
  if (m_spinner == nullptr) {
    return;
  }
  m_spinner->setVisible(visible);
  m_spinner->setParticipatesInLayout(visible);
  if (visible && !m_spinner->spinning()) {
    m_spinner->start();
  } else if (!visible && m_spinner->spinning()) {
    m_spinner->stop();
  }
}

void PackagesTab::onSearchChanged() {
  m_searchTimer.start(kSearchDebounce, [this]() { applySearchField(false); });
}

std::string PackagesTab::currentQuery() const {
  if (m_search == nullptr) {
    return {};
  }
  return StringUtils::trim(m_search->value());
}

void PackagesTab::syncPlaceholder() {
  if (m_search == nullptr) {
    return;
  }
  const char* key = "control-center.packages.search-placeholder";
  if (m_view == View::Updates) {
    key = "control-center.packages.filter-updates-placeholder";
  } else if (m_view == View::Installed) {
    key = "control-center.packages.filter-installed-placeholder";
  }
  m_search->setPlaceholder(i18n::tr(key));
}

std::size_t PackagesTab::indexFromView(View view) {
  switch (view) {
  case View::Search:
    return 0;
  case View::Updates:
    return 1;
  case View::Installed:
    return 2;
  }
  return 1;
}

PackagesTab::View PackagesTab::viewFromIndex(std::size_t index) {
  if (index == 0) {
    return View::Search;
  }
  if (index == 2) {
    return View::Installed;
  }
  return View::Updates;
}

std::size_t PackagesTab::indexFromKind(noctalia::packages::Kind kind) const {
  for (std::size_t i = 0; i < m_kindOrder.size(); ++i) {
    if (m_kindOrder[i] == kind) {
      return i;
    }
  }
  return 0;
}

noctalia::packages::Kind PackagesTab::kindFromIndex(std::size_t index) const {
  if (index < m_kindOrder.size()) {
    return m_kindOrder[index];
  }
  return noctalia::packages::Kind::Fedora;
}

noctalia::packages::Kind PackagesTab::currentKind() const { return m_kind; }

void PackagesTab::rebuildKindOrder() {
  m_kindOrder.clear();
  m_kindOrder.push_back(noctalia::packages::Kind::Fedora);
  if (noctalia::packages::flatpakAvailable()) {
    m_kindOrder.push_back(noctalia::packages::Kind::Flatpak);
  }
  if (noctalia::packages::firmwareAvailable()) {
    m_kindOrder.push_back(noctalia::packages::Kind::Firmware);
  }
  if (std::ranges::find(m_kindOrder, m_kind) == m_kindOrder.end()) {
    m_kind = noctalia::packages::Kind::Fedora;
  }
}

const char* PackagesTab::unavailableKey() const {
  if (m_kind == noctalia::packages::Kind::Flatpak) {
    return "control-center.packages.unavailable-flatpak";
  }
  if (m_kind == noctalia::packages::Kind::Firmware) {
    return "control-center.packages.unavailable-firmware";
  }
  return "control-center.packages.unavailable";
}

PackagesTab::ViewCache& PackagesTab::updatesCache() {
  if (m_kind == noctalia::packages::Kind::Flatpak) {
    return m_flatpakUpdatesCache;
  }
  if (m_kind == noctalia::packages::Kind::Firmware) {
    return m_firmwareUpdatesCache;
  }
  return m_updatesCache;
}

PackagesTab::ViewCache& PackagesTab::installedCache() {
  if (m_kind == noctalia::packages::Kind::Flatpak) {
    return m_flatpakInstalledCache;
  }
  if (m_kind == noctalia::packages::Kind::Firmware) {
    return m_firmwareInstalledCache;
  }
  return m_installedCache;
}

void PackagesTab::persistKind() {
  if (m_config == nullptr) {
    return;
  }
  const char* source = "fedora";
  if (m_kind == noctalia::packages::Kind::Flatpak) {
    source = "flatpak";
  } else if (m_kind == noctalia::packages::Kind::Firmware) {
    source = "firmware";
  }
  (void)m_config->setStateString("packages_tab", "source", source);
}

void PackagesTab::applyKind(std::size_t index) {
  if (m_kindSyncing) {
    return;
  }
  const auto next = kindFromIndex(index);
  if (next == m_kind) {
    return;
  }
  m_kind = next;
  persistKind();
  m_issuedQuery.reset();
  m_filterIndex = 0;
  if (m_filter != nullptr) {
    m_filter->setSelectedIndex(0);
  }
  clearPendingRemove();
  if (m_kind == noctalia::packages::Kind::Firmware && m_view == View::Search) {
    showView(View::Updates, false);
    return;
  }
  if (m_view == View::Search) {
    m_hits.clear();
    m_totalHits = 0;
  }
  restoreOrLoad(m_view, false);
}

void PackagesTab::applyView(std::size_t index) {
  if (m_viewSyncing) {
    return;
  }
  View view = viewFromIndex(index);
  if (m_kind == noctalia::packages::Kind::Firmware && view == View::Search) {
    view = View::Updates;
  }
  showView(view, false);
}

void PackagesTab::showView(View view, bool forceReload) {
  const bool viewChanged = view != m_view;
  m_view = view;
  if (m_viewSwitch != nullptr) {
    m_viewSyncing = true;
    m_viewSwitch->setSelectedIndex(indexFromView(m_view));
    m_viewSyncing = false;
  }
  if (m_kindSwitch != nullptr) {
    m_kindSyncing = true;
    m_kindSwitch->setSelectedIndex(indexFromKind(m_kind));
    m_kindSyncing = false;
  }
  if (viewChanged) {
    m_filterIndex = 0;
    if (m_filter != nullptr) {
      m_filter->setSelectedIndex(0);
    }
    m_issuedQuery.reset();
    clearPendingRemove();
    if (m_view == View::Search) {
      m_hits.clear();
      m_totalHits = 0;
    }
  }
  if (forceReload) {
    if (m_view == View::Installed) {
      installedCache() = {};
    } else if (m_view == View::Updates) {
      updatesCache() = {};
    }
  }
  syncPlaceholder();
  restoreOrLoad(m_view, forceReload);
}

void PackagesTab::restoreOrLoad(View view, bool forceReload) {
  if (view == View::Search) {
    applySearchField(forceReload);
    return;
  }
  ViewCache& cache = view == View::Installed ? installedCache() : updatesCache();
  if (cache.loaded) {
    m_hits = cache.hits;
    m_totalHits = cache.hits.size();
    refreshStatus();
    rebuildResults();
    syncBusy();
    PanelManager::instance().refresh();
    return;
  }
  if (view == View::Installed) {
    loadInstalled();
  } else {
    loadUpdates();
  }
}

void PackagesTab::invalidateCaches() {
  m_updatesCache = {};
  m_installedCache = {};
  m_flatpakUpdatesCache = {};
  m_flatpakInstalledCache = {};
  m_firmwareUpdatesCache = {};
  m_firmwareInstalledCache = {};
}

void PackagesTab::applySearchField(bool force) {
  m_searchTimer.stop();
  if (m_search == nullptr) {
    return;
  }
  const std::string query = currentQuery();
  if (m_view == View::Search) {
    if (!query.empty() && (!noctalia::packages::hasSafeSearchChars(query) || query.size() > 80)) {
      setStatus(i18n::tr("control-center.packages.search-invalid"));
      PanelManager::instance().refresh();
      return;
    }
    if (query.empty()) {
      m_issuedQuery = std::string{};
      m_hits.clear();
      m_totalHits = 0;
      refreshStatus();
      rebuildResults();
      syncBusy();
      PanelManager::instance().refresh();
      return;
    }
    if (query.size() < 2) {
      if (force) {
        setStatus(i18n::tr("control-center.packages.search-invalid"));
        PanelManager::instance().refresh();
      }
      return;
    }
    if (!force && m_issuedQuery.has_value() && *m_issuedQuery == query) {
      return;
    }
    if (noctalia::packages::busy()) {
      m_rerunAfterBusy = true;
      return;
    }
    searchFromInput();
    return;
  }
  refreshStatus();
  rebuildResults();
  syncBusy();
  PanelManager::instance().refresh();
}

void PackagesTab::searchFromInput() {
  if (m_search == nullptr) {
    return;
  }
  const std::string query = currentQuery();
  if (!noctalia::packages::isSafeSearchQuery(query)) {
    setStatus(i18n::tr("control-center.packages.search-invalid"));
    return;
  }
  if (noctalia::packages::busy()) {
    m_rerunAfterBusy = true;
    setStatus(i18n::tr("control-center.packages.busy"));
    return;
  }
  const bool kindChanged = !m_issuedQuery.has_value() || *m_issuedQuery != query;
  m_issuedQuery = query;
  if (kindChanged) {
    m_hits.clear();
    m_totalHits = 0;
    rebuildResults();
    if (m_scroll != nullptr) {
      m_scroll->requestScrollToOffset(0.0F);
    }
  }
  const auto generation = ++m_generation;
  setStatus(i18n::tr("control-center.packages.searching"));
  syncBusy();
  if (!noctalia::packages::search(
          query,
          [this, generation](noctalia::packages::CommandResult result) {
            DeferredCall::callLater([this, generation, result = std::move(result)]() mutable {
              finishQuery(generation, std::move(result), View::Search);
            });
          },
          m_kind
      )) {
    --m_generation;
    setStatus(i18n::tr(
        unavailableKey()
    ));
    syncBusy();
  }
}

void PackagesTab::loadUpdates() {
  if (noctalia::packages::busy()) {
    m_rerunAfterBusy = true;
    setStatus(i18n::tr("control-center.packages.busy"));
    return;
  }
  m_issuedQuery.reset();
  m_hits.clear();
  m_totalHits = 0;
  rebuildResults();
  if (m_scroll != nullptr) {
    m_scroll->requestScrollToOffset(0.0F);
  }
  const auto generation = ++m_generation;
  setStatus(i18n::tr("control-center.packages.loading-updates"));
  syncBusy();
  if (!noctalia::packages::listUpdates(
          [this, generation](noctalia::packages::CommandResult result) {
            DeferredCall::callLater([this, generation, result = std::move(result)]() mutable {
              finishQuery(generation, std::move(result), View::Updates);
            });
          },
          m_kind
      )) {
    --m_generation;
    setStatus(i18n::tr(
        unavailableKey()
    ));
    syncBusy();
  }
}

void PackagesTab::loadInstalled() {
  if (noctalia::packages::busy()) {
    m_rerunAfterBusy = true;
    setStatus(i18n::tr("control-center.packages.busy"));
    return;
  }
  m_issuedQuery.reset();
  m_hits.clear();
  m_totalHits = 0;
  rebuildResults();
  if (m_scroll != nullptr) {
    m_scroll->requestScrollToOffset(0.0F);
  }
  const auto generation = ++m_generation;
  setStatus(i18n::tr("control-center.packages.loading-installed"));
  syncBusy();
  if (!noctalia::packages::listInstalled(
          [this, generation](noctalia::packages::CommandResult result) {
            DeferredCall::callLater([this, generation, result = std::move(result)]() mutable {
              finishQuery(generation, std::move(result), View::Installed);
            });
          },
          m_kind
      )) {
    --m_generation;
    setStatus(i18n::tr(
        unavailableKey()
    ));
    syncBusy();
  }
}

void PackagesTab::finishQuery(std::uint64_t generation, noctalia::packages::CommandResult result, View view) {
  if (generation != m_generation) {
    return;
  }
  if (result.cancelled) {
    setStatus(i18n::tr("control-center.packages.cancelled-task"));
    syncBusy();
    PanelManager::instance().refresh();
    return;
  }
  if (!result.success) {
    setStatus(
        result.error.empty() ? i18n::tr("control-center.packages.failed")
                             : i18n::tr("control-center.packages.failed-detail", "error", result.error)
    );
    syncBusy();
    PanelManager::instance().refresh();
    if (m_rerunAfterBusy) {
      m_rerunAfterBusy = false;
      showView(m_view, true);
    }
    return;
  }
  auto hits = noctalia::packages::dedupeByName(std::move(result.hits));
  noctalia::packages::sortHits(hits);
  if (view == View::Updates) {
    updatesCache().hits = hits;
    updatesCache().loaded = true;
    noctalia::packages::notePendingUpdates(m_kind, hits.size());
  } else if (view == View::Installed) {
    installedCache().hits = hits;
    installedCache().loaded = true;
  }
  if (view != m_view) {
    syncBusy();
    PanelManager::instance().refresh();
    return;
  }
  m_hits = std::move(hits);
  m_totalHits = m_hits.size();
  refreshStatus();
  rebuildResults();
  syncBusy();
  PanelManager::instance().refresh();
  if (m_rerunAfterBusy) {
    m_rerunAfterBusy = false;
    applySearchField(false);
  }
}

void PackagesTab::runAction(
    noctalia::packages::Action action, std::string name, std::string installation, std::string remotes
) {
  if (action == noctalia::packages::Action::Remove && noctalia::packages::isProtectedPackage(name)) {
    notify::error(
        "Noctalia", i18n::tr("control-center.packages.title"), i18n::tr("control-center.packages.protected")
    );
    return;
  }
  if (action == noctalia::packages::Action::Remove) {
    if (!m_pendingRemove.has_value() || *m_pendingRemove != name) {
      clearPendingRemove();
      m_pendingRemove = name;
      for (const auto& row : m_rows) {
        if (row.packageName == name && row.action != nullptr) {
          row.action->setText(i18n::tr("control-center.packages.remove-confirm"));
        }
      }
      m_removeConfirmTimer.start(kRemoveConfirmTimeout, [this]() {
        clearPendingRemove();
        PanelManager::instance().refresh();
      });
      PanelManager::instance().refresh();
      return;
    }
    m_removeConfirmTimer.stop();
    m_pendingRemove.reset();
  }
  if (m_ensurePolkit && !skipPolkit(action, installation)) {
    m_ensurePolkit([this, action, name = std::move(name), installation = std::move(installation),
                    remotes = std::move(remotes)](bool ok) {
      if (!ok) {
        setStatus(i18n::tr("control-center.packages.cancelled"));
        PanelManager::instance().refresh();
        return;
      }
      launchAction(action, name, installation, remotes);
    });
    return;
  }
  launchAction(action, std::move(name), std::move(installation), std::move(remotes));
}

void PackagesTab::launchAction(
    noctalia::packages::Action action, std::string name, std::string installation, std::string remotes
) {
  if (noctalia::packages::busy()) {
    setStatus(i18n::tr("control-center.packages.busy"));
    return;
  }
  const auto generation = ++m_generation;
  setStatus(i18n::tr("control-center.packages.working", "name", name));
  syncBusy();
  PanelManager::instance().refresh();
  if (!noctalia::packages::mutate(
          action, name,
          [this, generation, name](noctalia::packages::CommandResult result) {
            DeferredCall::callLater([this, generation, name, result = std::move(result)]() mutable {
              if (generation != m_generation) {
                return;
              }
              if (result.cancelled) {
                setStatus(i18n::tr("control-center.packages.cancelled-task"));
                syncBusy();
                PanelManager::instance().refresh();
                return;
              }
              if (result.success) {
                notify::info(
                    "Noctalia", i18n::tr("control-center.packages.title"),
                    i18n::tr("control-center.packages.action-ok", "name", name)
                );
                m_issuedQuery.reset();
                invalidateCaches();
                showView(m_view, true);
                return;
              }
              notify::error(
                  "Noctalia", i18n::tr("control-center.packages.title"),
                  result.error.empty() ? i18n::tr("control-center.packages.failed")
                                       : i18n::tr("control-center.packages.failed-detail", "error", result.error)
              );
              syncBusy();
              PanelManager::instance().refresh();
            });
          },
          progressSink(generation), m_kind, std::move(installation), std::move(remotes)
      )) {
    --m_generation;
    setStatus(
        i18n::tr(unavailableKey())
    );
    syncBusy();
    PanelManager::instance().refresh();
  }
}

void PackagesTab::runUpdateAll() {
  if (m_ensurePolkit) {
    m_ensurePolkit([this](bool ok) {
      if (!ok) {
        setStatus(i18n::tr("control-center.packages.cancelled"));
        PanelManager::instance().refresh();
        return;
      }
      launchUpdateAll();
    });
    return;
  }
  launchUpdateAll();
}

void PackagesTab::launchUpdateAll() {
  if (noctalia::packages::busy()) {
    setStatus(i18n::tr("control-center.packages.busy"));
    return;
  }
  const auto generation = ++m_generation;
  setStatus(i18n::tr("control-center.packages.working", "name", i18n::tr("control-center.packages.update-all")));
  syncBusy();
  PanelManager::instance().refresh();
  auto onComplete = [this, generation](noctalia::packages::CommandResult result) {
    DeferredCall::callLater([this, generation, result = std::move(result)]() mutable {
      if (generation != m_generation) {
        return;
      }
      if (result.cancelled) {
        setStatus(i18n::tr("control-center.packages.cancelled-task"));
        syncBusy();
        PanelManager::instance().refresh();
        return;
      }
      if (result.success) {
        notify::info(
            "Noctalia", i18n::tr("control-center.packages.title"), i18n::tr("control-center.packages.update-all-ok")
        );
        m_issuedQuery.reset();
        invalidateCaches();
        showView(View::Updates, true);
        return;
      }
      notify::error(
          "Noctalia", i18n::tr("control-center.packages.title"),
          result.error.empty() ? i18n::tr("control-center.packages.failed")
                               : i18n::tr("control-center.packages.failed-detail", "error", result.error)
      );
      syncBusy();
      PanelManager::instance().refresh();
    });
  };
  bool launched = false;
  if (m_filterIndex == 0) {
    launched = noctalia::packages::updateAll(onComplete, progressSink(generation), m_kind);
  } else {
    std::vector<std::string> names;
    for (const auto* hit : visibleHits()) {
      if (hit != nullptr) {
        names.push_back(hit->name);
      }
    }
    launched = noctalia::packages::updatePackages(
        std::move(names), onComplete, progressSink(generation), m_kind
    );
  }
  if (!launched) {
    --m_generation;
    setStatus(
        i18n::tr(unavailableKey())
    );
    syncBusy();
    PanelManager::instance().refresh();
  }
}

bool PackagesTab::matchesName(const noctalia::packages::PackageHit& hit) const {
  const std::string query = currentQuery();
  if (query.empty() || m_view == View::Search) {
    return true;
  }
  return StringUtils::containsInsensitive(hit.name, query)
      || StringUtils::containsInsensitive(hit.title, query)
      || StringUtils::containsInsensitive(hit.summary, query);
}

bool PackagesTab::skipPolkit(noctalia::packages::Action action, std::string_view installation) const {
  if (m_kind != noctalia::packages::Kind::Flatpak) {
    return false;
  }
  if (action == noctalia::packages::Action::Install) {
    return true;
  }
  return installation != "system";
}

bool PackagesTab::matchesFilter(const noctalia::packages::PackageHit& hit) const {
  if (m_view != View::Updates || m_filterIndex == 0) {
    return true;
  }
  if (m_filterIndex == 1) {
    return hit.info == "Security";
  }
  return hit.info != "Security";
}

std::vector<const noctalia::packages::PackageHit*> PackagesTab::visibleHits() const {
  std::vector<const noctalia::packages::PackageHit*> hits;
  hits.reserve(std::min(m_hits.size(), kMaxRows));
  for (const auto& hit : m_hits) {
    if (!matchesFilter(hit) || !matchesName(hit)) {
      continue;
    }
    hits.push_back(&hit);
    if (hits.size() >= kMaxRows) {
      break;
    }
  }
  return hits;
}

void PackagesTab::applyFilter(std::size_t index) {
  if (index == m_filterIndex) {
    return;
  }
  m_filterIndex = index;
  clearPendingRemove();
  refreshStatus();
  rebuildResults();
  if (m_scroll != nullptr) {
    m_scroll->requestScrollToOffset(0.0F);
  }
  PanelManager::instance().refresh();
}

void PackagesTab::refreshStatus() {
  std::size_t matched = 0;
  for (const auto& hit : m_hits) {
    if (matchesFilter(hit) && matchesName(hit)) {
      ++matched;
    }
  }
  if (m_hits.empty()) {
    if (m_view == View::Updates) {
      setStatus(i18n::tr("control-center.packages.no-updates"));
    } else if (m_view == View::Installed) {
      setStatus(i18n::tr("control-center.packages.no-installed"));
    } else {
      setStatus(i18n::tr(
          currentQuery().empty() ? "control-center.packages.hint-search" : "control-center.packages.empty"
      ));
    }
    return;
  }
  if (matched == 0) {
    setStatus(i18n::tr("control-center.packages.filter-empty"));
    return;
  }
  const std::size_t shown = std::min(matched, kMaxRows);
  if (shown != matched) {
    setStatus(i18n::tr(
        "control-center.packages.showing-limited", "shown", std::to_string(shown), "total", std::to_string(matched)
    ));
    return;
  }
  const char* key = "control-center.packages.results";
  if (m_view == View::Updates) {
    key = "control-center.packages.results-updates";
  } else if (m_view == View::Installed) {
    key = "control-center.packages.results-installed";
  }
  setStatus(i18n::tr(key, "count", std::to_string(shown)));
}

void PackagesTab::syncFilterControl() {
  if (m_filter == nullptr) {
    return;
  }
  const bool show = m_view == View::Updates && m_kind == noctalia::packages::Kind::Fedora && !m_hits.empty();
  m_filter->setVisible(show);
  m_filter->setParticipatesInLayout(show);
}

noctalia::packages::ProgressCallback PackagesTab::progressSink(std::uint64_t generation) {
  return [this, generation](noctalia::packages::PkconProgress progress) {
    DeferredCall::callLater([this, generation, progress = std::move(progress)]() {
      if (generation != m_generation || !noctalia::packages::busy()) {
        return;
      }
      if (progress.status.empty()) {
        return;
      }
      if (progress.percent >= 0) {
        setStatus(i18n::tr(
            "control-center.packages.progress", "status", progress.status, "percent", std::to_string(progress.percent)
        ));
      } else {
        setStatus(progress.status);
      }
      PanelManager::instance().refresh();
    });
  };
}

void PackagesTab::clearPendingRemove() {
  m_removeConfirmTimer.stop();
  if (!m_pendingRemove.has_value()) {
    return;
  }
  const std::string name = *m_pendingRemove;
  m_pendingRemove.reset();
  for (const auto& row : m_rows) {
    if (row.packageName == name && row.action != nullptr) {
      row.action->setText(i18n::tr("control-center.packages.remove"));
    }
  }
}

void PackagesTab::rebuildResults() {
  if (m_results == nullptr) {
    return;
  }
  while (!m_results->children().empty()) {
    m_results->removeChild(m_results->children().front().get());
  }
  m_rows.clear();
  clearPendingRemove();

  const float scale = contentScale();
  const float actionHeight = Style::controlHeightSm * scale;
  const bool idle = !noctalia::packages::busy();
  const auto hits = visibleHits();
  auto card = ui::column({
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });

  if (hits.empty()) {
    if (!noctalia::packages::busy() && !m_hits.empty()) {
      card->addChild(
          ui::label({
              .text = i18n::tr("control-center.packages.filter-empty"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 3,
          })
      );
      m_results->addChild(std::move(card));
    }
  } else {
    for (std::size_t index = 0; index < hits.size(); ++index) {
      const auto* hitPtr = hits[index];
      if (hitPtr == nullptr) {
        continue;
      }
      const auto& hit = *hitPtr;
      if (index > 0) {
        card->addChild(
            ui::separator({
                .thickness = std::max(1.0F, scale),
                .spacing = Style::spaceXs * scale,
            })
        );
      }
      ResultRow row;
      row.packageName = hit.name;
      row.installation = hit.installation;
      row.remotes = hit.repo;
      auto item = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .paddingV = Style::spaceXs * scale,
      });
      auto header = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
          .minHeight = actionHeight,
      });
      header->addChild(
          ui::label({
              .out = &row.name,
              .text = displayName(hit),
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
              .flexGrow = 1.0F,
              .configure =
                  [tip = hit.name, titled = !hit.title.empty()](Label& label) {
                    if (titled) {
                      label.setTooltip(tip);
                    }
                  },
          })
      );

      const auto addAction = [&](noctalia::packages::Action action, const char* key, ButtonVariant variant) {
        const std::string name = hit.name;
        const std::string installation = hit.installation;
        const std::string remotes = hit.repo;
        header->addChild(
            ui::button({
                .out = &row.action,
                .text = i18n::tr(key),
                .fontSize = Style::fontSizeCaption * scale,
                .enabled = idle,
                .contentAlign = ButtonContentAlign::Center,
                .variant = variant,
                .minHeight = actionHeight,
                .paddingV = Style::spaceXs * scale,
                .paddingH = Style::spaceSm * scale,
                .radius = Style::scaledRadiusMd(scale),
                .onClick = [this, action, name, installation, remotes]() {
                  runAction(action, name, installation, remotes);
                },
            })
        );
      };

      if (hit.state == noctalia::packages::PackageState::Installed) {
        if (m_kind != noctalia::packages::Kind::Firmware
            && !noctalia::packages::isProtectedPackage(hit.name)) {
          addAction(noctalia::packages::Action::Remove, "control-center.packages.remove", ButtonVariant::Destructive);
        }
      } else if (hit.state == noctalia::packages::PackageState::Update) {
        addAction(noctalia::packages::Action::Update, "control-center.packages.update", ButtonVariant::Default);
      } else {
        addAction(noctalia::packages::Action::Install, "control-center.packages.install", ButtonVariant::Default);
      }
      item->addChild(std::move(header));
      if (!hit.summary.empty()) {
        item->addChild(
            ui::label({
                .out = &row.summary,
                .text = hit.summary,
                .fontSize = Style::fontSizeCaption * scale,
                .color = colorSpecFromRole(ColorRole::OnSurface),
                .maxLines = 1,
                .ellipsize = TextEllipsize::End,
                .configure = [tip = hit.summary](Label& label) { label.setTooltip(tip); },
            })
        );
      }
      item->addChild(
          ui::label({
              .out = &row.version,
              .text = detailLabel(hit),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
              .configure = [tip = fullDetailLabel(hit)](Label& label) { label.setTooltip(tip); },
          })
      );
      card->addChild(std::move(item));
      m_rows.push_back(row);
    }
    m_results->addChild(std::move(card));
  }

  syncFilterControl();
  syncUpdateAllBar();
}

void PackagesTab::syncUpdateAllBar() {
  if (m_updateAll == nullptr) {
    return;
  }
  const auto visible = visibleHits();
  const bool show = m_view == View::Updates && !visible.empty();
  m_updateAll->setVisible(show);
  m_updateAll->setParticipatesInLayout(show);
  if (!show) {
    return;
  }
  if (m_filterIndex == 0) {
    m_updateAll->setText(i18n::tr("control-center.packages.update-all"));
  } else {
    m_updateAll->setText(
        i18n::tr("control-center.packages.update-count", "count", std::to_string(visible.size()))
    );
  }
}

void PackagesTab::syncBusy() {
  const bool idle = !noctalia::packages::busy();
  if (m_updatesButton != nullptr) {
    m_updatesButton->setEnabled(idle);
  }
  if (m_cancelButton != nullptr) {
    m_cancelButton->setVisible(!idle);
    m_cancelButton->setParticipatesInLayout(!idle);
  }
  if (m_updateAll != nullptr) {
    m_updateAll->setEnabled(idle);
  }
  if (m_viewSwitch != nullptr) {
    m_viewSwitch->setEnabled(idle);
  }
  if (m_kindSwitch != nullptr) {
    m_kindSwitch->setEnabled(idle);
  }
  if (m_filter != nullptr) {
    m_filter->setEnabled(idle);
  }
  for (const auto& row : m_rows) {
    if (row.action != nullptr) {
      row.action->setEnabled(idle);
    }
  }
  setSpinnerVisible(!idle);
}
