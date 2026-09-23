#include "shell/control_center/tabs/dock_tab.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/control_center/pinned_app_list.h"
#include "shell/dock/pinned_apps.h"
#include "shell/panel/panel_manager.h"
#include "system/desktop_entry.h"
#include "ui/builders.h"
#include "ui/controls/list_editor.h"
#include "ui/controls/segmented.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  [[nodiscard]] bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return changed;
  }

  constexpr std::size_t kAutoHideOff = 0;
  constexpr std::size_t kAutoHideOn = 1;
  constexpr std::size_t kAutoHideSmart = 2;

  [[nodiscard]] std::size_t autoHideIndex(const DockConfig& dock) {
    if (dock.smartAutoHide) {
      return kAutoHideSmart;
    }
    if (dock.autoHide) {
      return kAutoHideOn;
    }
    return kAutoHideOff;
  }

  [[nodiscard]] std::vector<ListEditorOption> pinnedAppOptions(const std::vector<std::string>& pinned) {
    std::vector<ListEditorOption> options;
    for (const auto& entry : desktopEntries()) {
      if (entry.noDisplay || entry.hidden) {
        continue;
      }
      options.push_back(ListEditorOption{.value = entry.id, .label = entry.name.empty() ? entry.id : entry.name});
    }
    for (const auto& resolved : shell::dock::pinned_apps::resolveEntries(pinned)) {
      const bool known = std::ranges::any_of(options, [&](const ListEditorOption& opt) {
        return opt.value == resolved.id;
      });
      if (!known) {
        options.push_back(ListEditorOption{
            .value = resolved.id,
            .label = resolved.name.empty() ? resolved.id : resolved.name,
        });
      }
    }
    return options;
  }

} // namespace

std::size_t DockTab::indexFromSection(Section section) noexcept {
  return section == Section::Pinned ? 1 : 0;
}

DockTab::Section DockTab::sectionFromIndex(std::size_t index) noexcept {
  return index == 1 ? Section::Pinned : Section::Layout;
}

DockTab::Section DockTab::sectionFromId(std::string_view id) noexcept {
  return id == "pinned" ? Section::Pinned : Section::Layout;
}

std::string_view DockTab::idFromSection(Section section) noexcept {
  return section == Section::Pinned ? "pinned" : "layout";
}

