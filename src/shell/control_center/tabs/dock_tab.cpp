#include "shell/control_center/tabs/dock_tab.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "shell/dock/pinned_apps.h"
#include "shell/panel/panel_manager.h"
#include "system/desktop_entry.h"
#include "ui/builders.h"
#include "ui/controls/list_editor.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

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

DockTab::DockTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "dock"})) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> DockTab::create() {
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
  });

  auto scroll = ui::scrollView({
      .contentScale = scale,
      .scrollbarVisible = true,
      .flexGrow = 1.0F,
      .configure =
          [](ScrollView& scrollView) {
            scrollView.clearFill();
            scrollView.clearBorder();
          },
  });
  auto* content = scroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Style::spaceMd * scale);

  auto addCaption = [scale, captionSize](Flex& parent, const std::string& text) {
    parent.addChild(
        ui::label({
            .text = text,
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  };

  auto addToggleRow = [scale, this](Flex& parent, const std::string& text, Toggle** out, bool checked,
                                    void (DockTab::*apply)(bool)) {
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .minHeight = Style::controlHeightSm * scale,
    });
    row->addChild(
        ui::label({
            .text = text,
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
    row->addChild(
        ui::toggle({
            .out = out,
            .checkedImmediate = checked,
            .toggleSize = ToggleSize::Medium,
            .scale = scale,
            .onChange =
                [this, apply](bool value) {
                  (this->*apply)(value);
                },
        })
    );
    parent.addChild(std::move(row));
  };

  const DockConfig dock = m_config != nullptr ? m_config->config().dock : DockConfig{};

  auto generalCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addTitle(*generalCard, i18n::tr("settings.navigation.sections.dock"), scale);
  addToggleRow(*generalCard, i18n::tr("control-center.dock.enable"), &m_enabled, dock.enabled, &DockTab::applyEnabled);

  addCaption(*generalCard, i18n::tr("control-center.dock.position"));
  generalCard->addChild(
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
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyPosition(index); },
      })
  );

  addCaption(*generalCard, i18n::tr("control-center.dock.auto-hide"));
  generalCard->addChild(
      ui::segmented({
          .out = &m_autoHide,
          .options =
              std::vector<ui::SegmentedOption>{
                  {.label = i18n::tr("settings.options.bar.auto-hide.off")},
                  {.label = i18n::tr("settings.options.bar.auto-hide.on")},
                  {.label = i18n::tr("settings.options.bar.auto-hide.smart")},
              },
          .selectedIndex = autoHideIndex(dock),
          .fontSize = captionSize,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyAutoHide(index); },
      })
  );
  addToggleRow(
      *generalCard, i18n::tr("control-center.dock.show-running"), &m_showRunning, dock.showRunning,
      &DockTab::applyShowRunning
  );
  content->addChild(std::move(generalCard));

  auto pinnedCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addTitle(*pinnedCard, i18n::tr("control-center.dock.pinned"), scale);
  auto pinnedList = std::make_unique<ListEditor>();
  m_pinnedList = pinnedList.get();
  m_pinnedList->setScale(scale);
  m_pinnedList->setAddPlaceholder(i18n::tr("settings.controls.list.add-entry-placeholder"));
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
  pinnedCard->addChild(std::move(pinnedList));
  content->addChild(std::move(pinnedCard));

  live->addChild(std::move(scroll));
  auto wrapped = m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
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
  m_position = nullptr;
  m_autoHide = nullptr;
  m_showRunning = nullptr;
  m_pinnedList = nullptr;
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
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void DockTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
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
