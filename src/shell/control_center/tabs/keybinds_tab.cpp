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

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
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

KeybindsTab::KeybindsTab(const ControlCenterServices& services)
    : m_clipboard(services.clipboard),
      m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(
          services,
          SettingsPaneOptions{
              .section = "keybinds",
              .includedGroups = {"in-noctalia"},
          }
      )),
      m_liveSettings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "keybinds", .includedGroups = {"in-noctalia"}}
      )) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> view = m_config->stateString("keybinds_tab", "view")) {
      m_section = sectionFromState(*view);
    }
  }
  applyLiveGroups();
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
          .out = &m_sectionSwitchHost,
          .align = FlexAlign::Stretch,
          .gap = 0.0F,
          .configure =
              [scale, opacity = panelCardOpacity()](Flex& section) {
                applyCompactCardStyle(section, scale, opacity, false);
              },
      })
  );
  if (m_sectionSwitchHost != nullptr) {
    m_sectionSwitchHost->addChild(
        ui::segmented({
            .out = &m_sectionSwitch,
            .scale = scale,
            .compact = true,
            .surfaceOpacity = panelCardOpacity(),
            .onChange = [this](std::size_t index) { applySection(index); },
        })
    );
  }

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
  const bool niri = niriAvailable();
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
          .out = &m_resetButton,
          .glyph = "refresh",
          .tooltip = i18n::tr("control-center.keybinds.reset-tooltip"),
          .visible = niri,
          .participatesInLayout = niri,
          .onClick = [this]() { resetNiriBinds(); },
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
  m_hint = nullptr;
  m_sectionSwitchHost = nullptr;
  m_sectionSwitch = nullptr;
  m_liveHost = nullptr;
  m_sectionSyncing = false;
  m_copyMissingButton = nullptr;
  m_installButton = nullptr;
  m_resetButton = nullptr;
  m_folderButton = nullptr;
}

void KeybindsTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool KeybindsTab::configureMode() const { return m_settings->configureMode(); }

void KeybindsTab::setSettingsSearchQuery(std::string_view query) {
  if (!query.empty() && m_settings != nullptr && m_settings->configureMode()) {
    m_settings->setConfigureMode(false);
  }
  const bool searching = !query.empty();
  if (m_searching != searching) {
    m_searching = searching;
    applyLiveGroups();
    syncSectionSwitch();
  }
  if (m_liveSettings != nullptr) {
    m_liveSettings->setSearchQuery(std::string(query));
  }
  if (m_settings != nullptr) {
    m_settings->setSearchQuery(std::string(query));
  }
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

void KeybindsTab::onPanelCardOpacityChanged(float opacity) {
  if (m_sectionSwitch != nullptr) {
    m_sectionSwitch->setSurfaceOpacity(opacity);
  }
}

void KeybindsTab::syncLiveControls() {
  const bool niri = niriAvailable();
  setShown(m_copyMissingButton, niri);
  setShown(m_installButton, niri);
  setShown(m_resetButton, niri);
  setShown(m_folderButton, niri);
  if (m_hint != nullptr) {
    m_hint->setText(i18n::tr(
        niri ? "control-center.keybinds.niri-hint" : "control-center.keybinds.in-noctalia-hint"
    ));
  }
  setShown(m_hint, true);
  applyLiveGroups();
  syncSectionSwitch();
  PanelManager::instance().requestLayout();
}

void KeybindsTab::applyLiveGroups() {
  if (m_liveSettings == nullptr) {
    return;
  }
  if (!niriAvailable()) {
    m_liveSettings->setIncludedGroups({"in-noctalia"});
    return;
  }
  if (m_searching) {
    m_liveSettings->setIncludedGroups(
        {"niri-attention", "niri-open", "niri-apps", "niri-capture", "niri-media", "niri-session"}
    );
    return;
  }
  m_liveSettings->setIncludedGroups({std::string(sectionGroup(m_section))});
}

void KeybindsTab::applySection(std::size_t index) {
  if (m_sectionSyncing || index >= m_visibleSections.size()) {
    return;
  }
  const Section next = m_visibleSections[index];
  if (m_section == next) {
    return;
  }
  m_section = next;
  persistSection();
  applyLiveGroups();
  PanelManager::instance().requestLayout();
}

void KeybindsTab::persistSection() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("keybinds_tab", "view", std::string(sectionStateKey(m_section)));
}