DockTab::DockTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "dock", .showConfigureButton = false}
      )) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> section = m_config->stateString("dock_tab", "section")) {
      m_section = sectionFromId(*section);
    }
  }
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> DockTab::create() {
  return m_settings->wrapLive(createLiveBody(), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> DockTab::createLiveBody(bool hosted) {
  m_hosted = hosted;
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;
  const float opacity = panelCardOpacity();
  const DockConfig dock = m_config != nullptr ? m_config->config().dock : DockConfig{};

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = hosted ? 0.0F : 1.0F,
  });

  auto compactCard = ui::column({
      .configure =
          [scale, opacity, hosted](Flex& card) {
            applyCompactCardStyle(card, scale, opacity, hosted);
          },
  });
  m_enabled = addToggleRow(*compactCard, scale, i18n::tr("control-center.dock.enable"), [this](bool value) {
    applyEnabled(value);
  });
  if (m_enabled != nullptr) {
    m_enabled->setCheckedImmediate(dock.enabled);
  }

  if (!hosted) {
    auto sectionToolbar = ui::column({
        .out = &m_sectionToolbar,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .visible = dock.enabled,
        .participatesInLayout = dock.enabled,
    });
    sectionToolbar->addChild(
        ui::segmented({
            .out = &m_sectionSwitch,
            .options =
                std::vector<ui::SegmentedOption>{
                    {
                        .label = "",
                        .glyph = "layout-bottombar-inactive",
                        .tooltip = i18n::tr("control-center.dock.layout-tooltip"),
                    },
                    {
                        .label = "",
                        .glyph = "pin",
                        .tooltip = i18n::tr("control-center.dock.pinned-tooltip"),
                    },
                },
            .selectedIndex = indexFromSection(m_section),
            .scale = scale,
            .compact = true,
            .surfaceOpacity = opacity,
            .onChange = [this](std::size_t index) { applySection(index); },
        })
    );
    sectionToolbar->addChild(
        ui::label({
            .out = &m_caption,
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 2,
        })
    );
    compactCard->addChild(std::move(sectionToolbar));
  }
  live->addChild(std::move(compactCard));

  const bool layoutOpen = dock.enabled && m_section == Section::Layout;
  auto layoutHost = ui::column({
      .out = &m_layoutHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = hosted ? 0.0F : 1.0F,
      .visible = layoutOpen,
      .participatesInLayout = layoutOpen,
      .configure =
          [scale, hosted](Flex& section) {
            if (!hosted) {
              applyContentCardStyle(section, scale);
            }
          },
  });
  addCaption(*layoutHost, i18n::tr("control-center.dock.position"), scale);
  layoutHost->addChild(
      ui::segmented({
          .out = &m_position,
          .options =
              std::vector<ui::SegmentedOption>{
                  {.label = i18n::tr("settings.options.edge.top")},
                  {.label = i18n::tr("settings.options.edge.bottom")},
                  {.label = i18n::tr("settings.options.edge.left")},
                  {.label = i18n::tr("settings.options.edge.right")},
              },
          .selectedIndex = static_cast<std::size_t>(dock.position),
          .fontSize = captionSize,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = opacity,
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyPosition(index); },
      })
  );
  addCaption(*layoutHost, i18n::tr("control-center.dock.auto-hide"), scale);
  layoutHost->addChild(
      ui::segmented({
          .out = &m_autoHide,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = i18n::tr("settings.options.bar.auto-hide.off"),
                      .tooltip = i18n::tr("control-center.bar.auto-hide-off-tooltip"),
                  },
                  {
                      .label = i18n::tr("settings.options.bar.auto-hide.on"),
                      .tooltip = i18n::tr("control-center.bar.auto-hide-on-tooltip"),
                  },
                  {
                      .label = i18n::tr("settings.options.bar.auto-hide.smart"),
                      .tooltip = i18n::tr("control-center.bar.auto-hide-smart-tooltip"),
                  },
              },
          .selectedIndex = autoHideIndex(dock),
          .fontSize = captionSize,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = opacity,
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyAutoHide(index); },
      })
  );
  m_showRunning = addToggleRow(
      *layoutHost, scale, i18n::tr("control-center.dock.show-running"), [this](bool value) { applyShowRunning(value); }
  );
  if (m_showRunning != nullptr) {
    m_showRunning->setCheckedImmediate(dock.showRunning);
  }
  live->addChild(std::move(layoutHost));

  const bool pinnedOpen = dock.enabled && m_section == Section::Pinned;
  auto pinnedHost = ui::column({
      .out = &m_pinnedHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = hosted ? 0.0F : 1.0F,
      .visible = pinnedOpen,
      .participatesInLayout = pinnedOpen,
      .configure =
          [scale, hosted](Flex& section) {
            if (!hosted) {
              applyContentCardStyle(section, scale);
            }
          },
  });
  auto pinnedList = std::make_unique<ListEditor>();
  m_pinnedList = pinnedList.get();
  m_pinnedList->setScale(scale);
  m_pinnedIcons.attach(*m_pinnedList, scale);
  m_pinnedList->setAddPlaceholder(i18n::tr("control-center.dock.add-app"));
  m_pinnedList->setOnAddRequested([this](std::string value) {
    if (m_syncing || m_config == nullptr || value.empty()) {
      return;
    }
    auto items = m_config->config().dock.pinned;
    items.push_back(std::move(value));
    commitPinned(std::move(items));
  });
  m_pinnedList->setOnRemoveRequested([this](std::size_t index) {
    if (m_syncing || m_config == nullptr) {
      return;
    }
    auto items = m_config->config().dock.pinned;
    if (index >= items.size()) {
      return;
    }
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
    commitPinned(std::move(items));
  });
  m_pinnedList->setOnMoveRequested([this](std::size_t from, std::size_t to) {
    if (m_syncing || m_config == nullptr || from == to) {
      return;
    }
    auto items = m_config->config().dock.pinned;
    if (from >= items.size() || to >= items.size()) {
      return;
    }
    const std::string item = items[from];
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(from));
    items.insert(items.begin() + static_cast<std::ptrdiff_t>(to), item);
    commitPinned(std::move(items));
  });
  pinnedHost->addChild(std::move(pinnedList));
  live->addChild(std::move(pinnedHost));

  syncLiveControls();
  return live;
}

