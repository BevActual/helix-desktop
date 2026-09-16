#include "shell/control_center/settings_pane.h"

#include "calendar/calendar_service.h"
#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "compositors/niri/niri_config_blur.h"
#include "compositors/niri/niri_config_overview.h"
#include "compositors/niri/niri_layout_theme.h"
#include "compositors/niri/niri_runtime.h"
#include "compositors/niri/niri_window_rules.h"
#include "config/atomic_file.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/process/process.h"
#include "core/ui_phase.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/text/font_weight_catalog.h"
#include "scripting/plugin_manager.h"
#include "scripting/plugin_registry.h"
#include "shell/bar/widget_action.h"
#include "shell/control_center/tab.h"
#include "shell/greeter/greeter_appearance_sync.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "shell/profile/avatar_path.h"
#include "shell/settings/font_family_catalog.h"
#include "shell/settings/settings_bar_management.h"
#include "shell/settings/settings_content_plugins.h"
#include "shell/settings/settings_hosted_editors.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_runtime.h"
#include "system/battery_warning_monitor.h"
#include "system/day_night_schedule.h"
#include "system/dependency_service.h"
#include "theme/builtin_templates.h"
#include "theme/community_palettes.h"
#include "theme/community_templates.h"
#include "theme/custom_palettes.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select.h"
#include "ui/controls/toggle.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/palette.h"
#include "ui/popup_parent.h"
#include "ui/scroll_into_view.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <thread>
#include <variant>
#include <vector>

namespace {

  bool useLightPalettePreview(ThemeMode mode) { return mode == ThemeMode::Light; }

  ColorSwatchPreview palettePreviewFromMetadata(const noctalia::theme::AvailablePalette::PreviewMode& metadata) {
    ColorSwatchPreview preview;
    Color surface;
    if (tryParseHexColor(metadata.surface, surface)) {
      preview.surface = fixedColorSpec(surface);
    }
    preview.swatches.reserve(metadata.accents.size());
    for (const auto& hexColor : metadata.accents) {
      Color color;
      if (tryParseHexColor(hexColor, color)) {
        preview.swatches.push_back(fixedColorSpec(color));
      }
    }
    return preview;
  }

  ColorSwatchPreview availablePalettePreview(const noctalia::theme::AvailablePalette& palette, ThemeMode mode) {
    if (useLightPalettePreview(mode)) {
      ColorSwatchPreview preview = palettePreviewFromMetadata(palette.preview.light);
      if (!preview.empty()) {
        return preview;
      }
      return palettePreviewFromMetadata(palette.preview.dark);
    }
    return palettePreviewFromMetadata(palette.preview.dark);
  }

  std::string translatedIpcActionField(std::string_view command, std::string_view field) {
    return i18n::tr(std::format("settings.widgets.actions.commands.{}.{}", command, field));
  }

  [[nodiscard]] compositors::niri::BlurSettings niriBlurSettingsFrom(const ThemeConfig::NiriLayoutThemeConfig& niri) {
    return {
        .enabled = niri.blur,
        .passes = niri.blurPasses,
        .offset = niri.blurOffset,
        .noise = niri.blurNoise,
    };
  }

  [[nodiscard]] bool isNiriBlurPersistPath(const std::vector<std::string>& path) {
    return path.size() == 3 && path[0] == "theme" && path[1] == "niri_layout"
        && (path[2] == "blur" || path[2] == "blur_passes" || path[2] == "blur_offset" || path[2] == "blur_noise");
  }

  [[nodiscard]] bool isNiriLayoutPersistPath(const std::vector<std::string>& path) {
    return path.size() == 3 && path[0] == "theme" && path[1] == "niri_layout"
        && (path[2] == "focus_ring" || path[2] == "border" || path[2] == "gaps" || path[2] == "struts"
            || path[2] == "default_column_width");
  }

  [[nodiscard]] bool isNiriWindowRulesPersistPath(const std::vector<std::string>& path) {
    return path.size() == 3 && path[0] == "theme" && path[1] == "niri_layout"
        && path[2] == "draw_border_with_background";
  }

  std::string upowerDeviceLabel(const UPowerDeviceInfo& device) {
    const std::string nativeName =
        !device.nativePath.empty() ? StringUtils::pathTail(device.nativePath) : StringUtils::pathTail(device.path);

    std::string label;
    if (!device.vendor.empty() && !device.model.empty()) {
      label = device.vendor + " " + device.model;
    } else if (!device.model.empty()) {
      label = device.model;
    } else if (!device.vendor.empty()) {
      label = device.vendor;
    } else {
      label = nativeName;
    }

    if (!nativeName.empty() && label != nativeName) {
      label += " (" + nativeName + ")";
    }
    return label;
  }

  bool isCustomSchedulePath(const std::vector<std::string>& path) {
    return path.size() == 2
        && path[0] == "location"
        && (path[1] == "custom_schedule" || path[1] == "sunset" || path[1] == "sunrise");
  }

  std::string settingsMutationError(const ConfigService& config, std::string fallback) {
    return config.lastMutationError().empty() ? fallback : config.lastMutationError();
  }

} // namespace

SettingsPane::SettingsPane(const ControlCenterServices& services, SettingsPaneOptions options)
    : m_config(services.config), m_clipboard(services.clipboard), m_platform(services.platform),
      m_dependencies(services.dependencies), m_upower(services.upower), m_accounts(services.accounts),
      m_ipc(services.ipc), m_hostedEditors(services.hostedEditors), m_pluginManager(services.pluginManager),
      m_asyncTextures(services.asyncTextures), m_calendar(services.calendar), m_modalHost(services.modalHost),
      m_actions(services.settingsActions), m_openStore(std::move(options.openStore)),
      m_selectedSection(std::move(options.section)),
      m_includedGroups(std::move(options.includedGroups)), m_compact(options.compact) {
  m_showAdvanced = m_config != nullptr ? m_config->config().shell.settingsShowAdvanced : true;
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { onExternalChanged(); });
  }
}

SettingsPane::~SettingsPane() = default;

