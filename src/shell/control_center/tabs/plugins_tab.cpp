#include "shell/control_center/tabs/plugins_tab.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "net/url_open.h"
#include "render/core/renderer.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "scripting/plugin_catalog.h"
#include "scripting/plugin_manager.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  void setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return;
    }
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
  }

  void clearChildren(Flex* host) {
    if (host == nullptr) {
      return;
    }
    while (!host->children().empty()) {
      host->removeChild(host->children().back().get());
    }
  }

} // namespace

PluginsTab::View PluginsTab::viewFromIndex(std::size_t index) noexcept {
  return index == 1 ? View::Store : View::Installed;
}

std::size_t PluginsTab::indexFromView(View view) noexcept {
  return view == View::Store ? 1 : 0;
}

PluginsTab::PluginsTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_pluginManager(services.pluginManager),
      m_asyncTextures(services.asyncTextures),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "plugins", .includedGroups = {"sources"}}
      )),
      m_installedSettings(std::make_unique<SettingsPane>(
          services,
          SettingsPaneOptions{
              .section = "plugins",
              .includedGroups = {"plugins"},
              .openStore =
                  [this]() { applyView(indexFromView(View::Store)); },
          }
      )) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> view = m_config->stateString("plugins_tab", "view")) {
      if (*view == "store") {
        m_view = View::Store;
      }
    }
  }
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() {
      m_liveDirty = true;
      if (m_installedSettings != nullptr) {
        m_installedSettings->onExternalChanged();
      }
      invalidateStore(false);
    });
  }
}

std::unique_ptr<Flex> PluginsTab::create() {
  const float scale = contentScale();

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
  });

  auto toolbar = ui::column({
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) {
            applySectionCardStyle(section, scale, opacity);
            section.setGap(Style::spaceXs * scale);
            section.setPadding(Style::spaceSm * scale);
          },
  });
  toolbar->addChild(
      ui::segmented({
          .out = &m_viewSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = "",
                      .glyph = "puzzle",
                      .tooltip = i18n::tr("control-center.plugins.installed-tooltip"),
                  },
                  {
                      .label = "",
                      .glyph = "shopping-bag",
                      .tooltip = i18n::tr("control-center.plugins.store-tooltip"),
                  },
              },
          .selectedIndex = indexFromView(m_view),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .onChange = [this](std::size_t index) { applyView(index); },
      })
  );
  live->addChild(std::move(toolbar));

  live->addChild(
      ui::label({
          .out = &m_status,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
          .visible = false,
          .participatesInLayout = false,
      })
  );

  live->addChild(
      ui::column({
          .out = &m_installedHost,
          .align = FlexAlign::Stretch,
          .gap = 0.0F,
          .flexGrow = 1.0F,
      })
  );
  live->addChild(
      ui::column({
          .out = &m_storeHost,
          .align = FlexAlign::Stretch,
          .gap = 0.0F,
          .flexGrow = 1.0F,
      })
  );

  rebuildInstalledPane();
  syncLiveControls();
  if (m_view == View::Store) {
    requestStoreCatalog();
  }

  auto wrapped = m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
}