std::unique_ptr<Flex> DockTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void DockTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_enabled = nullptr;
  m_sectionToolbar = nullptr;
  m_sectionSwitch = nullptr;
  m_caption = nullptr;
  m_layoutHost = nullptr;
  m_pinnedHost = nullptr;
  m_position = nullptr;
  m_autoHide = nullptr;
  m_showRunning = nullptr;
  m_pinnedList = nullptr;
  m_pinnedIcons.clear();
}

void DockTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool DockTab::configureMode() const { return m_settings->configureMode(); }

void DockTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

SettingsPane* DockTab::settingsPane() { return m_settings.get(); }

void DockTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  if (m_hosted) {
    layoutHostedLiveBody(m_rootLayout, renderer, contentWidth);
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void DockTab::doUpdate(Renderer& renderer) {
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
  if (m_config != nullptr) {
    m_pinnedIcons.load(renderer, m_config->config().dock.pinned, contentScale());
  }
}

void DockTab::onPanelCardOpacityChanged(float opacity) {
  if (m_sectionSwitch != nullptr) {
    m_sectionSwitch->setSurfaceOpacity(opacity);
  }
  if (m_position != nullptr) {
    m_position->setSurfaceOpacity(opacity);
  }
  if (m_autoHide != nullptr) {
    m_autoHide->setSurfaceOpacity(opacity);
  }
}

void DockTab::syncHosts() {
  const bool enabled = m_config != nullptr && m_config->config().dock.enabled;
  const bool layoutOpen = enabled && m_section == Section::Layout;
  const bool pinnedOpen = enabled && m_section == Section::Pinned;
  bool layoutDirty = false;
  layoutDirty |= setShown(m_sectionToolbar, enabled);
  layoutDirty |= setShown(m_layoutHost, layoutOpen);
  layoutDirty |= setShown(m_pinnedHost, pinnedOpen);
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
}

void DockTab::persistSection() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("dock_tab", "section", std::string(idFromSection(m_section)));
}

void DockTab::applySection(std::size_t index) {
  if (m_sectionSyncing) {
    return;
  }
  const Section section = sectionFromIndex(index);
  if (m_section == section) {
    syncHosts();
    return;
  }
  m_section = section;
  persistSection();
  if (m_sectionSwitch != nullptr) {
    m_sectionSyncing = true;
    m_sectionSwitch->setSelectedIndex(indexFromSection(m_section));
    m_sectionSyncing = false;
  }
  syncHosts();
  syncLiveControls();
}

void DockTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }

  const DockConfig& dock = m_config->config().dock;
  m_syncing = true;
  if (m_enabled != nullptr) {
    m_enabled->setCheckedImmediate(dock.enabled);
  }
  if (m_caption != nullptr) {
    m_caption->setText(
        m_section == Section::Pinned ? i18n::tr("control-center.dock.pinned-caption")
                                     : i18n::tr("control-center.dock.layout-caption")
    );
  }
  syncHosts();
  if (m_position != nullptr) {
    m_position->setSelectedIndex(static_cast<std::size_t>(dock.position));
  }
  if (m_autoHide != nullptr) {
    m_autoHide->setSelectedIndex(autoHideIndex(dock));
  }
  if (m_showRunning != nullptr) {
    m_showRunning->setCheckedImmediate(dock.showRunning);
  }
  if (m_pinnedList != nullptr) {
    m_pinnedList->setSuggestedOptions(pinnedAppOptions(dock.pinned));
    m_pinnedList->setItems(dock.pinned);
  }
  m_syncing = false;
}

void DockTab::applyEnabled(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"dock", "enabled"}, enabled);
  syncLiveControls();
  PanelManager::instance().refresh();
}

void DockTab::applyPosition(std::size_t index) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  DockEdge edge = DockEdge::Bottom;
  if (index < std::size(kDockEdges)) {
    edge = kDockEdges[index].value;
  }
  (void)m_config->setOverride({"dock", "position"}, std::string(enumToKey(kDockEdges, edge)));
  PanelManager::instance().refresh();
}

void DockTab::applyAutoHide(std::size_t index) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverrides({
      {{"dock", "auto_hide"}, index == kAutoHideOn},
      {{"dock", "smart_auto_hide"}, index == kAutoHideSmart},
  });
  PanelManager::instance().refresh();
}

void DockTab::applyShowRunning(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"dock", "show_running"}, enabled);
  PanelManager::instance().refresh();
}

void DockTab::commitPinned(std::vector<std::string> items) {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"dock", "pinned"}, std::move(items));
  PanelManager::instance().refresh();
}
