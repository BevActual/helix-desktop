#include "shell/control_center/settings_pane.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
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
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/text/font_weight_catalog.h"
#include "shell/bar/widget_action.h"
#include "shell/control_center/tab.h"
#include "shell/greeter/greeter_appearance_sync.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "shell/profile/avatar_path.h"
#include "shell/settings/font_family_catalog.h"
#include "shell/settings/settings_hosted_editors.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_content_common.h"
#include "system/battery_warning_monitor.h"
#include "system/day_night_schedule.h"
#include "system/dependency_service.h"
#include "theme/builtin_templates.h"
#include "theme/community_palettes.h"
#include "theme/community_templates.h"
#include "theme/custom_palettes.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/scroll_into_view.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
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
      m_asyncTextures(services.asyncTextures), m_selectedSection(std::move(options.section)),
      m_includedGroups(std::move(options.includedGroups)), m_compact(options.compact) {
  m_showAdvanced = m_config != nullptr ? m_config->config().shell.settingsShowAdvanced : true;
}

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
                m_scrollState.offset = 0.0F;
                queueRebuild();
              },
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

  auto scroll = ui::scrollView({
      .out = &m_scroll,
      .state = &m_scrollState,
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = m_compact ? 0.0F : 1.0F,
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
          .out = &m_openButton,
          .glyph = "external-link",
          .tooltip = i18n::tr("control-center.settings.open"),
          .onClick = [this]() { openFullSettings(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  return row;
}

std::unique_ptr<Flex> SettingsPane::wrapLive(std::unique_ptr<Flex> live, float scale, float panelCardOpacity) {
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
  auto editor = create(scale, panelCardOpacity);
  editor->setFlexGrow(1.0F);
  host->addChild(std::move(editor));
  applyConfigureVisibility();
  return host;
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
  m_openButton = nullptr;
  m_layoutRenderer = nullptr;
  m_rebuildQueued = false;
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
  syncBarSelect();
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
  const auto registry = settings::buildSettingsRegistry(cfg, selectedBar, nullptr, env);
  (void)settings::addSettingsContentSections(content, registry, makeContentContext(cfg, selectedBar, nullptr));

  if (m_selectedSection != "plugins") {
    return;
  }

  auto card = ui::column({});
  control_center::applySectionCardStyle(*card, m_scale, m_panelCardOpacity);
  card->addChild(
      ui::label({
          .text = i18n::tr("control-center.settings.plugins-hint"),
          .fontSize = Style::fontSizeBody * m_scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  card->addChild(
      ui::button({
          .text = i18n::tr("control-center.settings.plugins-open"),
          .glyph = "puzzle",
          .fontSize = Style::fontSizeBody * m_scale,
          .contentAlign = ButtonContentAlign::Start,
          .minHeight = Style::controlHeight * m_scale,
          .onClick =
              [this]() {
                bindHostedEditors();
                if (m_hostedEditors != nullptr) {
                  m_hostedEditors->openPluginStore();
                } else {
                  openFullSettings();
                }
              },
      })
  );
  content.addChild(std::move(card));
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
  const auto openEditors = [this]() { openFullSettings(); };
  const bool searching = !m_searchQuery.empty();
  return settings::SettingsContentContext{
      .config = cfg,
      .configService = m_config,
      .scale = m_scale,
      .searchQuery = m_searchQuery,
      .selectedSection = searching ? std::string_view{} : m_selectedSection,
      .includedGroups = searching ? std::unordered_set<std::string>{} : m_includedGroups,
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
          [this, openEditors](const std::vector<std::string>& lanePath) {
            bindHostedEditors();
            if (m_hostedEditors != nullptr) {
              m_hostedEditors->openBarWidgetAdd(lanePath);
            } else {
              openEditors();
            }
          },
      .openSearchPickerPopup = {},
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
      .resetBarLane = [openEditors](std::vector<std::string>) { openEditors(); },
      .isResetConfirmationPending =
          [this](const std::vector<std::vector<std::string>>& paths) { return m_pendingResetSettingPaths == paths; },
      .requestResetConfirmation =
          [this](std::vector<std::vector<std::string>> paths) {
            m_pendingResetSettingPaths = std::move(paths);
            queueRebuild();
          },
      .renameWidgetInstance =
          [this, openEditors](
              std::string oldName, std::string newName,
              std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides
          ) {
            bindHostedEditors();
            if (m_hostedEditors != nullptr && m_hostedEditors->sheet() != nullptr) {
              setSettingOverrides(std::move(overrides));
              (void)oldName;
              (void)newName;
            } else {
              openEditors();
            }
          },
      .openSessionActionEntryEditor =
          [this, openEditors](std::size_t index) {
            bindHostedEditors();
            if (m_hostedEditors != nullptr) {
              m_hostedEditors->openSessionActionEntry(index);
            } else {
              openEditors();
            }
          },
      .openIdleBehaviorEntryEditor =
          [this, openEditors](std::size_t index) {
            bindHostedEditors();
            if (m_hostedEditors != nullptr) {
              m_hostedEditors->openIdleBehaviorEntry(index);
            } else {
              openEditors();
            }
          },
      .openIdleBehaviorCreateEditor =
          [this, openEditors]() {
            bindHostedEditors();
            if (m_hostedEditors != nullptr) {
              m_hostedEditors->openIdleBehaviorCreate();
            } else {
              openEditors();
            }
          },
      .openNotificationFilterEntryEditor =
          [this, openEditors](std::size_t index) {
            bindHostedEditors();
            if (m_hostedEditors != nullptr) {
              m_hostedEditors->openNotificationFilterEntry(index);
            } else {
              openEditors();
            }
          },
      .openNotificationFilterCreateEditor =
          [this, openEditors]() {
            bindHostedEditors();
            if (m_hostedEditors != nullptr) {
              m_hostedEditors->openNotificationFilterCreate();
            } else {
              openEditors();
            }
          },
      .openWidgetInspectorEditor =
          [this, openEditors](std::vector<std::string> lanePath, std::string widgetName) {
            bindHostedEditors();
            if (m_hostedEditors != nullptr) {
              m_hostedEditors->openWidgetInspector(std::move(lanePath), std::move(widgetName));
            } else {
              openEditors();
            }
          },
      .openCapsuleGroupEditor =
          [this, openEditors](std::vector<std::string> lanePath, std::string groupId) {
            bindHostedEditors();
            if (m_hostedEditors != nullptr) {
              m_hostedEditors->openCapsuleGroup(std::move(lanePath), std::move(groupId));
            } else {
              openEditors();
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
    if (!m_config->setOverrides(std::move(overrides))) {
      m_statusMessage = settingsMutationError(*m_config, i18n::tr("settings.errors.batch-write"));
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

void SettingsPane::openFullSettings() const {
  PanelManager::instance().openSettingsWindow(m_selectedSection);
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
                return makeContentContext(cfg, selectedBar, nullptr);
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
  if (m_barSelect == nullptr || m_config == nullptr) {
    return;
  }
  const auto names = settings::barNames(m_config->config());
  m_barSelect->setOptions(names);
  const auto it = std::ranges::find(names, m_selectedBarName);
  if (it != names.end()) {
    m_barSelect->setSelectedIndexSilently(static_cast<std::size_t>(it - names.begin()));
  }
  if (m_barSelectRow != nullptr) {
    m_barSelectRow->setVisible(!m_compact && m_selectedSection == "bar" && m_searchQuery.empty());
  }
}