std::unique_ptr<Flex> PluginsTab::createHeaderActions() {
  const float scale = contentScale();
  auto live = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .out = &m_backButton,
          .glyph = Style::rtl() ? "chevron-right" : "chevron-left",
          .tooltip = i18n::tr("settings.plugins.store.back-to-catalog"),
          .visible = false,
          .participatesInLayout = false,
          .onClick =
              [this]() {
                if (m_store != nullptr) {
                  m_store->closeDetail();
                }
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      }),
      ui::button({
          .out = &m_pageButton,
          .glyph = "external-link",
          .tooltip = i18n::tr("settings.plugins.store.open-page"),
          .visible = false,
          .participatesInLayout = false,
          .onClick =
              [this]() {
                if (m_store == nullptr) {
                  return;
                }
                if (const auto url = m_store->detailPageUrl()) {
                  (void)net::openInBrowser(*url);
                }
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      }),
      ui::button({
          .out = &m_sourceButton,
          .glyph = "brand-git",
          .tooltip = i18n::tr("settings.plugins.store.open-source"),
          .visible = false,
          .participatesInLayout = false,
          .onClick =
              [this]() {
                if (m_store == nullptr) {
                  return;
                }
                if (const auto url = m_store->detailSourceUrl()) {
                  (void)net::openInBrowser(*url);
                }
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      }),
      ui::button({
          .out = &m_refreshButton,
          .glyph = "refresh",
          .tooltip = i18n::tr("control-center.plugins.refresh-tooltip"),
          .onClick = [this]() { refreshCurrentView(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  auto header = m_settings->embedHeader(std::move(live), scale);
  syncLiveControls();
  return header;
}

void PluginsTab::onClose() {
  ++m_storeGeneration;
  if (m_store != nullptr) {
    m_store->detachGrid();
  }
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  if (m_installedSettings != nullptr) {
    m_installedSettings->onClose();
  }
  m_store.reset();
  m_rootLayout = nullptr;
  m_viewSwitch = nullptr;
  m_status = nullptr;
  m_installedHost = nullptr;
  m_storeHost = nullptr;
  m_refreshButton = nullptr;
  m_backButton = nullptr;
  m_pageButton = nullptr;
  m_sourceButton = nullptr;
  m_storePopulated = false;
}

void PluginsTab::setConfigureMode(bool show) {
  const bool was = m_settings->configureMode();
  m_settings->setConfigureMode(show);
  if (was && !show) {
    m_liveDirty = true;
    invalidateStore(true);
    if (m_view == View::Store) {
      requestStoreCatalog();
    }
  }
}

bool PluginsTab::configureMode() const { return m_settings->configureMode(); }

void PluginsTab::setSettingsSearchQuery(std::string_view query) {
  if (!query.empty()) {
    m_settings->setConfigureMode(true);
  }
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* PluginsTab::settingsPane() { return m_settings.get(); }

bool PluginsTab::dismissTransientUi() {
  if (m_store != nullptr && m_store->isDetailView()) {
    m_store->closeDetail();
    return true;
  }
  return false;
}

bool PluginsTab::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (configureMode() || m_view != View::Store || m_store == nullptr) {
    return false;
  }
  return m_store->handleKeyEvent(
      sym, modifiers, pressed, preedit, PanelManager::instance().inputDispatcher().focusedArea()
  );
}

void PluginsTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->configureMode()) {
    (void)m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight);
    return;
  }
  if (m_view == View::Store) {
    populateStoreIfNeeded(renderer);
  }
  if (m_settings != nullptr) {
    (void)m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight);
  }
}

void PluginsTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
}

void PluginsTab::onPanelCardOpacityChanged(float opacity) {
  if (m_viewSwitch != nullptr) {
    m_viewSwitch->setSurfaceOpacity(opacity);
  }
}

void PluginsTab::syncLiveControls() {
  m_syncing = true;
  if (m_viewSwitch != nullptr) {
    m_viewSwitch->setSelectedIndex(indexFromView(m_view));
  }
  const bool store = m_view == View::Store;
  const bool detail = store && m_store != nullptr && m_store->isDetailView();
  setShown(m_installedHost, !store);
  setShown(m_storeHost, store);
  setShown(m_refreshButton, !detail);
  setShown(m_backButton, detail);
  const auto pageUrl = detail ? m_store->detailPageUrl() : std::optional<std::string>{};
  const auto sourceUrl = detail ? m_store->detailSourceUrl() : std::optional<std::string>{};
  setShown(m_pageButton, pageUrl.has_value());
  setShown(m_sourceButton, sourceUrl.has_value());
  if (m_status != nullptr) {
    if (store && m_storeLoading) {
      m_status->setText(i18n::tr("control-center.plugins.loading-store"));
    } else if (store && m_storeLoaded && m_catalog.empty() && !m_catalogErrors.empty()) {
      m_status->setText(
          i18n::tr("control-center.plugins.store-failed", "sources", StringUtils::join(m_catalogErrors, ", "))
      );
    } else if (store && m_storeLoaded && m_catalog.empty()) {
      m_status->setText(i18n::tr("control-center.plugins.store-empty"));
    } else if (store && m_storeLoaded && !m_catalogErrors.empty()) {
      m_status->setText(
          i18n::tr("control-center.plugins.store-partial", "sources", StringUtils::join(m_catalogErrors, ", "))
      );
    } else {
      m_status->setText({});
    }
  }
  const bool showStatus =
      store && (m_storeLoading || (m_storeLoaded && (m_catalog.empty() || !m_catalogErrors.empty())));
  setShown(m_status, showStatus);
  m_syncing = false;
}

void PluginsTab::applyView(std::size_t index) {
  if (m_syncing) {
    return;
  }
  const View next = viewFromIndex(index);
  if (m_view == next) {
    return;
  }
  m_view = next;
  persistView();
  if (m_view == View::Store && !m_storeLoaded && !m_storeLoading) {
    requestStoreCatalog();
  }
  syncLiveControls();
  PanelManager::instance().requestLayout();
}

void PluginsTab::persistView() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("plugins_tab", "view", m_view == View::Store ? "store" : "installed");
}

void PluginsTab::rebuildInstalledPane() {
  clearChildren(m_installedHost);
  if (m_installedHost == nullptr || m_installedSettings == nullptr) {
    return;
  }
  auto body = m_installedSettings->create(contentScale(), panelCardOpacity());
  if (body != nullptr) {
    body->setFlexGrow(1.0F);
    body->setFillHeight(true);
    m_installedHost->addChild(std::move(body));
  }
}

