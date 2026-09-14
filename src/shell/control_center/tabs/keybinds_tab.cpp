#include "shell/control_center/tabs/keybinds_tab.h"

#include "compositors/compositor_detect.h"
#include "compositors/niri/niri_config_binds.h"
#include "config/config_service.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"
#include "wayland/clipboard_service.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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

KeybindsTab::Source KeybindsTab::sourceFromIndex(std::size_t index) noexcept {
  return index == 1 ? Source::Niri : Source::InNoctalia;
}

std::size_t KeybindsTab::indexFromSource(Source source) noexcept {
  return source == Source::Niri ? 1 : 0;
}

KeybindsTab::KeybindsTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_clipboard(services.clipboard),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "keybinds", .includedGroups = {"niri-actions"}}
      )),
      m_liveSettings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "keybinds", .includedGroups = {"in-noctalia"}}
      )) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> source = m_config->stateString("keybinds_tab", "source")) {
      if (*source == "niri" && niriAvailable()) {
        m_sourceKind = Source::Niri;
      }
    }
  }
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> KeybindsTab::create() {
  const float scale = contentScale();

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
  });

  auto toolbar = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
  });
  toolbar->addChild(
      ui::segmented({
          .out = &m_source,
          .options =
              std::vector<ui::SegmentedOption>{
                  {.label = i18n::tr("settings.navigation.groups.in-noctalia")},
                  {.label = i18n::tr("settings.navigation.groups.niri")},
              },
          .selectedIndex = indexFromSource(m_sourceKind),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0F,
          .onChange = [this](std::size_t index) { applySource(index); },
      })
  );
  live->addChild(std::move(toolbar));

  live->addChild(
      ui::label({
          .out = &m_hint,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 4,
          .visible = false,
          .participatesInLayout = false,
      })
  );

  live->addChild(
      ui::column({
          .out = &m_liveHost,
          .align = FlexAlign::Stretch,
          .gap = 0.0F,
          .flexGrow = 1.0F,
      })
  );

  syncLiveControls();
  rebuildLivePane();

  auto wrapped = m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
}