void KeybindsTab::syncSectionSwitch() {
  const bool niri = niriAvailable();
  const bool showTabs = niri && !m_searching;
  setShown(m_sectionSwitchHost, showTabs);
  setShown(m_sectionSwitch, showTabs);
  if (!showTabs || m_sectionSwitch == nullptr) {
    return;
  }

  auto visible = visibleSections();
  if (std::ranges::find(visible, m_section) == visible.end()) {
    m_section = visible.empty() ? Section::Open : visible.front();
    persistSection();
    applyLiveGroups();
  }
  if (visible == m_visibleSections && m_sectionSwitch->selectedIndex() < visible.size()
      && visible[m_sectionSwitch->selectedIndex()] == m_section) {
    return;
  }

  m_visibleSections = std::move(visible);
  m_sectionSyncing = true;
  m_sectionSwitch->clearOptions();
  std::size_t selected = 0;
  for (std::size_t i = 0; i < m_visibleSections.size(); ++i) {
    m_sectionSwitch->addOption("", sectionGlyph(m_visibleSections[i]));
    m_sectionSwitch->setOptionTooltip(i, i18n::tr(sectionTabTooltipKey(m_visibleSections[i])));
    if (m_visibleSections[i] == m_section) {
      selected = i;
    }
  }
  m_sectionSwitch->setSelectedIndex(selected);
  m_sectionSyncing = false;
}

bool KeybindsTab::attentionNeeded() const {
  if (!niriAvailable()) {
    return false;
  }
  for (const auto& status : compositors::niri::bindStatuses(compositors::niri::scanConfig())) {
    if (status.boundKeys.empty()) {
      return true;
    }
  }
  return false;
}

std::vector<KeybindsTab::Section> KeybindsTab::visibleSections() const {
  std::vector<Section> sections;
  if (attentionNeeded()) {
    sections.push_back(Section::Attention);
  }
  sections.insert(sections.end(), {Section::Open, Section::Apps, Section::Capture, Section::Sound, Section::Session});
  return sections;
}

std::string_view KeybindsTab::sectionGroup(Section section) {
  switch (section) {
  case Section::Attention:
    return "niri-attention";
  case Section::Open:
    return "niri-open";
  case Section::Apps:
    return "niri-apps";
  case Section::Capture:
    return "niri-capture";
  case Section::Sound:
    return "niri-media";
  case Section::Session:
    return "niri-session";
  }
  return "niri-open";
}

std::string_view KeybindsTab::sectionTabKey(Section section) {
  switch (section) {
  case Section::Attention:
    return "control-center.keybinds.tab-attention";
  case Section::Open:
    return "control-center.keybinds.tab-open";
  case Section::Apps:
    return "control-center.keybinds.tab-apps";
  case Section::Capture:
    return "control-center.keybinds.tab-capture";
  case Section::Sound:
    return "control-center.keybinds.tab-sound";
  case Section::Session:
    return "control-center.keybinds.tab-session";
  }
  return "control-center.keybinds.tab-open";
}

std::string_view KeybindsTab::sectionTabTooltipKey(Section section) {
  switch (section) {
  case Section::Attention:
    return "control-center.keybinds.tab-attention-tooltip";
  case Section::Open:
    return "control-center.keybinds.tab-open-tooltip";
  case Section::Apps:
    return "control-center.keybinds.tab-apps-tooltip";
  case Section::Capture:
    return "control-center.keybinds.tab-capture-tooltip";
  case Section::Sound:
    return "control-center.keybinds.tab-sound-tooltip";
  case Section::Session:
    return "control-center.keybinds.tab-session-tooltip";
  }
  return "control-center.keybinds.tab-open-tooltip";
}

