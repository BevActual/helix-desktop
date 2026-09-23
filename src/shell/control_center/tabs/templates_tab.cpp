#include "shell/control_center/tabs/templates_tab.h"

#include "config/config_service.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/template_store_content.h"
#include "system/desktop_entry.h"
#include "theme/template_apply_service.h"
#include "theme/adw_gtk3.h"
#include "theme/builtin_templates.h"
#include "theme/community_templates.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
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

  [[nodiscard]] std::string stripDesktopSuffix(std::string id) {
    constexpr std::string_view kSuffix = ".desktop";
    if (id.size() > kSuffix.size() && id.ends_with(kSuffix)) {
      id.resize(id.size() - kSuffix.size());
    }
    return id;
  }

  [[nodiscard]] std::string lastIdComponent(std::string_view id) {
    const auto pos = id.rfind('.');
    if (pos == std::string_view::npos || pos + 1 >= id.size()) {
      return std::string(id);
    }
    return std::string(id.substr(pos + 1));
  }

  [[nodiscard]] std::string sanitizeTemplateId(std::string_view raw) {
    std::string id;
    id.reserve(raw.size());
    for (const unsigned char ch : raw) {
      if (std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.') {
        id.push_back(static_cast<char>(std::tolower(ch)));
        continue;
      }
      if (!id.empty() && id.back() != '-') {
        id.push_back('-');
      }
    }
    while (!id.empty() && (id.front() == '-' || id.front() == '.')) {
      id.erase(id.begin());
    }
    while (!id.empty() && (id.back() == '-' || id.back() == '.')) {
      id.pop_back();
    }
    if (id.empty() || !noctalia::theme::isSafeCommunityTemplateId(id)) {
      return "app";
    }
    return id;
  }

  [[nodiscard]] std::string templateIdForApp(const DesktopEntry& app) {
    return sanitizeTemplateId(lastIdComponent(stripDesktopSuffix(app.id)));
  }

  [[nodiscard]] int matchScore(const noctalia::theme::AvailableTemplate& entry, const std::vector<std::string>& needles) {
    const std::string id = StringUtils::toLower(entry.id);
    const std::string name = StringUtils::toLower(entry.displayName);
    int best = 0;
    for (const auto& needle : needles) {
      if (needle.empty()) {
        continue;
      }
      if (id == needle || name == needle) {
        best = std::max(best, 100);
        continue;
      }
      if (id.ends_with(needle) || needle.ends_with(id)) {
        best = std::max(best, 90);
        continue;
      }
      if (needle.size() >= 4 && (name.find(needle) != std::string::npos || id.find(needle) != std::string::npos)) {
        best = std::max(best, 80);
      }
    }
    return best;
  }

  struct CatalogMatch {
    TemplatesTab::Source source = TemplatesTab::Source::Builtin;
    std::string id;
  };

  [[nodiscard]] std::optional<CatalogMatch> matchExistingTemplate(const DesktopEntry& app) {
    std::vector<std::string> needles;
    const std::string desktopId = StringUtils::toLower(stripDesktopSuffix(app.id));
    needles.push_back(desktopId);
    needles.push_back(StringUtils::toLower(lastIdComponent(desktopId)));
    needles.push_back(app.nameLower);
    if (desktopId.find("firefox") != std::string::npos || app.nameLower.find("firefox") != std::string::npos) {
      needles.emplace_back("pywalfox-beta4");
      needles.emplace_back("pywalfox");
      needles.emplace_back("firefox");
    }
    if (desktopId.find("code") != std::string::npos || app.nameLower.find("visual studio code") != std::string::npos) {
      needles.emplace_back("vscode");
      needles.emplace_back("vscodium");
    }
    if (desktopId.find("cursor") != std::string::npos || desktopId.find("230313mzl4w4u92") != std::string::npos
        || app.nameLower.find("cursor") != std::string::npos) {
      needles.emplace_back("cursor");
    }
    if (desktopId.find("warp") != std::string::npos || app.nameLower.find("warp") != std::string::npos) {
      needles.emplace_back("warp");
    }

    CatalogMatch best;
    int bestScore = 0;
    const auto consider = [&](TemplatesTab::Source source, const noctalia::theme::AvailableTemplate& entry) {
      const int score = matchScore(entry, needles);
      if (score > bestScore) {
        bestScore = score;
        best = CatalogMatch{.source = source, .id = entry.id};
      }
    };
    for (const auto& entry : noctalia::theme::availableTemplates()) {
      consider(TemplatesTab::Source::Builtin, entry);
    }
    for (const auto& entry : noctalia::theme::CommunityTemplateService::availableTemplates()) {
      consider(TemplatesTab::Source::Community, entry);
    }
    if (bestScore < 80 || best.id.empty()) {
      return std::nullopt;
    }
    return best;
  }

  [[nodiscard]] const DesktopEntry* findDesktopEntry(std::string_view id) {
    for (const auto& entry : desktopEntries()) {
      if (entry.id == id) {
        return &entry;
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool writeStarterTemplate(const std::filesystem::path& path, std::string_view appName, std::string_view id) {
    std::error_code ec;
    FileUtils::createPrivateDirectories(path.parent_path(), ec);
    if (ec) {
      return false;
    }
    if (std::filesystem::exists(path, ec)) {
      return true;
    }
    std::ofstream out(path);
    if (!out) {
      return false;
    }
    out << "# Helix user template for " << appName << " (" << id << ")\n";
    out << "# Colors update on each theme change. Point output_path at the app's theme file.\n";
    out << "# Tokens: https://docs.noctalia.dev/noctalia/theming/templates/\n\n";
    out << "background=" << "{{ colors.surface.default.hex }}\n";
    out << "foreground=" << "{{ colors.on_surface.default.hex }}\n";
    out << "primary=" << "{{ colors.primary.default.hex }}\n";
    out << "secondary=" << "{{ colors.secondary.default.hex }}\n";
    out << "tertiary=" << "{{ colors.tertiary.default.hex }}\n";
    out << "error=" << "{{ colors.error.default.hex }}\n";
    out << "outline=" << "{{ colors.outline.default.hex }}\n";
    return static_cast<bool>(out);
  }

  [[nodiscard]] bool containsId(const std::vector<std::string>& ids, std::string_view id) {
    return std::ranges::find(ids, id) != ids.end();
  }

} // namespace

TemplatesTab::Source TemplatesTab::sourceFromIndex(std::size_t index) noexcept {
  switch (index) {
  case 1:
    return Source::Community;
  case 2:
    return Source::User;
  default:
    return Source::Builtin;
  }
}

std::size_t TemplatesTab::indexFromSource(Source source) noexcept {
  switch (source) {
  case Source::Community:
    return 1;
  case Source::User:
    return 2;
  case Source::Builtin:
  default:
    return 0;
  }
}

TemplatesTab::TemplatesTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_applyTemplates(services.settingsActions.applyTemplates),
      m_templateApply(services.templateApply),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "templates", .includedGroups = {"greeter"}}
      )) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> source = m_config->stateString("templates_tab", "source")) {
      if (*source == "community") {
        m_sourceKind = Source::Community;
      } else if (*source == "user") {
        m_sourceKind = Source::User;
      }
    }
  }
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> TemplatesTab::create() {
  const float scale = contentScale();
  const float opacity = panelCardOpacity();

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .flexGrow = 1.0F,
  });

  auto sectionCard = ui::column({
      .align = FlexAlign::Stretch,
      .configure =
          [scale, opacity](Flex& section) {
            applyCompactCardStyle(section, scale, opacity, false);
          },
  });

  auto toolbar = ui::row({
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
  });
  auto browse = ui::row({
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceXs * scale,
      .flexGrow = 1.0F,
  });
  browse->addChild(
      ui::segmented({
          .out = &m_source,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = "",
                      .glyph = "package",
                      .tooltip = i18n::tr("control-center.templates.built-in-tooltip"),
                  },
                  {
                      .label = "",
                      .glyph = "users",
                      .tooltip = i18n::tr("control-center.templates.community-tooltip"),
                  },
                  {
                      .label = "",
                      .glyph = "user",
                      .tooltip = i18n::tr("control-center.templates.user-tooltip"),
                  },
              },
          .selectedIndex = indexFromSource(m_sourceKind),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = opacity,
          .onChange = [this](std::size_t index) { applySource(index); },
      })
  );
  toolbar->addChild(std::move(browse));

  auto enableCluster = ui::row({
      .out = &m_catalogWell,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .visible = m_sourceKind != Source::User,
      .participatesInLayout = m_sourceKind != Source::User,
  });
  enableCluster->addChild(
      ui::label({
          .text = i18n::tr("control-center.templates.enable"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
      })
  );
  enableCluster->addChild(
      ui::toggle({
          .out = &m_enable,
          .checkedImmediate = false,
          .toggleSize = ToggleSize::Medium,
          .scale = scale,
          .onChange = [this](bool enabled) { applyEnable(enabled); },
      })
  );
  toolbar->addChild(std::move(enableCluster));
  sectionCard->addChild(std::move(toolbar));

  auto gtkFollow = ui::column({
      .out = &m_gtkFollowSection,
      .align = FlexAlign::Stretch,
      .visible = false,
      .participatesInLayout = false,
  });
  addWellDivider(*gtkFollow, scale);
  m_gtkFollowHint = addCaption(*gtkFollow, i18n::tr("control-center.templates.gtk-follow-off"), scale, 4);
  auto gtkRow = makeControlRow(i18n::tr("control-center.templates.gtk-follow"), scale, nullptr, false);
  gtkRow->addChild(
      ui::button({
          .out = &m_gtkFollowButton,
          .text = i18n::tr("control-center.templates.enable"),
          .fontSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.templates.gtk-follow-enable"),
          .minHeight = Style::controlHeightSm * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick = [this]() { enableGtkTemplates(); },
      })
  );
  gtkFollow->addChild(std::move(gtkRow));
  sectionCard->addChild(std::move(gtkFollow));
  // Store search, filters, and sort rebuild into this host so they sit on the compact card
  // (Wallpaper navigation); the grid alone stays leftover.
  sectionCard->addChild(
      ui::column({
          .out = &m_storeToolbarHost,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
      })
  );
  sectionCard->addChild(
      ui::label({
          .out = &m_hint,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 4,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  live->addChild(std::move(sectionCard));

  live->addChild(
      ui::column({
          .out = &m_storeHost,
          .align = FlexAlign::Stretch,
          .gap = 0.0F,
          .flexGrow = 1.0F,
      })
  );

  m_storePopulated = false;
  m_storeScale = 0.0F;
  m_liveEnabled = false;
  m_liveSource = sourceFromIndex((indexFromSource(m_sourceKind) + 1) % 3);
  m_catalogKey.clear();

  auto wrapped = m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
}

std::unique_ptr<Flex> TemplatesTab::createHeaderActions() {
  const float scale = contentScale();
  auto live = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .out = &m_applyButton,
          .glyph = "refresh",
          .tooltip = i18n::tr("control-center.templates.apply-tooltip"),
          .onClick = [this]() { applyNow(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      }),
      ui::button({
          .out = &m_folderButton,
          .glyph = "folder",
          .tooltip = i18n::tr("control-center.templates.open-folder-tooltip"),
          .visible = m_sourceKind == Source::User,
          .participatesInLayout = m_sourceKind == Source::User,
          .onClick = [this]() { openTemplatesFolder(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      }),
      ui::button({
          .out = &m_newButton,
          .glyph = "plus",
          .tooltip = i18n::tr("control-center.templates.new-tooltip"),
          .onClick = [this]() { openAppPicker(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  return m_settings->embedHeader(std::move(live), scale);
}

void TemplatesTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_store.reset();
  m_rootLayout = nullptr;
  m_source = nullptr;
  m_catalogWell = nullptr;
  m_enable = nullptr;
  m_hint = nullptr;
  m_storeHost = nullptr;
  m_storeToolbarHost = nullptr;
  m_applyButton = nullptr;
  m_folderButton = nullptr;
  m_newButton = nullptr;
  m_gtkFollowSection = nullptr;
  m_gtkFollowHint = nullptr;
  m_gtkFollowButton = nullptr;
  m_storePopulated = false;
}

void TemplatesTab::setConfigureMode(bool show) {
  const bool was = m_settings->configureMode();
  m_settings->setConfigureMode(show);
  if (was && !show) {
    invalidateStore(true);
  }
}

bool TemplatesTab::configureMode() const { return m_settings->configureMode(); }

void TemplatesTab::setSettingsSearchQuery(std::string_view query) {
  if (!query.empty()) {
    m_settings->setConfigureMode(true);
  }
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* TemplatesTab::settingsPane() { return m_settings.get(); }

void TemplatesTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->configureMode()) {
    (void)m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight);
    return;
  }
  populateStoreIfNeeded(renderer);
  if (m_settings != nullptr) {
    (void)m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight);
  }
}

void TemplatesTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
}

void TemplatesTab::onPanelCardOpacityChanged(float opacity) {
  if (m_source != nullptr) {
    m_source->setSurfaceOpacity(opacity);
  }
}

void TemplatesTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  m_syncing = true;
  const bool enabled = sourceEnabled();
  const bool userSource = m_sourceKind == Source::User;
  if (m_source != nullptr) {
    m_source->setSelectedIndex(indexFromSource(m_sourceKind));
  }
  if (m_enable != nullptr) {
    m_enable->setChecked(enabled);
  }
  setShown(m_catalogWell, !userSource);
  const auto& templates = m_config->config().theme.templates;
  m_applyFailures = m_templateApply != nullptr ? m_templateApply->lastFailures() : std::unordered_map<std::string, std::string>{};
  const bool gtkMissingTheme = m_sourceKind == Source::Builtin && enabled
      && noctalia::theme::gtkTemplatesEnabled(templates) && !noctalia::theme::adwGtk3ThemePresent();
  std::size_t applyFailureCount = 0;
  if (enabled) {
    const auto countIf = [&](const std::string& id) {
      if (m_applyFailures.contains(id)) {
        ++applyFailureCount;
      }
    };
    if (m_sourceKind == Source::Builtin) {
      for (const auto& id : templates.builtinIds) {
        countIf(id);
      }
    } else if (m_sourceKind == Source::Community) {
      for (const auto& id : templates.communityIds) {
        countIf(id);
      }
    } else {
      for (const auto& entry : templates.userTemplates) {
        if (entry.enabled) {
          countIf(entry.id);
        }
      }
    }
  }
  if (m_hint != nullptr) {
    if (gtkMissingTheme) {
      m_hint->setText(i18n::tr("control-center.templates.gtk-missing-theme"));
    } else if (applyFailureCount > 0) {
      m_hint->setText(
          i18n::tr("control-center.templates.apply-failures", "count", std::to_string(applyFailureCount))
      );
    } else if (m_sourceKind == Source::Community) {
      m_hint->setText(i18n::tr("control-center.templates.disabled-community"));
    } else if (userSource) {
      m_hint->setText(i18n::tr("control-center.templates.disabled-user"));
    } else {
      m_hint->setText(i18n::tr("control-center.templates.disabled-builtin"));
    }
  }
  const bool showHint = gtkMissingTheme || applyFailureCount > 0 || !enabled;
  setShown(m_hint, showHint);
  setShown(m_storeHost, enabled);
  setShown(m_storeToolbarHost, enabled);
  setShown(m_folderButton, userSource);

  const std::string nextCatalog = catalogKey();
  const bool sourceChanged = m_liveSource != m_sourceKind;
  const bool enableChanged = m_liveEnabled != enabled;
  const bool catalogChanged = m_catalogKey != nextCatalog;
  m_liveEnabled = enabled;
  m_liveSource = m_sourceKind;
  m_catalogKey = nextCatalog;
  m_syncing = false;

  if (sourceChanged || enableChanged || catalogChanged) {
    invalidateStore(true);
  }

  syncGtkFollow();
}

void TemplatesTab::syncGtkFollow() {
  if (m_config == nullptr) {
    return;
  }
  const bool gtkOn = noctalia::theme::gtkTemplatesEnabled(m_config->config().theme.templates);
  const bool showGtkFollow = !gtkOn && m_sourceKind == Source::Builtin
      && m_config->config().theme.templates.enableBuiltinTemplates;
  if (m_gtkFollowHint != nullptr) {
    m_gtkFollowHint->setText(i18n::tr("control-center.templates.gtk-follow-off"));
  }
  setShown(m_gtkFollowSection, showGtkFollow);
}

void TemplatesTab::enableGtkTemplates() {
  if (m_config == nullptr) {
    return;
  }
  const auto& current = m_config->config().theme.templates;
  std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
  if (!current.enableBuiltinTemplates) {
    overrides.push_back({{"theme", "templates", "enable_builtin_templates"}, true});
  }
  auto ids = current.builtinIds;
  const bool idsChanged = noctalia::theme::appendGtkTemplateIds(ids);
  if (idsChanged) {
    overrides.push_back({{"theme", "templates", "builtin_ids"}, std::move(ids)});
  }
  if (!overrides.empty()) {
    (void)m_config->setOverrides(std::move(overrides));
  }
  if (m_applyTemplates) {
    m_applyTemplates();
  }
  syncGtkFollow();
}

void TemplatesTab::applySource(std::size_t index) {
  if (m_syncing) {
    return;
  }
  const Source next = sourceFromIndex(index);
  if (m_sourceKind == next) {
    return;
  }
  m_sourceKind = next;
  persistSource();
  syncLiveControls();
}

void TemplatesTab::persistSource() {
  if (m_config == nullptr) {
    return;
  }
  const char* value = "builtin";
  if (m_sourceKind == Source::Community) {
    value = "community";
  } else if (m_sourceKind == Source::User) {
    value = "user";
  }
  (void)m_config->setStateString("templates_tab", "source", value);
}

void TemplatesTab::applyEnable(bool enabled) {
  if (m_syncing || m_config == nullptr || m_sourceKind == Source::User) {
    return;
  }
  const char* key = m_sourceKind == Source::Community ? "enable_community_templates" : "enable_builtin_templates";
  (void)m_config->setOverride({"theme", "templates", key}, enabled);
}

void TemplatesTab::invalidateStore(bool resetContent) {
  if (resetContent) {
    m_store.reset();
  }
  m_storePopulated = false;
  clearChildren(m_storeHost);
  clearChildren(m_storeToolbarHost);
  PanelManager::instance().requestLayout();
}

void TemplatesTab::ensureStore() {
  if (m_store != nullptr || m_config == nullptr) {
    return;
  }
  const auto& templates = m_config->config().theme.templates;
  std::vector<noctalia::theme::AvailableTemplate> catalog;
  std::unordered_set<std::string> selected;
  std::function<void(std::vector<std::string>)> setSelected;
  std::string emptyText;
  if (m_sourceKind == Source::Community) {
    catalog = noctalia::theme::CommunityTemplateService::availableTemplates();
    selected.insert(templates.communityIds.begin(), templates.communityIds.end());
    setSelected = [this](std::vector<std::string> ids) {
      if (m_config != nullptr) {
        (void)m_config->setOverride({"theme", "templates", "community_ids"}, std::move(ids));
      }
    };
    emptyText = i18n::tr("settings.schema.templates.community-ids.empty");
  } else if (m_sourceKind == Source::User) {
    catalog.reserve(templates.userTemplates.size());
    for (const auto& entry : templates.userTemplates) {
      noctalia::theme::AvailableTemplate item;
      item.id = entry.id;
      item.displayName = entry.id;
      item.category = "user";
      item.outputPaths = entry.outputPaths;
      item.outputDynamic = !entry.outputPathDynamic.empty();
      catalog.push_back(std::move(item));
      if (entry.enabled) {
        selected.insert(entry.id);
      }
    }
    setSelected = [this](std::vector<std::string> ids) {
      if (m_config == nullptr) {
        return;
      }
      const std::unordered_set<std::string> enabled(ids.begin(), ids.end());
      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
      for (const auto& entry : m_config->config().theme.templates.userTemplates) {
        const bool on = enabled.contains(entry.id);
        if (entry.enabled != on) {
          overrides.push_back({{"theme", "templates", "user", entry.id, "enabled"}, on});
        }
      }
      if (!overrides.empty()) {
        (void)m_config->setOverrides(std::move(overrides));
      }
    };
    emptyText = i18n::tr("control-center.templates.empty-user");
  } else {
    catalog = noctalia::theme::availableTemplates();
    selected.insert(templates.builtinIds.begin(), templates.builtinIds.end());
    setSelected = [this](std::vector<std::string> ids) {
      if (m_config != nullptr) {
        (void)m_config->setOverride({"theme", "templates", "builtin_ids"}, std::move(ids));
      }
    };
    emptyText = i18n::tr("settings.schema.templates.builtin-ids.empty");
  }
  m_store = std::make_unique<settings::TemplateStoreContent>(
      std::move(catalog), std::move(selected), m_config,
      settings::TemplateStoreCallbacks{
          .setSelected = std::move(setSelected),
          .scale = contentScale(),
          .inset = false,
          .toolbarHost = m_storeToolbarHost,
          .emptyCatalogText = std::move(emptyText),
          .onEditUser =
              m_sourceKind == Source::User
                  ? std::function<void(std::string)>{
                        [this](std::string id) {
                          if (m_settings != nullptr) {
                            m_settings->openUserTemplateEditor(std::move(id));
                          }
                        }
                    }
                  : nullptr,
          .applyErrorForId =
              [this](std::string_view id) {
                const auto it = m_applyFailures.find(std::string(id));
                return it == m_applyFailures.end() ? std::string{} : it->second;
              },
      }
  );
  m_store->setOnRebuildNeeded([this]() { invalidateStore(false); });
}

void TemplatesTab::populateStoreIfNeeded(Renderer& renderer) {
  if (m_config == nullptr || m_storeHost == nullptr || !sourceEnabled()) {
    return;
  }
  if (m_storePopulated && m_storeScale == contentScale()) {
    return;
  }
  if (m_storeScale != contentScale() && m_storeScale != 0.0F) {
    m_store.reset();
  }
  clearChildren(m_storeHost);
  clearChildren(m_storeToolbarHost);
  ensureStore();
  if (m_store == nullptr) {
    return;
  }
  if (m_sourceKind == Source::Community && m_config->config().shell.offlineMode) {
    m_storeHost->addChild(
        settings::makeOfflineModeNotice(contentScale(), i18n::tr("settings.window.offline-mode-notice.template-store"))
    );
  }
  m_store->populateBody(*m_storeHost, renderer);
  m_storePopulated = true;
  m_storeScale = contentScale();
}

bool TemplatesTab::sourceEnabled() const {
  if (m_sourceKind == Source::User) {
    return true;
  }
  if (m_config == nullptr) {
    return false;
  }
  const auto& templates = m_config->config().theme.templates;
  return m_sourceKind == Source::Community ? templates.enableCommunityTemplates : templates.enableBuiltinTemplates;
}

std::string TemplatesTab::catalogKey() const {
  std::string key;
  if (m_sourceKind == Source::Builtin) {
    key = "builtin\n";
    if (m_config != nullptr) {
      for (const auto& id : m_config->config().theme.templates.builtinIds) {
        key += id;
        key += '\n';
      }
    }
  } else if (m_sourceKind == Source::User) {
    key = "user\n";
    if (m_config != nullptr) {
      for (const auto& entry : m_config->config().theme.templates.userTemplates) {
        key += entry.id;
        key += '\t';
        key += entry.enabled ? '1' : '0';
        key += '\t';
        key += entry.inputPath;
        key += '\t';
        for (const auto& path : entry.outputPaths) {
          key += path;
          key += ',';
        }
        key += '\t';
        key += entry.postHook;
        key += '\n';
      }
    }
  } else {
    key = "community\n";
    for (const auto& entry : noctalia::theme::CommunityTemplateService::availableTemplates()) {
      key += entry.id;
      key += '\n';
    }
  }
  key += "failures\n";
  std::vector<std::string> failedIds;
  failedIds.reserve(m_applyFailures.size());
  for (const auto& [id, error] : m_applyFailures) {
    failedIds.push_back(id);
  }
  std::ranges::sort(failedIds);
  for (const auto& id : failedIds) {
    key += id;
    key += '\t';
    key += m_applyFailures.at(id);
    key += '\n';
  }
  return key;
}

void TemplatesTab::openAppPicker() {
  if (m_config == nullptr || m_settings == nullptr) {
    return;
  }
  std::vector<settings::SelectOption> options;
  options.reserve(desktopEntries().size());
  for (const auto& entry : desktopEntries()) {
    if (entry.noDisplay || entry.hidden || entry.id.empty() || entry.name.empty()) {
      continue;
    }
    options.push_back(
        settings::SelectOption{
            .value = entry.id,
            .label = entry.name,
            .description = entry.id,
        }
    );
  }
  std::ranges::sort(options, [](const settings::SelectOption& a, const settings::SelectOption& b) {
    return StringUtils::naturalCaseInsensitiveCompare(a.label, b.label) < 0;
  });
  m_settings->openSearchPicker(
      settings::SearchPickerOpenRequest{
          .title = i18n::tr("control-center.templates.new"),
          .options = std::move(options),
          .placeholder = i18n::tr("control-center.templates.search-app"),
          .emptyText = i18n::tr("ui.controls.search-picker.empty"),
          .onSelect =
              [this](const std::string& value) {
                if (const DesktopEntry* app = findDesktopEntry(value); app != nullptr) {
                  createFromApp(*app);
                }
              },
      }
  );
}

void TemplatesTab::createFromApp(const DesktopEntry& app) {
  if (const auto match = matchExistingTemplate(app)) {
    enableCatalogTemplate(match->source, match->id);
    return;
  }
  createUserTemplate(app);
}

void TemplatesTab::enableCatalogTemplate(Source source, std::string id) {
  if (m_config == nullptr || id.empty()) {
    return;
  }
  auto& current = m_config->config().theme.templates;
  std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
  if (source == Source::Community) {
    if (!current.enableCommunityTemplates) {
      overrides.push_back({{"theme", "templates", "enable_community_templates"}, true});
    }
    auto ids = current.communityIds;
    if (!containsId(ids, id)) {
      ids.push_back(id);
      overrides.push_back({{"theme", "templates", "community_ids"}, std::move(ids)});
    }
  } else {
    if (!current.enableBuiltinTemplates) {
      overrides.push_back({{"theme", "templates", "enable_builtin_templates"}, true});
    }
    auto ids = current.builtinIds;
    if (!containsId(ids, id)) {
      ids.push_back(id);
      overrides.push_back({{"theme", "templates", "builtin_ids"}, std::move(ids)});
    }
  }
  if (!overrides.empty()) {
    (void)m_config->setOverrides(std::move(overrides));
  }
  m_sourceKind = source;
  persistSource();
  syncLiveControls();
}

void TemplatesTab::createUserTemplate(const DesktopEntry& app) {
  if (m_config == nullptr) {
    return;
  }
  std::string id = templateIdForApp(app);
  for (const auto& entry : m_config->config().theme.templates.userTemplates) {
    if (entry.id == id) {
      if (!entry.enabled) {
        (void)m_config->setOverride({"theme", "templates", "user", id, "enabled"}, true);
      }
      m_sourceKind = Source::User;
      persistSource();
      syncLiveControls();
      if (m_settings != nullptr) {
        m_settings->openUserTemplateEditor(id);
      }
      return;
    }
  }

  const std::filesystem::path templatesDir = std::filesystem::path(FileUtils::configDir()) / "templates";
  const std::filesystem::path inputPath = templatesDir / (id + ".conf");
  if (!writeStarterTemplate(inputPath, app.name, id)) {
    if (m_hint != nullptr) {
      m_hint->setText(i18n::tr("control-center.templates.create-failed"));
      setShown(m_hint, true);
    }
    return;
  }

  const std::string input = "$XDG_CONFIG_HOME/noctalia/templates/" + id + ".conf";
  const std::string output = "$XDG_CONFIG_HOME/noctalia/templates/rendered/" + id;
  (void)m_config->setOverrides(
      {
          {{"theme", "templates", "user", id, "enabled"}, true},
          {{"theme", "templates", "user", id, "input_path"}, input},
          {{"theme", "templates", "user", id, "output_path"}, std::vector<std::string>{output}},
      }
  );
  m_sourceKind = Source::User;
  persistSource();
  syncLiveControls();
  if (m_settings != nullptr) {
    m_settings->openUserTemplateEditor(id);
  }
  if (process::commandExists("xdg-open")) {
    (void)process::runAsync(std::vector<std::string>{"xdg-open", inputPath.string()});
  }
}

void TemplatesTab::applyNow() {
  if (m_applyTemplates) {
    m_applyTemplates();
  }
}

void TemplatesTab::openTemplatesFolder() {
  const std::filesystem::path dir = std::filesystem::path(FileUtils::configDir()) / "templates";
  std::error_code ec;
  FileUtils::createPrivateDirectories(dir, ec);
  if (ec || !process::commandExists("xdg-open")) {
    if (m_hint != nullptr) {
      m_hint->setText(i18n::tr("control-center.templates.open-folder-failed"));
      setShown(m_hint, true);
    }
    return;
  }
  (void)process::runAsync(std::vector<std::string>{"xdg-open", dir.string()});
}