std::unique_ptr<Flex> KeybindsTab::createHeaderActions() {
  const float scale = contentScale();
  const bool niri = niriAvailable() && m_sourceKind == Source::Niri;
  auto live = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .out = &m_copyMissingButton,
          .glyph = "copy",
          .tooltip = i18n::tr("control-center.keybinds.copy-missing-tooltip"),
          .visible = niri,
          .participatesInLayout = niri,
          .onClick = [this]() { copyMissingNiriBinds(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      }),
      ui::button({
          .out = &m_installButton,
          .glyph = "plus",
          .tooltip = i18n::tr("control-center.keybinds.install-tooltip"),
          .visible = niri,
          .participatesInLayout = niri,
          .onClick = [this]() { installMissingNiriBinds(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      }),
      ui::button({
          .out = &m_folderButton,
          .glyph = "folder",
          .tooltip = i18n::tr("control-center.keybinds.open-folder-tooltip"),
          .visible = niri,
          .participatesInLayout = niri,
          .onClick = [this]() { openNiriConfigFolder(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  return m_settings->embedHeader(std::move(live), scale);
}

void KeybindsTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  if (m_liveSettings != nullptr) {
    m_liveSettings->onClose();
  }
  m_rootLayout = nullptr;
  m_source = nullptr;
  m_hint = nullptr;
  m_liveHost = nullptr;
  m_copyMissingButton = nullptr;
  m_installButton = nullptr;
  m_folderButton = nullptr;
}

void KeybindsTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool KeybindsTab::configureMode() const { return m_settings->configureMode(); }

void KeybindsTab::setSettingsSearchQuery(std::string_view query) {
  if (!query.empty()) {
    m_settings->setConfigureMode(true);
  }
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* KeybindsTab::settingsPane() { return m_settings.get(); }

void KeybindsTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr) {
    (void)m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight);
  }
}

void KeybindsTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
}

void KeybindsTab::syncLiveControls() {
  m_syncing = true;
  const bool niri = niriAvailable();
  if (!niri && m_sourceKind == Source::Niri) {
    m_sourceKind = Source::InNoctalia;
    persistSource();
  }
  if (m_source != nullptr) {
    m_source->setSelectedIndex(indexFromSource(m_sourceKind));
  }
  setShown(m_source, niri);
  const bool niriSource = niri && m_sourceKind == Source::Niri;
  setShown(m_copyMissingButton, niriSource);
  setShown(m_installButton, niriSource);
  setShown(m_folderButton, niriSource);
  if (m_hint != nullptr) {
    m_hint->setText(i18n::tr(
        niriSource ? "control-center.keybinds.niri-hint" : "control-center.keybinds.in-noctalia-hint"
    ));
  }
  setShown(m_hint, true);
  m_syncing = false;

  const char* group = m_sourceKind == Source::Niri ? "niri" : "in-noctalia";
  if (m_liveSettings != nullptr) {
    m_liveSettings->setIncludedGroups({group});
  }
  PanelManager::instance().requestLayout();
}

void KeybindsTab::applySource(std::size_t index) {
  if (m_syncing) {
    return;
  }
  Source next = sourceFromIndex(index);
  if (!niriAvailable()) {
    next = Source::InNoctalia;
  }
  if (m_sourceKind == next) {
    return;
  }
  m_sourceKind = next;
  persistSource();
  syncLiveControls();
}

void KeybindsTab::persistSource() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString(
      "keybinds_tab", "source", m_sourceKind == Source::Niri ? "niri" : "in-noctalia"
  );
}

void KeybindsTab::rebuildLivePane() {
  clearChildren(m_liveHost);
  if (m_liveHost == nullptr || m_liveSettings == nullptr) {
    return;
  }
  auto body = m_liveSettings->create(contentScale(), panelCardOpacity());
  if (body != nullptr) {
    body->setFlexGrow(1.0F);
    body->setFillHeight(true);
    m_liveHost->addChild(std::move(body));
  }
}

bool KeybindsTab::niriAvailable() const { return compositors::isNiri(); }

void KeybindsTab::copyMissingNiriBinds() {
  const auto missing = compositors::niri::missingRecommended(compositors::niri::scanConfig());
  if (missing.empty()) {
    notify::info(
        "Noctalia", i18n::tr("settings.schema.keybinds.niri.title"),
        i18n::tr("settings.schema.keybinds.niri.none-missing")
    );
    return;
  }
  if (m_clipboard != nullptr) {
    (void)m_clipboard->copyText(compositors::niri::kdlBlock(missing));
  }
  notify::info(
      "Noctalia", i18n::tr("settings.schema.keybinds.niri.title"),
      i18n::tr("settings.schema.keybinds.niri.copied-missing")
  );
}

void KeybindsTab::installMissingNiriBinds() {
  const auto result = compositors::niri::installMissing();
  if (!result.ok) {
    notify::error(
        "Noctalia", i18n::tr("settings.schema.keybinds.niri.title"),
        result.error.empty() ? i18n::tr("settings.schema.keybinds.niri.install-failed") : result.error
    );
    return;
  }
  if (result.added == 0) {
    notify::info(
        "Noctalia", i18n::tr("settings.schema.keybinds.niri.title"),
        i18n::tr("settings.schema.keybinds.niri.install-none")
    );
  } else {
    notify::info(
        "Noctalia", i18n::tr("settings.schema.keybinds.niri.title"),
        i18n::tr("settings.schema.keybinds.niri.install-ok", "count", result.added)
    );
  }
  if (m_liveSettings != nullptr) {
    m_liveSettings->onExternalChanged();
  }
  if (m_settings != nullptr) {
    m_settings->onExternalChanged();
  }
}

void KeybindsTab::openNiriConfigFolder() {
  const auto scan = compositors::niri::scanConfig();
  std::filesystem::path dir = scan.configPath.parent_path();
  if (dir.empty()) {
    dir = std::filesystem::path(FileUtils::configDir()).parent_path() / "niri";
  }
  std::error_code ec;
  if (!dir.empty()) {
    FileUtils::createPrivateDirectories(dir, ec);
  }
  if (ec || dir.empty() || !process::commandExists("xdg-open")) {
    if (m_hint != nullptr) {
      m_hint->setText(i18n::tr("control-center.keybinds.open-folder-failed"));
      setShown(m_hint, true);
    }
    return;
  }
  (void)process::runAsync(std::vector<std::string>{"xdg-open", dir.string()});
}