std::unique_ptr<Flex> SettingsPane::create(float scale, float panelCardOpacity) {
  m_scale = scale;
  m_panelCardOpacity = panelCardOpacity;
  m_showAdvanced = m_config != nullptr ? m_config->config().shell.settingsShowAdvanced : m_showAdvanced;
  if (m_config != nullptr) {
    const auto names = settings::barNames(m_config->config());
    if (m_selectedBarName.empty() || settings::findBar(m_config->config(), m_selectedBarName) == nullptr) {
      m_selectedBarName = names.empty() ? std::string{} : names.front();
    }
  }

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });

  auto barRow = ui::row({
      .out = &m_barSelectRow,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .visible = !m_compact && m_selectedSection == "bar",
  });
  barRow->addChild(
      ui::label({
          .text = i18n::tr(settings::settingsSectionLabelKey(settings::SettingsSection::Bar)),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  barRow->addChild(
      ui::select({
          .out = &m_barSelect,
          .options = m_config != nullptr ? settings::barNames(m_config->config()) : std::vector<std::string>{},
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .flexGrow = 1.0F,
          .onSelectionChanged =
              [this](std::size_t index, std::string_view /*label*/) {
                if (m_config == nullptr) {
                  return;
                }
                const auto names = settings::barNames(m_config->config());
                if (index >= names.size() || names[index] == m_selectedBarName) {
                  return;
                }
                m_selectedBarName = names[index];
                m_selectedMonitorOverride.clear();
                m_scrollState.offset = 0.0F;
                queueRebuild();
              },
      })
  );
  barRow->addChild(
      ui::button({
          .glyph = "plus",
          .tooltip = i18n::tr("settings.entities.bar.new"),
          .onClick =
              [this]() {
                if (m_config == nullptr) {
                  return;
                }
                std::string name = "bar";
                std::size_t n = 2;
                while (settings::findBar(m_config->config(), name) != nullptr) {
                  name = "bar_" + std::to_string(n++);
                }
                createBar(std::move(name));
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  std::vector<std::string> overrideOptions{i18n::tr("settings.entities.monitor-override.base")};
  if (m_config != nullptr) {
    if (const auto* bar = settings::findBar(m_config->config(), m_selectedBarName); bar != nullptr) {
      for (const auto& ovr : bar->monitorOverrides) {
        overrideOptions.push_back(ovr.match);
      }
    }
  }
  barRow->addChild(
      ui::select({
          .out = &m_monitorOverrideSelect,
          .options = std::move(overrideOptions),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .flexGrow = 1.0F,
          .onSelectionChanged =
              [this](std::size_t index, std::string_view /*label*/) {
                if (index == 0) {
                  m_selectedMonitorOverride.clear();
                } else if (m_config != nullptr) {
                  if (const auto* bar = settings::findBar(m_config->config(), m_selectedBarName); bar != nullptr) {
                    if (index - 1 < bar->monitorOverrides.size()) {
                      m_selectedMonitorOverride = bar->monitorOverrides[index - 1].match;
                    }
                  }
                }
                m_scrollState.offset = 0.0F;
                queueRebuild();
              },
      })
  );
  barRow->addChild(
      ui::button({
          .glyph = "device-desktop",
          .tooltip = i18n::tr("settings.entities.monitor-override.new"),
          .onClick =
              [this]() {
                if (m_config == nullptr || m_selectedBarName.empty()) {
                  return;
                }
                std::string match = "HDMI-A-1";
                if (const auto* bar = settings::findBar(m_config->config(), m_selectedBarName); bar != nullptr) {
                  std::size_t n = 1;
                  while (settings::findMonitorOverride(*bar, match) != nullptr) {
                    match = "monitor_" + std::to_string(n++);
                  }
                }
                createMonitorOverride(m_selectedBarName, std::move(match));
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  tab->addChild(std::move(barRow));
  syncBarSelect();

  tab->addChild(
      settings::makeSettingsStatusBanner({
          .message = m_statusMessage,
          .error = m_statusIsError,
          .scale = scale,
          .out = &m_statusBanner,
          .messageOut = &m_statusLabel,
      })
  );

  // Compact panes sit inside a live tab's own scroll. An inner ScrollView with
  // height 0 treats that as "use natural height" and paints over the host.
  if (m_compact) {
    auto content = ui::column({
        .out = &m_content,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceMd * scale,
        .flexGrow = 0.0F,
    });
    rebuildContent();
    tab->addChild(std::move(content));
    m_scroll = nullptr;
    return tab;
  }

  auto scroll = ui::scrollView({
      .out = &m_scroll,
      .state = &m_scrollState,
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  m_content = scroll->content();
  rebuildContent();
  tab->addChild(std::move(scroll));
  return tab;
}

std::unique_ptr<Flex> SettingsPane::createHeaderActions(float scale) {
  auto row = ui::row({
      .out = &m_editorHeaderActions,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });
  row->addChild(
      ui::label({
          .text = i18n::tr("settings.badges.advanced"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  row->addChild(
      ui::toggle({
          .out = &m_advancedToggle,
          .checked = m_showAdvanced,
          .scale = scale,
          .onChange =
              [this](bool value) {
                if (m_config != nullptr && !m_config->setOverride({"shell", "settings_show_advanced"}, value)) {
                  m_statusMessage = i18n::tr("settings.errors.write");
                  m_statusIsError = true;
                  if (m_statusBanner != nullptr && m_statusLabel != nullptr) {
                    settings::updateSettingsStatusBanner(*m_statusBanner, *m_statusLabel, m_statusMessage, true);
                  }
                  return;
                }
                m_showAdvanced = value;
                m_pendingResetSettingPaths.clear();
                queueRebuild();
              },
      })
  );
  row->addChild(
      ui::button({
          .out = &m_actionsMenuButton,
          .glyph = "more-vertical",
          .tooltip = i18n::tr("settings.window.export-config"),
          .onClick = [this]() { openActionsMenu(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  return row;
}

std::unique_ptr<Flex> SettingsPane::wrapLive(std::unique_ptr<Flex> live, float scale, float panelCardOpacity) {
  m_scale = scale;
  m_panelCardOpacity = panelCardOpacity;
  auto host = ui::column({
      .out = &m_embedHost,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
  });
  if (live != nullptr) {
    live->setFlexGrow(1.0F);
    m_liveRoot = live.get();
    host->addChild(std::move(live));
  }
  if (m_configureMode) {
    ensureEmbeddedEditor();
  }
  applyConfigureVisibility();
  return host;
}

void SettingsPane::ensureEmbeddedEditor() {
  if (m_rootLayout != nullptr || m_embedHost == nullptr) {
    return;
  }
  auto editor = create(m_scale, m_panelCardOpacity);
  editor->setFlexGrow(1.0F);
  m_embedHost->addChild(std::move(editor));
}

std::unique_ptr<Flex> SettingsPane::embedHeader(std::unique_ptr<Flex> liveActions, float scale) {
  auto row = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });
  if (liveActions != nullptr) {
    m_liveHeaderActions = liveActions.get();
    row->addChild(std::move(liveActions));
  }
  row->addChild(
      ui::button({
          .out = &m_configureButton,
          .glyph = "settings",
          .selected = m_configureMode,
          .tooltip = i18n::tr("control-center.settings.configure"),
          .onClick =
              [this]() {
                setConfigureMode(!m_configureMode);
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  auto editorActions = createHeaderActions(scale);
  row->addChild(std::move(editorActions));
  applyConfigureVisibility();
  return row;
}

void SettingsPane::onClose() {
  m_embedHost = nullptr;
  m_liveRoot = nullptr;
  m_liveHeaderActions = nullptr;
  m_editorHeaderActions = nullptr;
  m_configureButton = nullptr;
  m_rootLayout = nullptr;
  m_barSelect = nullptr;
  m_barSelectRow = nullptr;
  m_statusLabel = nullptr;
  m_statusBanner = nullptr;
  m_scroll = nullptr;
  m_content = nullptr;
  m_advancedToggle = nullptr;
  m_actionsMenuButton = nullptr;
  m_layoutRenderer = nullptr;
  m_rebuildQueued = false;
  if (m_actionsMenuPopup != nullptr && m_actionsMenuPopup->isOpen()) {
    m_actionsMenuPopup->close();
  }
}

void SettingsPane::layout(Renderer& renderer, float contentWidth, float bodyHeight) {
  m_layoutRenderer = &renderer;
  if (m_embedHost != nullptr) {
    m_embedHost->setSize(contentWidth, bodyHeight);
    m_embedHost->layout(renderer);
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

bool SettingsPane::layoutEmbedded(Renderer& renderer, float contentWidth, float bodyHeight) {
  m_layoutRenderer = &renderer;
  if (m_embedHost == nullptr) {
    return false;
  }
  m_embedHost->setSize(contentWidth, bodyHeight);
  m_embedHost->layout(renderer);
  return m_configureMode;
}

void SettingsPane::setConfigureMode(bool show) {
  if (show) {
    ensureEmbeddedEditor();
  }
  if (m_configureMode == show) {
    applyConfigureVisibility();
    return;
  }
  m_configureMode = show;
  applyConfigureVisibility();
  PanelManager::instance().requestLayout();
}

void SettingsPane::setSearchQuery(std::string query) {
  if (m_searchQuery == query) {
    return;
  }
  m_searchQuery = std::move(query);
  m_scrollState.offset = 0.0F;
  if (m_embedHost != nullptr && !m_searchQuery.empty()) {
    ensureEmbeddedEditor();
  }
  syncBarSelect();
  queueRebuild();
}

void SettingsPane::setSection(std::string section) {
  if (m_selectedSection == section) {
    return;
  }
  m_selectedSection = std::move(section);
  m_scrollState.offset = 0.0F;
  queueRebuild();
}

void SettingsPane::applyConfigureVisibility() {
  if (m_embedHost == nullptr) {
    if (m_rootLayout != nullptr) {
      m_rootLayout->setVisible(true);
      m_rootLayout->setParticipatesInLayout(true);
    }
    if (m_editorHeaderActions != nullptr) {
      m_editorHeaderActions->setVisible(true);
    }
    return;
  }
  if (m_liveRoot != nullptr) {
    m_liveRoot->setVisible(!m_configureMode);
    m_liveRoot->setParticipatesInLayout(!m_configureMode);
  }
  if (m_rootLayout != nullptr) {
    m_rootLayout->setVisible(m_configureMode);
    m_rootLayout->setParticipatesInLayout(m_configureMode);
  }
  if (m_liveHeaderActions != nullptr) {
    m_liveHeaderActions->setVisible(!m_configureMode);
  }
  if (m_editorHeaderActions != nullptr) {
    m_editorHeaderActions->setVisible(m_configureMode);
  }
  if (m_configureButton != nullptr) {
    m_configureButton->setSelected(m_configureMode);
  }
}

void SettingsPane::queueRebuild() {
  if (m_rebuildQueued) {
    return;
  }
  m_rebuildQueued = true;
  DeferredCall::callLater([this]() {
    m_rebuildQueued = false;
    if (m_content == nullptr) {
      return;
    }
    rebuildContent();
    PanelManager::instance().requestRedraw();
  });
}

void SettingsPane::rebuildContent() {
  uiAssertNotRendering("SettingsPane::rebuildContent");
  if (m_content == nullptr) {
    return;
  }
  while (!m_content->children().empty()) {
    m_content->removeChild(m_content->children().back().get());
  }
  m_content->setDirection(FlexDirection::Vertical);
  m_content->setAlign(FlexAlign::Stretch);
  m_content->setGap(Style::spaceMd * m_scale);
  fillContent(*m_content);
  if (m_statusBanner != nullptr && m_statusLabel != nullptr) {
    settings::updateSettingsStatusBanner(*m_statusBanner, *m_statusLabel, m_statusMessage, m_statusIsError);
  }
  syncBarSelect();
}

void SettingsPane::fillContent(Flex& content) {
  if (m_config == nullptr) {
    return;
  }
  const Config& cfg = m_config->config();
  const BarConfig* selectedBar = settings::findBar(cfg, m_selectedBarName);
  if (selectedBar == nullptr) {
    const auto names = settings::barNames(cfg);
    m_selectedBarName = names.empty() ? std::string{} : names.front();
    selectedBar = settings::findBar(cfg, m_selectedBarName);
  }
  auto env = buildRegistryEnvironment();
  env.copyToClipboard = [this](std::string text) {
    if (m_clipboard != nullptr) {
      (void)m_clipboard->copyText(std::move(text));
    }
  };
  env.requestRebuild = [this]() { queueRebuild(); };
  auto registry = settings::buildSettingsRegistry(cfg, selectedBar, selectedMonitorOverride(selectedBar), env);
  settings::enrichSettingsRegistry(registry, cfg, env, makeRuntimeActions());
  const auto* monitorOverride = selectedMonitorOverride(selectedBar);
  (void)settings::addSettingsContentSections(content, registry, makeContentContext(cfg, selectedBar, monitorOverride));

  if (m_selectedSection == "bar" && m_searchQuery.empty()) {
    settings::addSettingsBarManagement(
        content,
        settings::SettingsBarManagementContext{
            .config = cfg,
            .configService = m_config,
            .scale = m_scale,
            .searchQuery = m_searchQuery,
            .selectedSection = m_selectedSection,
            .selectedBar = selectedBar,
            .selectedMonitorOverride = monitorOverride,
            .renamingBarName = m_renamingBarName,
            .pendingDeleteBarName = m_pendingDeleteBarName,
            .renamingMonitorOverrideBarName = m_renamingMonitorOverrideBarName,
            .renamingMonitorOverrideMatch = m_renamingMonitorOverrideMatch,
            .pendingDeleteMonitorOverrideBarName = m_pendingDeleteMonitorOverrideBarName,
            .pendingDeleteMonitorOverrideMatch = m_pendingDeleteMonitorOverrideMatch,
            .requestRebuild = [this]() { queueRebuild(); },
            .renameBar = [this](std::string oldName, std::string newName) { renameBar(std::move(oldName), std::move(newName)); },
            .deleteBar = [this](std::string name) { deleteBar(std::move(name)); },
            .moveBar = [this](std::string name, int direction) { moveBar(std::move(name), direction); },
            .renameMonitorOverride =
                [this](std::string barName, std::string oldMatch, std::string newMatch) {
                  renameMonitorOverride(std::move(barName), std::move(oldMatch), std::move(newMatch));
                },
            .deleteMonitorOverride =
                [this](std::string barName, std::string match) {
                  deleteMonitorOverride(std::move(barName), std::move(match));
                },
        }
    );
  }

  if (m_selectedSection != "plugins") {
    return;
  }

  refreshPluginListIfNeeded();
  settings::addSettingsPlugins(
      content,
      settings::SettingsPluginsContext{
          .scale = m_scale,
          .selectedSection = m_selectedSection,
          .includedGroups = (!m_searchQuery.empty() && !m_compact) ? std::unordered_set<std::string>{}
                                                                    : m_includedGroups,
          .plugins = m_pluginList,
          .sources = cfg.plugins.sources,
          .searchActive = !m_searchQuery.empty(),
          .pageTitleRow = nullptr,
          .groupJumpRow = nullptr,
          .scrollContentToTop =
              [this](const Node& target) {
                if (m_scroll != nullptr) {
                  scrollNodeToScrollViewTop(*m_scroll, target, Style::spaceMd * m_scale);
                }
              },
          .expandedGroupsByPage = m_expandedGroupsByPage,
          .pluginsLoading = m_pluginListDirty || m_pluginListRefreshInFlight,
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
                markPluginListDirty();
                queueRebuild();
              },
          .isEnabling =
              [this](const std::string& id) { return m_pluginManager != nullptr && m_pluginManager->isEnabling(id); },
          .addSource =
              [this]() {
                bindHostedEditors();
                if (m_hostedEditors != nullptr) {
                  m_hostedEditors->openPluginSource();
                }
              },
          .setSourceEnabled =
              [this](PluginSourceConfig source, bool enabled) {
                if (m_pluginManager == nullptr) {
                  return;
                }
                source.enabled = enabled;
                m_pluginManager->addSource(source);
                markPluginListDirty();
                queueRebuild();
              },
          .editSource =
              [this](PluginSourceConfig source) {
                bindHostedEditors();
                if (m_hostedEditors != nullptr) {
                  m_hostedEditors->openPluginSource(std::move(source));
                }
              },
          .updateSource =
              [this](std::string source) {
                if (m_pluginManager != nullptr) {
                  m_pluginManager->update(std::move(source));
                }
              },
          .refresh =
              [this]() {
                markPluginListDirty();
                queueRebuild();
              },
          .autoUpdateMode = cfg.plugins.autoUpdate,
          .setAutoUpdate =
              [this](PluginAutoUpdateMode mode) {
                if (m_pluginManager == nullptr) {
                  return;
                }
                m_pluginManager->setAutoUpdateMode(mode);
                markPluginListDirty();
                queueRebuild();
              },
          .updateAll =
              [this]() {
                if (m_pluginManager != nullptr) {
                  m_pluginManager->updateAutoUpdateScope(PluginAutoUpdateMode::All);
                }
              },
          .config = &cfg,
          .onConfigure = [this](std::string id) { openPluginSettings(std::move(id)); },
          .onRemove =
              [this](std::string id) {
                if (m_pluginManager != nullptr) {
                  m_pluginManager->remove(id);
                  m_pendingDeletePluginId.clear();
                  markPluginListDirty();
                  queueRebuild();
                }
              },
          .openStore =
              m_openStore
                  ? m_openStore
                  : (m_includedGroups.empty()
                         ? std::function<void()>{
                               [this]() {
                                 bindHostedEditors();
                                 if (m_hostedEditors != nullptr) {
                                   m_hostedEditors->openPluginStore();
                                 }
                               }
                           }
                         : nullptr),
          .pendingDeletePluginId = m_pendingDeletePluginId,
          .requestDeleteConfirm =
              [this](std::string id) {
                m_pendingDeletePluginId = std::move(id);
                queueRebuild();
              },
          .cancelDelete =
              [this]() {
                m_pendingDeletePluginId.clear();
                queueRebuild();
              },
      }
  );
}

settings::RegistryEnvironment SettingsPane::buildRegistryEnvironment() const {
  settings::RegistryEnvironment env;
  WaylandConnection* wayland = m_platform != nullptr ? &m_platform->wayland() : nullptr;
  if (m_config != nullptr) {
    env.shellAvatarPath = shell::resolvedAvatarPath(m_accounts, m_config->config());
  }
  env.niriBackdropSupported = wayland != nullptr && compositors::isNiri();
  env.screencopySupported = wayland != nullptr && wayland->hasScreencopy();
  env.niriOverviewTypeToLaunchSupported = wayland != nullptr && compositors::isNiri();
  if (env.niriBackdropSupported && m_platform != nullptr) {
    const auto version = m_platform->niriRuntime().compositorVersion();
    env.niriBlurSupported = version.has_value() && compositors::niri::versionSupportsBlur(*version);
  }
  env.ddcutilAvailable = m_dependencies != nullptr && m_dependencies->hasDdcutil();
  env.systemdUserManaged = process::runningUnderSystemdUserManager();
  env.gammaControlAvailable = wayland != nullptr && wayland->hasGammaControl();
  env.greeterSyncAvailable =
      m_config != nullptr && greeter::appearanceSyncAvailable(m_config->config().shell.greeterSync);

  const ThemeMode previewMode = m_config != nullptr ? shellThemeMode(m_config->config().theme) : ThemeMode::Dark;
  for (const auto& paletteInfo : noctalia::theme::availableCommunityPalettes()) {
    env.communityPalettes.push_back(
        settings::SelectOption{
            .value = paletteInfo.name,
            .label = paletteInfo.name,
            .preview = availablePalettePreview(paletteInfo, previewMode),
        }
    );
  }
  for (const auto& customPalette : noctalia::theme::availableCustomPalettes()) {
    env.customPalettes.push_back(
        settings::SelectOption{
            .value = customPalette.name,
            .label = customPalette.name,
            .preview = availablePalettePreview(customPalette, previewMode),
        }
    );
  }
  for (const auto& tmpl : noctalia::theme::CommunityTemplateService::availableTemplates()) {
    env.communityTemplates.push_back(
        settings::SelectOption{
            .value = tmpl.id,
            .label = tmpl.displayName,
            .description = tmpl.category,
            .tooltip = noctalia::theme::formatTemplateTooltip(tmpl),
        }
    );
  }
  static const std::vector<settings::SelectOption> kFontFamilies = []() {
    std::vector<settings::SelectOption> options;
    for (const std::string& family : settings::discoverFontFamilies()) {
      options.push_back(settings::SelectOption{family, family});
    }
    return options;
  }();
  env.fontFamilies = kFontFamilies;

  const auto allBatteryDeviceOptions = batteryDeviceOptions();
  const auto* systemBattery = m_upower != nullptr ? m_upower->defaultSystemBattery() : nullptr;
  env.systemBatteryAvailable = systemBattery != nullptr;
  for (const auto& option : allBatteryDeviceOptions) {
    if (option.value == "auto" || (systemBattery != nullptr && option.value == systemBattery->path)) {
      continue;
    }
    env.batteryDeviceOptions.push_back(option);
  }
  env.batteryAvailable = env.systemBatteryAvailable || !env.batteryDeviceOptions.empty();
  if (env.batteryAvailable && m_config != nullptr) {
    for (const auto& option : env.batteryDeviceOptions) {
      env.batteryWarningThresholds.emplace(
          option.value, batteryWarningThresholdForSelector(m_config->config().battery, m_upower, option.value)
      );
    }
  }
  env.keyboardLayoutNames = wayland != nullptr ? wayland->keyboardLayoutNames() : std::vector<std::string>{};
  if (wayland != nullptr) {
    for (const auto& output : wayland->outputs()) {
      if (output.output == nullptr || output.connectorName.empty()) {
        continue;
      }
      std::string label = output.connectorName;
      if (!output.description.empty()) {
        label += " (" + output.description + ")";
      }
      env.availableOutputs.push_back(settings::SelectOption{output.connectorName, std::move(label)});
    }
  }
  return env;
}

std::vector<settings::SelectOption> SettingsPane::batteryDeviceOptions() const {
  std::vector<settings::SelectOption> options;
  options.push_back(settings::SelectOption{.value = "auto", .label = i18n::tr("common.states.auto")});
  if (m_upower == nullptr) {
    return options;
  }
  const auto devices = m_upower->batteryDevices();
  options.reserve(devices.size() + 1);
  for (const auto& device : devices) {
    std::string description = device.path;
    if (!device.nativePath.empty() && device.nativePath != device.path) {
      description = device.nativePath + " - " + device.path;
    }
    options.push_back(
        settings::SelectOption{
            .value = device.path,
            .label = upowerDeviceLabel(device),
            .description = std::move(description),
        }
    );
  }
  return options;
}

std::vector<settings::GestureActionOption> SettingsPane::gestureActionCatalog() const {
  if (m_ipc == nullptr) {
    return {};
  }
  std::vector<settings::GestureActionOption> options;
  for (const auto& handler : m_ipc->handlers()) {
    if (handler.command == noctalia::bar::kExecVerb || handler.command == noctalia::bar::kNoneVerb) {
      continue;
    }
    if (handler.actionEditorVisibility == IpcService::ActionEditorVisibility::Hidden) {
      continue;
    }
    options.push_back(
        settings::GestureActionOption{
            .option =
                settings::SelectOption{
                    .value = std::string(handler.command),
                    .label = translatedIpcActionField(handler.command, "label"),
                    .description = translatedIpcActionField(handler.command, "description"),
                },
            .argsSpec = std::string(handler.args),
        }
    );
  }
  return options;
}

settings::SettingsContentContext SettingsPane::makeContentContext(
    const Config& cfg, const BarConfig* selectedBar, const BarMonitorOverride* selectedMonitorOverride
) {
  const bool searching = !m_searchQuery.empty();
  return settings::SettingsContentContext{
      .config = cfg,
      .configService = m_config,
      .scale = m_scale,
      .searchQuery = m_searchQuery,
      .selectedSection = searching ? std::string_view{} : m_selectedSection,
      .includedGroups = (searching && !m_compact) ? std::unordered_set<std::string>{} : m_includedGroups,
      .selectedBar = selectedBar,
      .selectedMonitorOverride = selectedMonitorOverride,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = false,
      .batteryDeviceOptions = batteryDeviceOptions(),
      .editingWidgetName = m_editingWidgetName,
      .editingCapsuleGroupId = m_editingCapsuleGroupId,
      .selectedLaneWidgets = m_selectedLaneWidgets,
      .pendingDeleteWidgetName = m_pendingDeleteWidgetName,
      .pendingDeleteWidgetSettingPath = m_pendingDeleteWidgetSettingPath,
      .renamingWidgetName = m_renamingWidgetName,
      .pendingGestureKey = m_pendingGestureKey,
      .pendingGestureVerb = m_pendingGestureVerb,
      .actionsExpandedFor = m_actionsExpandedFor,
      .expandedGroupsByPage = m_expandedGroupsByPage,
      .pageTitleRow = nullptr,
      .groupJumpRow = nullptr,
      .actionCatalog = gestureActionCatalog(),
      .requestRebuild = [this]() { queueRebuild(); },
      .requestContentRebuild = [this]() { queueRebuild(); },
      .resetContentScroll = [this]() { m_scrollState.offset = 0.0F; },
      .setScrollTarget = {},
      .scrollContentToTop =
          [this](const Node& target) {
            if (m_scroll != nullptr) {
              scrollNodeToScrollViewTop(*m_scroll, target, Style::spaceMd * m_scale);
            }
          },
      .focusArea = [](InputArea* area) { PanelManager::instance().inputDispatcher().setFocus(area); },
      .openBarWidgetAddPopup =
          [this](const std::vector<std::string>& lanePath) {
            bindHostedEditors();
            if (m_hostedEditors != nullptr) {
              m_hostedEditors->openBarWidgetAdd(lanePath);
            }
          },
      .openSearchPickerPopup =
          [this](settings::SearchPickerOpenRequest request) {
            bindHostedEditors();
            if (m_hostedEditors != nullptr) {
              m_hostedEditors->openSearchPicker(std::move(request));
            }
          },
      .setOverride =
          [this](std::vector<std::string> path, ConfigOverrideValue value) {
            setSettingOverride(std::move(path), std::move(value));
          },
      .setOverrides =
          [this](std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides) {
            setSettingOverrides(std::move(overrides));
          },
      .clearOverride = [this](std::vector<std::string> path) { clearSettingOverride(std::move(path)); },
      .clearOverrides = [this](std::vector<std::vector<std::string>> paths) { clearSettingOverrides(std::move(paths)); },
      .resetBarLane = [this](std::vector<std::string> lanePath) { resetBarLane(std::move(lanePath)); },
      .isResetConfirmationPending =
          [this](const std::vector<std::vector<std::string>>& paths) { return m_pendingResetSettingPaths == paths; },
      .requestResetConfirmation =
          [this](std::vector<std::vector<std::string>> paths) {
            m_pendingResetSettingPaths = std::move(paths);
            queueRebuild();
          },
      .renameWidgetInstance =
          [this](
              std::string oldName, std::string newName,
              std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides
          ) { renameWidgetInstance(std::move(oldName), std::move(newName), std::move(overrides)); },
      .openSessionActionEntryEditor =
          [this](std::size_t index) {
            bindHostedEditors();
            if (m_hostedEditors != nullptr) {
              m_hostedEditors->openSessionActionEntry(index);
            }
          },
      .openIdleBehaviorEntryEditor =
          [this](std::size_t index) {
            bindHostedEditors();
            if (m_hostedEditors != nullptr) {
              m_hostedEditors->openIdleBehaviorEntry(index);
            }
          },
      .openIdleBehaviorCreateEditor =
          [this]() {
            bindHostedEditors();
            if (m_hostedEditors != nullptr) {
              m_hostedEditors->openIdleBehaviorCreate();
            }
          },
      .openNotificationFilterEntryEditor =
          [this](std::size_t index) {
            bindHostedEditors();
            if (m_hostedEditors != nullptr) {
              m_hostedEditors->openNotificationFilterEntry(index);
            }
          },
      .openNotificationFilterCreateEditor =
          [this]() {
            bindHostedEditors();
            if (m_hostedEditors != nullptr) {
              m_hostedEditors->openNotificationFilterCreate();
            }
          },
      .openWidgetInspectorEditor =
          [this](std::vector<std::string> lanePath, std::string widgetName) {
            bindHostedEditors();
            if (m_hostedEditors != nullptr) {
              m_hostedEditors->openWidgetInspector(std::move(lanePath), std::move(widgetName));
            }
          },
      .openCapsuleGroupEditor =
          [this](std::vector<std::string> lanePath, std::string groupId) {
            bindHostedEditors();
            if (m_hostedEditors != nullptr) {
              m_hostedEditors->openCapsuleGroup(std::move(lanePath), std::move(groupId));
            }
          },
      .registerIdleLiveStatusLabel = {},
      .registerSessionActionSummaryLabel = {},
      .bindSessionActionsEditState = {},
      .afterSessionActionsCommit = {},
      .afterIdleBehaviorApply = {},
      .afterNotificationFilterApply = {},
      .closeHostedEditor = {},
      .supportsTaskbarWorkspaceGrouping = m_platform != nullptr && m_platform->supportsTaskbarWorkspaceGrouping(),
  };
}

void SettingsPane::persistNiriBlurInclude() {
  if (m_config == nullptr) {
    return;
  }
  const auto result = compositors::niri::saveNiriBlur(niriBlurSettingsFrom(m_config->config().theme.niriLayout));
  if (result.ok) {
    return;
  }
  m_statusMessage = result.error.empty() ? i18n::tr("control-center.theme.niri-save-failed") : result.error;
  m_statusIsError = true;
}

void SettingsPane::persistNiriOverviewInclude() {
  if (m_config == nullptr) {
    return;
  }
  const auto preset = m_config->config().theme.niriLayout.overview;
  if (preset != NiriOverviewPreset::None) {
    const bool wantBackdrop = compositors::niri::overviewWantsBackdrop(preset);
    if (m_config->config().backdrop.enabled != wantBackdrop) {
      (void)m_config->setOverride({"backdrop", "enabled"}, wantBackdrop);
    }
  }
  const auto color = colorForRole(ColorRole::Surface);
  const auto channel = [](float v) { return static_cast<int>(std::clamp(v, 0.0F, 1.0F) * 255.0F); };
  const std::string hex =
      std::format("#{:02x}{:02x}{:02x}", channel(color.r), channel(color.g), channel(color.b));
  const auto result = compositors::niri::saveNiriOverview({.preset = preset, .flatColorHex = hex});
  if (result.ok) {
    return;
  }
  m_statusMessage = result.error.empty() ? i18n::tr("control-center.theme.niri-save-failed") : result.error;
  m_statusIsError = true;
}

void SettingsPane::persistNiriLayoutInclude() {
  if (m_config == nullptr) {
    return;
  }
  const auto hex = [](const Color& color) {
    const auto channel = [](float v) { return static_cast<int>(std::clamp(v, 0.0F, 1.0F) * 255.0F); };
    return std::format("#{:02x}{:02x}{:02x}", channel(color.r), channel(color.g), channel(color.b));
  };
  const auto& niriLayout = m_config->config().theme.niriLayout;
  const auto result = compositors::niri::saveNiriLayoutTheme({
      .focusRing = niriLayout.focusRing,
      .border = niriLayout.border,
      .activeColorHex = hex(colorForRole(ColorRole::Primary)),
      .inactiveColorHex = hex(colorForRole(ColorRole::Outline)),
      .urgentColorHex = hex(colorForRole(ColorRole::Error)),
      .tabInactiveColorHex = hex(colorForRole(ColorRole::Secondary)),
      .insertHintColorHex = hex(colorForRole(ColorRole::Primary)) + "80",
      .gaps = niriLayout.gaps,
      .struts = niriLayout.struts,
      .defaultColumnWidth = niriLayout.defaultColumnWidth,
  });
  if (result.ok) {
    return;
  }
  m_statusMessage = result.error.empty() ? i18n::tr("control-center.theme.niri-save-failed") : result.error;
  m_statusIsError = true;
}

void SettingsPane::persistNiriWindowRulesInclude() {
  if (m_config == nullptr) {
    return;
  }
  const auto& niriLayout = m_config->config().theme.niriLayout;
  const auto result = compositors::niri::saveWindowRules({
      .cornerRadius = compositors::niri::currentWindowCornerRadius(),
      .drawBorderWithBackground = niriLayout.drawBorderWithBackground,
  });
  if (result.ok) {
    return;
  }
  m_statusMessage = result.error.empty() ? i18n::tr("control-center.theme.niri-save-failed") : result.error;
  m_statusIsError = true;
}

void SettingsPane::setSettingOverride(std::vector<std::string> path, ConfigOverrideValue value) {
  if (path.size() == 2 && path[0] == "shell" && path[1] == "font_family") {
    text::invalidateFontWeightCatalogCache();
  }
  const bool isAvatarPath = path.size() == 2 && path[0] == "shell" && path[1] == "avatar_path";
  DeferredCall::callLater([this, path = std::move(path), value = std::move(value), isAvatarPath]() mutable {
    if (m_config == nullptr || m_content == nullptr) {
      return;
    }
    if (isAvatarPath) {
      const auto* avatarPath = std::get_if<std::string>(&value);
      if (avatarPath == nullptr) {
        m_statusMessage = i18n::tr("settings.errors.write");
        m_statusIsError = true;
        queueRebuild();
        return;
      }
      const auto result = shell::applyAvatarPath(m_accounts, m_config, *avatarPath);
      if (result.success()) {
        m_statusMessage.clear();
        m_statusIsError = false;
        m_pendingResetSettingPaths.clear();
        queueRebuild();
        return;
      }
      m_statusMessage = i18n::tr(shell::avatarApplyErrorTranslationKey(result.error));
      m_statusIsError = true;
      queueRebuild();
      return;
    }
    if (!m_config->setOverride(path, std::move(value))) {
      m_statusMessage = settingsMutationError(*m_config, i18n::tr("settings.errors.write"));
      m_statusIsError = true;
      queueRebuild();
      return;
    }
    m_statusMessage.clear();
    m_statusIsError = false;
    m_pendingResetSettingPaths.clear();
    if (isNiriBlurPersistPath(path)) {
      persistNiriBlurInclude();
    }
    if (path.size() == 3 && path[0] == "theme" && path[1] == "niri_layout" && path[2] == "overview") {
      persistNiriOverviewInclude();
    }
    if (isNiriLayoutPersistPath(path)) {
      persistNiriLayoutInclude();
    }
    if (isNiriWindowRulesPersistPath(path)) {
      persistNiriWindowRulesInclude();
    }
    if (isCustomSchedulePath(path)) {
      const LocationConfig& location = m_config->config().location;
      if (location.customSchedule && !day_night_schedule::hasUsableCustomTimes(location)) {
        m_statusMessage = i18n::tr("settings.errors.custom-schedule-times");
        m_statusIsError = true;
      }
    }
    queueRebuild();
  });
}

void SettingsPane::setSettingOverrides(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides) {
  DeferredCall::callLater([this, overrides = std::move(overrides)]() mutable {
    if (m_config == nullptr || m_content == nullptr) {
      return;
    }
    if (overrides.empty()) {
      m_statusMessage.clear();
      m_statusIsError = false;
      queueRebuild();
      return;
    }
    bool wroteBlur = false;
    bool wroteOverview = false;
    bool wroteLayout = false;
    bool wroteWindowRules = false;
    for (const auto& [path, _] : overrides) {
      if (isNiriBlurPersistPath(path)) {
        wroteBlur = true;
      }
      if (path.size() == 3 && path[0] == "theme" && path[1] == "niri_layout" && path[2] == "overview") {
        wroteOverview = true;
      }
      if (isNiriLayoutPersistPath(path)) {
        wroteLayout = true;
      }
      if (isNiriWindowRulesPersistPath(path)) {
        wroteWindowRules = true;
      }
    }
    if (!m_config->setOverrides(std::move(overrides))) {
      m_statusMessage = settingsMutationError(*m_config, i18n::tr("settings.errors.batch-write"));
      m_statusIsError = true;
      queueRebuild();
      return;
    }
    m_statusMessage.clear();
    m_statusIsError = false;
    m_pendingResetSettingPaths.clear();
    if (wroteBlur) {
      persistNiriBlurInclude();
    }
    if (wroteOverview) {
      persistNiriOverviewInclude();
    }
    if (wroteLayout) {
      persistNiriLayoutInclude();
    }
    if (wroteWindowRules) {
      persistNiriWindowRulesInclude();
    }
    queueRebuild();
  });
}

void SettingsPane::clearSettingOverride(std::vector<std::string> path) {
  clearSettingOverrides({std::move(path)});
}

void SettingsPane::clearSettingOverrides(std::vector<std::vector<std::string>> paths) {
  DeferredCall::callLater([this, paths = std::move(paths)]() mutable {
    if (m_config == nullptr || m_content == nullptr) {
      return;
    }
    bool changed = false;
    if (!m_config->clearOverrides(paths, &changed)) {
      m_statusMessage = i18n::tr("settings.errors.write");
      m_statusIsError = true;
      queueRebuild();
      return;
    }
    m_statusMessage.clear();
    m_statusIsError = false;
    m_pendingResetSettingPaths.clear();
    queueRebuild();
  });
}

void SettingsPane::bindHostedEditors() {
  if (m_hostedEditors == nullptr || m_config == nullptr) {
    return;
  }
  m_hostedEditors->bind(
      settings::HostedEditorBindings{
          .config = m_config,
          .pluginManager = m_pluginManager,
          .asyncTextures = m_asyncTextures,
          .scale = [this]() { return m_scale; },
          .renderer = [this]() -> Renderer* { return m_layoutRenderer; },
          .makeContext =
              [this]() {
                const Config& cfg = m_config->config();
                const BarConfig* selectedBar = settings::findBar(cfg, m_selectedBarName);
                return makeContentContext(cfg, selectedBar, selectedMonitorOverride(selectedBar));
              },
          .requestRebuild = [this]() { queueRebuild(); },
          .setOverride =
              [this](std::vector<std::string> path, ConfigOverrideValue value) {
                setSettingOverride(std::move(path), std::move(value));
              },
          .setOverrides =
              [this](std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides) {
                setSettingOverrides(std::move(overrides));
              },
          .renameWidget =
              [this](
                  std::string oldName, std::string newName,
                  std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides
              ) { renameWidgetInstance(std::move(oldName), std::move(newName), std::move(overrides)); },
          .onPluginToggled =
              [this](const std::string&, bool, const scripting::CatalogEntry*) {
                markPluginListDirty();
                queueRebuild();
              },
          .setHostStatus =
              [this](std::string message, bool error) {
                m_statusMessage = std::move(message);
                m_statusIsError = error;
                queueRebuild();
              },
          .showAdvanced = [this]() { return m_showAdvanced; },
          .requestActivationToken =
              [this]() -> std::string {
                if (m_platform == nullptr) {
                  return {};
                }
                auto parent = PanelManager::instance().fallbackPopupParentContext();
                if (!parent.has_value() || parent->surface == nullptr) {
                  return {};
                }
                return m_platform->wayland().requestActivationToken(parent->surface);
              },
          .calendar = m_calendar,
          .editingWidgetName = &m_editingWidgetName,
          .editingCapsuleGroupId = &m_editingCapsuleGroupId,
          .renamingWidgetName = &m_renamingWidgetName,
          .pendingDeleteWidgetName = &m_pendingDeleteWidgetName,
          .pendingDeleteWidgetSettingPath = &m_pendingDeleteWidgetSettingPath,
          .selectedLaneWidgets = &m_selectedLaneWidgets,
          .pendingGestureKey = &m_pendingGestureKey,
          .pendingGestureVerb = &m_pendingGestureVerb,
          .actionsExpandedFor = &m_actionsExpandedFor,
      }
  );
}

void SettingsPane::syncBarSelect() {
  if (m_config == nullptr) {
    return;
  }
  const auto names = settings::barNames(m_config->config());
  if (m_barSelect != nullptr) {
    m_barSelect->setOptions(names);
    const auto it = std::ranges::find(names, m_selectedBarName);
    if (it != names.end()) {
      m_barSelect->setSelectedIndexSilently(static_cast<std::size_t>(it - names.begin()));
    }
  }
  if (m_monitorOverrideSelect != nullptr) {
    std::vector<std::string> overrideOptions{i18n::tr("settings.entities.monitor-override.base")};
    const auto* bar = settings::findBar(m_config->config(), m_selectedBarName);
    if (bar != nullptr) {
      for (const auto& ovr : bar->monitorOverrides) {
        overrideOptions.push_back(ovr.match);
      }
    }
    m_monitorOverrideSelect->setOptions(overrideOptions);
    std::size_t selected = 0;
    if (bar != nullptr && !m_selectedMonitorOverride.empty()) {
      for (std::size_t i = 0; i < bar->monitorOverrides.size(); ++i) {
        if (bar->monitorOverrides[i].match == m_selectedMonitorOverride) {
          selected = i + 1;
          break;
        }
      }
    }
    m_monitorOverrideSelect->setSelectedIndexSilently(selected);
  }
  if (m_barSelectRow != nullptr) {
    m_barSelectRow->setVisible(!m_compact && m_selectedSection == "bar" && m_searchQuery.empty());
  }
}

void SettingsPane::onExternalChanged() {
  markPluginListDirty();
  queueRebuild();
}

void SettingsPane::openSearchPicker(settings::SearchPickerOpenRequest request) {
  bindHostedEditors();
  if (m_hostedEditors != nullptr) {
    m_hostedEditors->openSearchPicker(std::move(request));
  }
}

void SettingsPane::openUserTemplateEditor(std::string id) {
  bindHostedEditors();
  if (m_hostedEditors != nullptr) {
    m_hostedEditors->openUserTemplateEditor(std::move(id));
  }
}

void SettingsPane::setIncludedGroups(std::unordered_set<std::string> groups) {
  if (m_includedGroups == groups) {
    return;
  }
  m_includedGroups = std::move(groups);
  queueRebuild();
}

void SettingsPane::openPluginSettings(std::string pluginId) {
  bindHostedEditors();
  if (m_hostedEditors != nullptr) {
    m_hostedEditors->openPluginSettings(std::move(pluginId));
  }
}

void SettingsPane::openWidgetInspector(std::string barName, std::string widgetName) {
  if (m_config == nullptr) {
    return;
  }
  m_selectedBarName = std::move(barName);
  m_selectedMonitorOverride.clear();
  bindHostedEditors();
  if (m_hostedEditors != nullptr) {
    m_hostedEditors->openWidgetInspector(
        settings::barWidgetLanePath(m_config->config(), m_selectedBarName, widgetName), std::move(widgetName)
    );
  }
  queueRebuild();
}

void SettingsPane::setSelectedBarName(std::string name) {
  m_selectedBarName = std::move(name);
  m_selectedMonitorOverride.clear();
  queueRebuild();
}

const BarMonitorOverride* SettingsPane::selectedMonitorOverride(const BarConfig* selectedBar) const {
  if (selectedBar == nullptr || m_selectedMonitorOverride.empty()) {
    return nullptr;
  }
  return settings::findMonitorOverride(*selectedBar, m_selectedMonitorOverride);
}

settings::RuntimeActions SettingsPane::makeRuntimeActions() {
  settings::RuntimeActions actions = m_actions;
  actions.calendar = m_calendar;
  actions.clipboard = m_clipboard;
  actions.pendingEncryptedStorageReset = &m_pendingEncryptedStorageReset;
  actions.requestRebuild = [this]() { queueRebuild(); };
  actions.openCommunityTemplateStore = [this]() {
    bindHostedEditors();
    if (m_hostedEditors != nullptr) {
      m_hostedEditors->openCommunityTemplateStore();
    }
  };
  if (m_calendar != nullptr) {
    actions.retryCalendarCredentials = [this]() {
      if (m_calendar != nullptr) {
        m_calendar->retryCredentialMigration();
      }
    };
    actions.retryCalendarStorage = [this]() {
      if (m_calendar != nullptr) {
        m_calendar->retryCachePersistence();
      }
    };
  }
  if (m_clipboard != nullptr) {
    actions.retryClipboardPersistence = [this]() { m_clipboard->retryPersistence(); };
  }
  actions.openCalendarAccount = [this](std::optional<std::string> accountId) { openCalendarAccount(std::move(accountId)); };
  return actions;
}

void SettingsPane::openCalendarAccount(std::optional<std::string> accountId) {
  bindHostedEditors();
  if (m_hostedEditors != nullptr) {
    m_hostedEditors->openCalendarAccount(std::move(accountId));
  }
}

void SettingsPane::markPluginListDirty() {
  m_pluginListDirty = true;
  ++m_pluginListRefreshGeneration;
}

void SettingsPane::refreshPluginListIfNeeded() {
  if (m_pluginManager == nullptr || m_config == nullptr || !m_pluginListDirty || m_pluginListRefreshInFlight) {
    return;
  }
  m_pluginListRefreshInFlight = true;
  const std::uint64_t generation = m_pluginListRefreshGeneration;
  auto* manager = m_pluginManager;
  PluginsConfig pluginsSnapshot = m_config->config().plugins;
  std::thread([this, manager, generation, pluginsSnapshot = std::move(pluginsSnapshot)]() {
    manager->fetchStaleCatalogs(pluginsSnapshot);
    auto plugins = manager->list(pluginsSnapshot, scripting::CatalogAccess::Network);
    DeferredCall::callLater([this, generation, plugins = std::move(plugins)]() mutable {
      m_pluginListRefreshInFlight = false;
      if (generation != m_pluginListRefreshGeneration) {
        refreshPluginListIfNeeded();
        return;
      }
      m_pluginList = std::move(plugins);
      m_pluginListDirty = false;
      if (m_content != nullptr && m_selectedSection == "plugins") {
        queueRebuild();
      }
    });
  }).detach();
}

void SettingsPane::resetBarLane(std::vector<std::string> lanePath) {
  DeferredCall::callLater([this, lanePath = std::move(lanePath)]() mutable {
    if (m_config == nullptr) {
      return;
    }
    bool changed = false;
    if (!m_config->resetBarLaneOverride(lanePath, &changed)) {
      m_statusMessage = i18n::tr("settings.errors.write");
      m_statusIsError = true;
      queueRebuild();
      return;
    }
    m_statusMessage.clear();
    m_statusIsError = false;
    m_pendingResetSettingPaths.clear();
    queueRebuild();
  });
}

void SettingsPane::renameWidgetInstance(
    std::string oldName, std::string newName,
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> referenceOverrides
) {
  DeferredCall::callLater([this, oldName = std::move(oldName), newName = std::move(newName),
                           referenceOverrides = std::move(referenceOverrides)]() mutable {
    if (m_config == nullptr) {
      return;
    }
    bool changed = m_config->renameOverrideTable({"widget", oldName}, {"widget", newName});
    if (!changed) {
      m_statusMessage = i18n::tr("settings.errors.widget.rename");
      m_statusIsError = true;
      queueRebuild();
      return;
    }
    bool failed = false;
    for (auto& [path, value] : referenceOverrides) {
      if (m_config->setOverride(path, std::move(value))) {
        changed = true;
      } else {
        failed = true;
      }
    }
    if (failed) {
      m_statusMessage = i18n::tr("settings.errors.batch-write");
      m_statusIsError = true;
      queueRebuild();
      return;
    }
    m_statusMessage.clear();
    m_statusIsError = false;
    queueRebuild();
  });
}

void SettingsPane::createBar(std::string name) {
  DeferredCall::callLater([this, name = std::move(name)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->createBarOverride(name)) {
      m_selectedBarName = name;
      m_selectedMonitorOverride.clear();
      m_creatingBarName.clear();
      m_renamingBarName.clear();
      m_pendingDeleteBarName.clear();
      m_scrollState.offset = 0.0F;
      m_statusMessage.clear();
      m_statusIsError = false;
      queueRebuild();
      return;
    }
    m_statusMessage = i18n::tr("settings.errors.bar.create");
    m_statusIsError = true;
    queueRebuild();
  });
}

void SettingsPane::renameBar(std::string oldName, std::string newName) {
  DeferredCall::callLater([this, oldName = std::move(oldName), newName = std::move(newName)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->renameBarOverride(oldName, newName)) {
      if (m_selectedBarName == oldName) {
        m_selectedBarName = newName;
      }
      m_selectedMonitorOverride.clear();
      m_renamingBarName.clear();
      m_pendingDeleteBarName.clear();
      m_scrollState.offset = 0.0F;
      m_statusMessage.clear();
      m_statusIsError = false;
      queueRebuild();
      return;
    }
    m_statusMessage = i18n::tr("settings.errors.bar.rename");
    m_statusIsError = true;
    queueRebuild();
  });
}

void SettingsPane::deleteBar(std::string name) {
  DeferredCall::callLater([this, name = std::move(name)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->deleteBarOverride(name)) {
      if (m_selectedBarName == name) {
        m_selectedBarName.clear();
        m_selectedMonitorOverride.clear();
        m_scrollState.offset = 0.0F;
      }
      m_renamingBarName.clear();
      m_pendingDeleteBarName.clear();
      m_statusMessage.clear();
      m_statusIsError = false;
      queueRebuild();
      return;
    }
    m_statusMessage = i18n::tr("settings.errors.bar.delete");
    m_statusIsError = true;
    queueRebuild();
  });
}

void SettingsPane::moveBar(std::string name, int direction) {
  DeferredCall::callLater([this, name = std::move(name), direction]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->moveBarOverride(name, direction)) {
      m_statusMessage.clear();
      m_statusIsError = false;
      queueRebuild();
      return;
    }
    m_statusMessage = i18n::tr("settings.errors.bar.move");
    m_statusIsError = true;
    queueRebuild();
  });
}

void SettingsPane::createMonitorOverride(std::string barName, std::string match) {
  DeferredCall::callLater([this, barName = std::move(barName), match = std::move(match)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->createMonitorOverride(barName, match)) {
      m_selectedBarName = barName;
      m_selectedMonitorOverride = match;
      m_creatingMonitorOverrideMatch.clear();
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_pendingDeleteMonitorOverrideBarName.clear();
      m_pendingDeleteMonitorOverrideMatch.clear();
      m_scrollState.offset = 0.0F;
      m_statusMessage.clear();
      m_statusIsError = false;
      queueRebuild();
      return;
    }
    m_statusMessage = i18n::tr("settings.errors.monitor-override.create");
    m_statusIsError = true;
    queueRebuild();
  });
}

void SettingsPane::renameMonitorOverride(std::string barName, std::string oldMatch, std::string newMatch) {
  DeferredCall::callLater([this, barName = std::move(barName), oldMatch = std::move(oldMatch),
                           newMatch = std::move(newMatch)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->renameMonitorOverride(barName, oldMatch, newMatch)) {
      if (m_selectedBarName == barName && m_selectedMonitorOverride == oldMatch) {
        m_selectedMonitorOverride = newMatch;
      }
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_pendingDeleteMonitorOverrideBarName.clear();
      m_pendingDeleteMonitorOverrideMatch.clear();
      m_scrollState.offset = 0.0F;
      m_statusMessage.clear();
      m_statusIsError = false;
      queueRebuild();
      return;
    }
    m_statusMessage = i18n::tr("settings.errors.monitor-override.rename");
    m_statusIsError = true;
    queueRebuild();
  });
}

void SettingsPane::deleteMonitorOverride(std::string barName, std::string match) {
  DeferredCall::callLater([this, barName = std::move(barName), match = std::move(match)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->deleteMonitorOverride(barName, match)) {
      if (m_selectedBarName == barName && m_selectedMonitorOverride == match) {
        m_selectedMonitorOverride.clear();
        m_scrollState.offset = 0.0F;
      }
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_pendingDeleteMonitorOverrideBarName.clear();
      m_pendingDeleteMonitorOverrideMatch.clear();
      m_statusMessage.clear();
      m_statusIsError = false;
      queueRebuild();
      return;
    }
    m_statusMessage = i18n::tr("settings.errors.monitor-override.delete");
    m_statusIsError = true;
    queueRebuild();
  });
}

void SettingsPane::openActionsMenu() {
  if (m_actionsMenuButton == nullptr || m_platform == nullptr) {
    return;
  }
  auto* renderContext = PanelManager::instance().renderContext();
  auto parentCtx = PanelManager::instance().fallbackPopupParentContext();
  if (renderContext == nullptr || !parentCtx.has_value()) {
    return;
  }
  if (m_actionsMenuPopup == nullptr) {
    m_actionsMenuPopup = std::make_unique<ContextMenuPopup>(m_platform->wayland(), *renderContext);
  } else if (m_actionsMenuPopup->isOpen()) {
    m_actionsMenuPopup->close();
    return;
  }

  constexpr std::int32_t kActionSupportReport = 1;
  constexpr std::int32_t kActionExportConfig = 2;
  m_actionsMenuPopup->setOnActivate([this](const ContextMenuControlEntry& entry) {
    switch (entry.id) {
    case kActionSupportReport:
      if (m_actionsMenuPopup != nullptr) {
        m_actionsMenuPopup->close();
      }
      DeferredCall::callLater([this]() { saveSupportReport(); });
      break;
    case kActionExportConfig:
      if (m_actionsMenuPopup != nullptr) {
        m_actionsMenuPopup->close();
      }
      DeferredCall::callLater([this]() { openConfigExportDialog(); });
      break;
    default:
      break;
    }
  });
  m_actionsMenuPopup->setOnDismissed([parentSurface = parentCtx->surface]() {
    PanelManager::instance().clearActivePopup();
    PanelManager::instance().endAttachedPopup(parentSurface);
  });

  float anchorAbsX = 0.0F;
  float anchorAbsY = 0.0F;
  Node::absolutePosition(m_actionsMenuButton, anchorAbsX, anchorAbsY);
  if (m_config != nullptr) {
    m_actionsMenuPopup->setShadowConfig(m_config->config().shell.shadow);
  }
  PanelManager::instance().beginAttachedPopup(parentCtx->surface);
  PanelManager::instance().setActivePopup(m_actionsMenuPopup.get());
  m_actionsMenuPopup->open(
      ContextMenuPopupRequest{
          .entries =
              {
                  {.id = kActionSupportReport, .label = i18n::tr("settings.window.support-report")},
                  {.id = kActionExportConfig, .label = i18n::tr("settings.window.export-config")},
              },
          .minMenuWidth = 220.0F * m_scale,
          .maxMenuWidth = Style::menuAutoMaxWidth * m_scale,
          .maxVisible = 8,
          .anchor =
              PopupAnchorRect{
                  .x = static_cast<std::int32_t>(anchorAbsX),
                  .y = static_cast<std::int32_t>(anchorAbsY),
                  .width = static_cast<std::int32_t>(m_actionsMenuButton->width()),
                  .height = static_cast<std::int32_t>(m_actionsMenuButton->height()),
              },
          .parent =
              PopupSurfaceParent{
                  .layerSurface = parentCtx->layerSurface,
                  .output = parentCtx->output,
              },
          .pointerParentSurface = parentCtx->surface,
      }
  );
}

void SettingsPane::openConfigExportDialog() {
  if (m_modalHost == nullptr || m_config == nullptr) {
    return;
  }
  if (m_configExportDialog == nullptr) {
    m_configExportDialog = std::make_unique<settings::ConfigExportDialogModal>();
    m_configExportDialog->initialize(*m_modalHost, []() {});
  }
  m_configExportDialog->open(
      settings::ConfigExportDialogRequest{
          .scale = m_scale,
          .callback = [this](settings::ConfigExportMode mode) { saveConfigExport(mode); },
      }
  );
}

void SettingsPane::saveSupportReport() {
  if (m_config == nullptr) {
    return;
  }
  FileDialogOptions options;
  options.mode = FileDialogMode::Save;
  options.defaultFilename = "noctalia-support-report.toml";
  options.title = i18n::tr("settings.window.support-report-title");
  options.extensions = {".toml"};
  const bool opened = FileDialog::open(std::move(options), [this](std::optional<std::filesystem::path> result) {
    if (!result.has_value() || m_config == nullptr) {
      return;
    }
    auto path = *result;
    if (path.extension().empty()) {
      path += ".toml";
    }
    if (!writeTextFileAtomic(path, m_config->buildSupportReport())) {
      m_statusMessage = i18n::tr("settings.errors.support-report");
      m_statusIsError = true;
      queueRebuild();
      return;
    }
    m_statusMessage = i18n::tr("settings.window.support-report-saved");
    m_statusIsError = false;
    queueRebuild();
  });
  if (!opened) {
    m_statusMessage = i18n::tr("settings.errors.support-report");
    m_statusIsError = true;
    queueRebuild();
  }
}

void SettingsPane::saveConfigExport(settings::ConfigExportMode mode) {
  if (m_config == nullptr) {
    return;
  }
  const bool fullEffective = mode == settings::ConfigExportMode::FullEffective;
  FileDialogOptions options;
  options.mode = FileDialogMode::Save;
  options.defaultFilename = fullEffective ? "noctalia-full-config.toml" : "noctalia-config.toml";
  options.title = fullEffective ? i18n::tr("settings.export-config.full-effective-save-title")
                                : i18n::tr("settings.export-config.merged-user-save-title");
  options.extensions = {".toml"};
  const bool opened = FileDialog::open(std::move(options), [this, mode](std::optional<std::filesystem::path> result) {
    if (!result.has_value() || m_config == nullptr) {
      return;
    }
    auto path = *result;
    if (path.extension().empty()) {
      path += ".toml";
    }
    const std::string content = mode == settings::ConfigExportMode::FullEffective ? m_config->buildEffectiveConfig()
                                                                                  : m_config->buildMergedUserConfig();
    if (!writeTextFileAtomic(path, content)) {
      m_statusMessage = i18n::tr("settings.errors.export-config");
      m_statusIsError = true;
      queueRebuild();
      return;
    }
    m_statusMessage = i18n::tr("settings.window.export-config-saved");
    m_statusIsError = false;
    queueRebuild();
  });
  if (!opened) {
    m_statusMessage = i18n::tr("settings.errors.export-config");
    m_statusIsError = true;
    queueRebuild();
  }
}