std::string_view KeybindsTab::sectionGlyph(Section section) {
  switch (section) {
  case Section::Attention:
    return "alert-triangle";
  case Section::Open:
    return "layout-dashboard";
  case Section::Apps:
    return "apps";
  case Section::Capture:
    return "screenshot";
  case Section::Sound:
    return "volume";
  case Section::Session:
    return "power";
  }
  return "layout-dashboard";
}

std::string_view KeybindsTab::sectionStateKey(Section section) {
  switch (section) {
  case Section::Attention:
    return "attention";
  case Section::Open:
    return "open";
  case Section::Apps:
    return "apps";
  case Section::Capture:
    return "capture";
  case Section::Sound:
    return "sound";
  case Section::Session:
    return "session";
  }
  return "open";
}

KeybindsTab::Section KeybindsTab::sectionFromState(std::string_view value) {
  if (value == "attention") {
    return Section::Attention;
  }
  if (value == "apps") {
    return Section::Apps;
  }
  if (value == "capture") {
    return Section::Capture;
  }
  if (value == "sound") {
    return Section::Sound;
  }
  if (value == "session") {
    return Section::Session;
  }
  return Section::Open;
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
        "Helix", i18n::tr("settings.schema.keybinds.niri.title"),
        i18n::tr("settings.schema.keybinds.niri.none-missing")
    );
    return;
  }
  if (m_clipboard != nullptr) {
    (void)m_clipboard->copyText(compositors::niri::kdlBlock(missing));
  }
  notify::info(
      "Helix", i18n::tr("settings.schema.keybinds.niri.title"),
      i18n::tr("settings.schema.keybinds.niri.copied-missing")
  );
}

void KeybindsTab::installMissingNiriBinds() {
  const auto result = compositors::niri::installMissing();
  if (!result.ok) {
    notify::error(
        "Helix", i18n::tr("settings.schema.keybinds.niri.title"),
        result.error.empty() ? i18n::tr("settings.schema.keybinds.niri.install-failed") : result.error
    );
    return;
  }
  if (result.added == 0) {
    notify::info(
        "Helix", i18n::tr("settings.schema.keybinds.niri.title"),
        i18n::tr("settings.schema.keybinds.niri.install-none")
    );
  } else {
    notify::info(
        "Helix", i18n::tr("settings.schema.keybinds.niri.title"),
        i18n::tr("settings.schema.keybinds.niri.install-ok", "count", result.added)
    );
  }
  if (m_liveSettings != nullptr) {
    m_liveSettings->onExternalChanged();
  }
  if (m_settings != nullptr) {
    m_settings->onExternalChanged();
  }
  syncLiveControls();
}

void KeybindsTab::resetNiriBinds() {
  const auto result = compositors::niri::resetRecommendedKeys();
  if (!result.ok) {
    notify::error(
        "Helix", i18n::tr("settings.schema.keybinds.niri.title"),
        result.error.empty() ? i18n::tr("settings.schema.keybinds.niri.install-failed") : result.error
    );
    return;
  }
  if (result.added == 0 && result.skippedConflicts == 0) {
    notify::info(
        "Helix", i18n::tr("settings.schema.keybinds.niri.title"),
        i18n::tr("settings.schema.keybinds.niri.reset-none")
    );
  } else if (result.added == 0) {
    notify::info(
        "Helix", i18n::tr("settings.schema.keybinds.niri.title"),
        i18n::tr("settings.schema.keybinds.niri.reset-skipped", "count", result.skippedConflicts)
    );
  } else {
    notify::info(
        "Helix", i18n::tr("settings.schema.keybinds.niri.title"),
        i18n::tr("settings.schema.keybinds.niri.reset-ok", "count", result.added)
    );
  }
  if (m_liveSettings != nullptr) {
    m_liveSettings->onExternalChanged();
  }
  if (m_settings != nullptr) {
    m_settings->onExternalChanged();
  }
  syncLiveControls();
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