void PluginsTab::invalidateStore(bool resetContent) {
  if (m_store != nullptr) {
    m_store->detachGrid();
  }
  if (resetContent) {
    m_store.reset();
    m_storeLoaded = false;
    m_catalog.clear();
    m_catalogErrors.clear();
  }
  m_storePopulated = false;
  clearChildren(m_storeHost);
  PanelManager::instance().requestLayout();
}

void PluginsTab::refreshCurrentView() {
  if (m_view == View::Store) {
    requestStoreCatalog(true);
    return;
  }
  if (m_installedSettings != nullptr) {
    m_installedSettings->onExternalChanged();
  }
}

void PluginsTab::requestStoreCatalog(bool force) {
  if (m_pluginManager == nullptr || m_config == nullptr) {
    return;
  }
  if (m_storeLoading && !force) {
    return;
  }
  m_storeLoading = true;
  syncLiveControls();
  const auto generation = ++m_storeGeneration;
  auto* manager = m_pluginManager;
  PluginsConfig snapshot = m_config->config().plugins;
  std::thread([this, manager, generation, force, snapshot = std::move(snapshot)]() mutable {
    manager->fetchStaleCatalogs(snapshot, force);
    std::vector<settings::StoreCatalogEntry> catalog;
    std::vector<std::string> errors;
    for (const auto& source : snapshot.sources) {
      if (!source.enabled) {
        continue;
      }
      auto result = scripting::discoverCatalog(source, scripting::CatalogAccess::Network);
      if (!result.ok) {
        errors.push_back(source.name);
        continue;
      }
      for (auto& entry : result.entries) {
        catalog.push_back(
            settings::StoreCatalogEntry{
                .entry = std::move(entry),
                .source = source.name,
                .sourceConfig = source,
            }
        );
      }
    }
    DeferredCall::callLater([this, generation, catalog = std::move(catalog), errors = std::move(errors)]() mutable {
      if (generation != m_storeGeneration) {
        return;
      }
      m_storeLoading = false;
      m_storeLoaded = true;
      m_catalog = std::move(catalog);
      m_catalogErrors = std::move(errors);
      m_store.reset();
      m_storePopulated = false;
      syncLiveControls();
      PanelManager::instance().requestLayout();
      PanelManager::instance().refresh();
    });
  }).detach();
}

void PluginsTab::ensureStore() {
  if (m_store != nullptr || m_config == nullptr || m_pluginManager == nullptr) {
    return;
  }
  m_store = std::make_unique<settings::PluginStoreContent>(
      m_catalog, m_config,
      settings::PluginStoreCallbacks{
          .setEnabled =
              [this](std::string id, bool enable) {
                if (m_pluginManager == nullptr) {
                  return;
                }
                if (enable) {
                  (void)m_pluginManager->enable(id);
                } else {
                  m_pluginManager->disable(id);
                }
                if (m_installedSettings != nullptr) {
                  m_installedSettings->onExternalChanged();
                }
                invalidateStore(false);
              },
          .isEnabling =
              [this](const std::string& id) {
                return m_pluginManager != nullptr && m_pluginManager->isEnabling(id);
              },
          .isInstalled =
              [this](const std::string& id) {
                return m_pluginManager != nullptr && m_pluginManager->isMaterialized(id);
              },
          .scale = contentScale(),
      },
      &m_fileCache, &m_storeScroll
  );
  m_fileCache.setOnReady([this](const std::string& pluginId, const std::string& filename, const std::string& path) {
    if (m_store != nullptr) {
      m_store->onFileReady(pluginId, filename, path);
    }
  });
  m_store->setOnRebuildNeeded([this]() { invalidateStore(false); });
}

void PluginsTab::populateStoreIfNeeded(Renderer& renderer) {
  if (m_storeHost == nullptr || m_view != View::Store || m_storeLoading) {
    return;
  }
  if (m_storePopulated && m_storeScale == contentScale()) {
    return;
  }
  if (m_storeScale != contentScale() && m_storeScale != 0.0F) {
    m_store.reset();
  }
  clearChildren(m_storeHost);
  if (!m_storeLoaded) {
    return;
  }
  ensureStore();
  if (m_store == nullptr) {
    return;
  }
  if (m_config != nullptr && m_config->config().shell.offlineMode) {
    m_storeHost->addChild(
        settings::makeOfflineModeNotice(contentScale(), i18n::tr("settings.window.offline-mode-notice.plugin-store"))
    );
  }
  m_store->populateBody(*m_storeHost, renderer, m_asyncTextures);
  m_storePopulated = true;
  m_storeScale = contentScale();
  syncLiveControls();
}
