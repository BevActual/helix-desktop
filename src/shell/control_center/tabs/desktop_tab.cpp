#include "shell/control_center/tabs/desktop_tab.h"

#include "compositors/compositor_detect.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/desktop/desktop_widgets_controller.h"
#include "shell/panel/panel_manager.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/grid_view.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <filesystem>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  constexpr std::array<std::string_view, 6> kHotCornerActions{
      "none", "launcher", "control_center", "widgets", "window_switcher", "command"};

  Button::ButtonPalette catalogTilePalette() {
    const ColorSpec fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.55F);
    const ColorSpec label = colorSpecFromRole(ColorRole::OnSurface);
    const ColorSpec restBorder = colorSpecFromRole(ColorRole::Outline, 0.28F);
    const ColorSpec selectedBorder = colorSpecFromRole(ColorRole::Primary);
    Button::ButtonStateColors rest{.bg = fill, .border = restBorder, .label = label};
    Button::ButtonStateColors selected{.bg = fill, .border = selectedBorder, .label = label};
    return {
        .borderWidth = 2.0F,
        .normal = rest,
        .hover = selected,
        .pressed = selected,
        .disabled = rest,
        .selected = selected,
    };
  }

  bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    if (changed) {
      node->setVisible(shown);
      node->setParticipatesInLayout(shown);
    }
    return changed;
  }

  [[nodiscard]] std::string_view glyphForWidgetType(std::string_view type) {
    if (type == "clock") {
      return "clock";
    }
    if (type == "calendar") {
      return "calendar-event";
    }
    if (type == "weather") {
      return "weather-cloud-sun";
    }
    if (type == "media_player") {
      return "disc-filled";
    }
    if (type == "volume") {
      return "volume";
    }
    if (type == "sysmon") {
      return "activity-heartbeat";
    }
    if (type == "audio_visualizer") {
      return "chart-bar";
    }
    if (type == "fancy_audio_visualizer") {
      return "sparkles";
    }
    if (type == "button") {
      return "click";
    }
    if (type == "label") {
      return "typography";
    }
    if (type == "sticker") {
      return "photo";
    }
    if (type == "folder") {
      return "folder";
    }
    if (type == "file") {
      return "file";
    }
    return "puzzle";
  }

  [[nodiscard]] std::size_t enabledWidgetCount(const DesktopWidgetsConfig& widgets) {
    std::size_t count = 0;
    for (const auto& widget : widgets.widgets) {
      if (widget.enabled) {
        ++count;
      }
    }
    return count;
  }

  [[nodiscard]] std::size_t enabledCountForType(const DesktopWidgetsConfig& widgets, std::string_view type) {
    std::size_t count = 0;
    for (const auto& widget : widgets.widgets) {
      if (widget.enabled && widget.type == type) {
        ++count;
      }
    }
    return count;
  }

  [[nodiscard]] std::vector<std::string> hotCornerActionLabels() {
    return {
        i18n::tr("settings.options.hot-corners.none"),
        i18n::tr("settings.options.hot-corners.launcher"),
        i18n::tr("settings.options.hot-corners.control-center"),
        i18n::tr("settings.options.hot-corners.widgets"),
        i18n::tr("settings.options.hot-corners.window-switcher"),
        i18n::tr("settings.options.hot-corners.command"),
    };
  }

  [[nodiscard]] std::size_t hotCornerActionIndex(std::string_view action) {
    for (std::size_t i = 0; i < kHotCornerActions.size(); ++i) {
      if (kHotCornerActions[i] == action) {
        return i;
      }
    }
    return 0;
  }

  [[nodiscard]] std::string pixelLabel(double value) { return std::format("{:.0f}", value); }

  [[nodiscard]] std::string delayLabel(double value) { return std::format("{:.0f} ms", value); }

} // namespace

DesktopTab::Section DesktopTab::sectionFromId(std::string_view id) noexcept {
  if (id == "corners" || id == "hot-corners" || id == "screen-corners" || id == "hot_corners"
      || id == "screen_corners") {
    return Section::Corners;
  }
  if (id == "bar") {
    return Section::Bar;
  }
  if (id == "dock") {
    return Section::Dock;
  }
  if (id == "windows") {
    return Section::Windows;
  }
  if (id == "panels") {
    return Section::Panels;
  }
  if (id == "osd") {
    return Section::Osd;
  }
  return Section::Widgets;
}

std::string_view DesktopTab::idFromSection(Section section) noexcept {
  switch (section) {
  case Section::Corners:
    return "corners";
  case Section::Bar:
    return "bar";
  case Section::Dock:
    return "dock";
  case Section::Windows:
    return "windows";
  case Section::Panels:
    return "panels";
  case Section::Osd:
    return "osd";
  case Section::Widgets:
  default:
    return "widgets";
  }
}

std::vector<DesktopTab::Section> DesktopTab::visibleSections() const {
  std::vector<Section> sections{Section::Widgets, Section::Corners, Section::Bar, Section::Dock};
  if (compositors::isNiri()) {
    sections.push_back(Section::Windows);
  }
  sections.push_back(Section::Panels);
  sections.push_back(Section::Osd);
  return sections;
}

std::size_t DesktopTab::indexFromSection(Section section) const noexcept {
  const auto sections = visibleSections();
  for (std::size_t i = 0; i < sections.size(); ++i) {
    if (sections[i] == section) {
      return i;
    }
  }
  return 0;
}

DesktopTab::Section DesktopTab::sectionFromIndex(std::size_t index) const noexcept {
  const auto sections = visibleSections();
  if (index >= sections.size()) {
    return Section::Widgets;
  }
  return sections[index];
}

std::size_t DesktopTab::indexFromWidgetCatalog(WidgetCatalog catalog) noexcept {
  switch (catalog) {
  case WidgetCatalog::Sound:
    return 1;
  case WidgetCatalog::Tools:
    return 2;
  case WidgetCatalog::Glanceable:
  default:
    return 0;
  }
}

DesktopTab::WidgetCatalog DesktopTab::widgetCatalogFromIndex(std::size_t index) noexcept {
  switch (index) {
  case 1:
    return WidgetCatalog::Sound;
  case 2:
    return WidgetCatalog::Tools;
  default:
    return WidgetCatalog::Glanceable;
  }
}

DesktopTab::WidgetCatalog DesktopTab::widgetCatalogFromId(std::string_view id) noexcept {
  if (id == "sound" || id == "media") {
    return WidgetCatalog::Sound;
  }
  if (id == "tools" || id == "custom") {
    return WidgetCatalog::Tools;
  }
  return WidgetCatalog::Glanceable;
}

std::string_view DesktopTab::idFromWidgetCatalog(WidgetCatalog catalog) noexcept {
  switch (catalog) {
  case WidgetCatalog::Sound:
    return "sound";
  case WidgetCatalog::Tools:
    return "tools";
  case WidgetCatalog::Glanceable:
  default:
    return "glanceable";
  }
}

std::size_t DesktopTab::indexFromCornersCatalog(CornersCatalog catalog) noexcept {
  return catalog == CornersCatalog::Hot ? 1 : 0;
}

DesktopTab::CornersCatalog DesktopTab::cornersCatalogFromIndex(std::size_t index) noexcept {
  return index == 1 ? CornersCatalog::Hot : CornersCatalog::Screen;
}

DesktopTab::CornersCatalog DesktopTab::cornersCatalogFromId(std::string_view id) noexcept {
  if (id == "hot" || id == "hot-corners" || id == "hot_corners") {
    return CornersCatalog::Hot;
  }
  return CornersCatalog::Screen;
}

std::string_view DesktopTab::idFromCornersCatalog(CornersCatalog catalog) noexcept {
  return catalog == CornersCatalog::Hot ? "hot" : "screen";
}

bool DesktopTab::widgetTypeInCatalog(std::string_view type, WidgetCatalog catalog) noexcept {
  switch (catalog) {
  case WidgetCatalog::Glanceable:
    return type == "clock" || type == "calendar" || type == "weather";
  case WidgetCatalog::Sound:
    return type == "media_player" || type == "volume" || type == "audio_visualizer"
        || type == "fancy_audio_visualizer";
  case WidgetCatalog::Tools:
    return type == "sysmon" || type == "button" || type == "label" || type == "sticker" || type == "folder"
      || type == "file";
  }
  return true;
}

DesktopTab::DesktopTab(const ControlCenterServices& services)
    : m_config(services.config), m_widgets(services.desktopWidgets),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "desktop"})),
      m_bar(std::make_unique<BarTab>(services)), m_dock(std::make_unique<DockTab>(services)),
      m_windows(std::make_unique<WindowsTab>(services)), m_panels(std::make_unique<PanelsTab>(services)),
      m_osd(std::make_unique<OsdTab>(services)) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> section = m_config->stateString("desktop_tab", "section")) {
      m_section = sectionFromId(*section);
    }
    if (m_section == Section::Windows && !compositors::isNiri()) {
      m_section = Section::Widgets;
    }
    if (const std::optional<std::string> catalog = m_config->stateString("desktop_tab", "widgets")) {
      m_widgetCatalog = widgetCatalogFromId(*catalog);
    }
    if (const std::optional<std::string> catalog = m_config->stateString("desktop_tab", "corners")) {
      m_cornersCatalog = cornersCatalogFromId(*catalog);
    }
  }
  syncSettingsSection();
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> DesktopTab::create() {
  const float scale = contentScale();
  const float opacity = panelCardOpacity();
  const Config* cfg = m_config != nullptr ? &m_config->config() : nullptr;
  const bool widgetsEnabled = cfg != nullptr && cfg->desktopWidgets.enabled;
  const bool cornersEnabled = cfg != nullptr && cfg->shell.screenCorners.enabled;
  const bool hotEnabled = cfg != nullptr && cfg->hotCorners.enabled;
  const auto& sizeRange = noctalia::config::schema::kScreenCornersSizeRange;
  const auto& delayRange = noctalia::config::schema::kHotCornersDelayMsRange;
  const double sizeMin = static_cast<double>(sizeRange.min.value_or(1));
  const double sizeMax = static_cast<double>(sizeRange.max.value_or(100));
  const double sizeStep = static_cast<double>(sizeRange.step.value_or(1));
  const double delayMin = static_cast<double>(delayRange.min.value_or(0));
  const double delayMax = static_cast<double>(delayRange.max.value_or(2000));
  const double delayStep = static_cast<double>(delayRange.step.value_or(50));
  const double cornerSize = cfg != nullptr ? static_cast<double>(cfg->shell.screenCorners.size) : 32.0;
  const double hotDelay = cfg != nullptr ? static_cast<double>(cfg->hotCorners.delayMs) : 0.0;
  syncChildChrome();

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto compactCard = ui::column({
      .configure =
          [scale, opacity](Flex& card) {
            applyCompactCardStyle(card, scale, opacity, false);
          },
  });
  compactCard->addChild(
      ui::segmented({
          .out = &m_sectionSwitch,
          .options =
              [&]() {
                std::vector<ui::SegmentedOption> options{
                    {
                        .label = "",
                        .glyph = "layout-board",
                        .tooltip = i18n::tr("control-center.desktop.widgets-tooltip"),
                    },
                    {
                        .label = "",
                        .glyph = "border-corners",
                        .tooltip = i18n::tr("control-center.desktop.corners-tooltip"),
                    },
                    {
                        .label = "",
                        .glyph = "crop-3-2",
                        .tooltip = i18n::tr("settings.navigation.sections.bar"),
                    },
                    {
                        .label = "",
                        .glyph = "layout-bottombar-inactive",
                        .tooltip = i18n::tr("settings.navigation.sections.dock"),
                    },
                };
                if (compositors::isNiri()) {
                  options.push_back({
                      .label = "",
                      .glyph = "app-window",
                      .tooltip = i18n::tr("settings.navigation.sections.windows"),
                  });
                }
                options.push_back({
                    .label = "",
                    .glyph = "layout-bottombar",
                    .tooltip = i18n::tr("settings.navigation.sections.panels"),
                });
                options.push_back({
                    .label = "",
                    .glyph = "message-circle",
                    .tooltip = i18n::tr("settings.navigation.sections.osd"),
                });
                return options;
              }(),
          .selectedIndex = indexFromSection(m_section),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = opacity,
          .equalSegmentWidths = false,
          .onChange = [this](std::size_t index) { applySection(index); },
          .configure = [](Segmented& sources) { sources.setWrap(true); },
      })
  );

  const bool widgetsOpen = m_section == Section::Widgets;
  auto widgetsToolbar = ui::column({
      .out = &m_widgetsToolbar,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .visible = widgetsOpen,
      .participatesInLayout = widgetsOpen,
  });
  m_widgetsEnabled = addToggleRow(
      *widgetsToolbar, scale, i18n::tr("settings.schema.desktop.widgets.label"),
      [this](bool value) { applyWidgets(value); }
  );
  if (m_widgetsEnabled != nullptr) {
    m_widgetsEnabled->setCheckedImmediate(widgetsEnabled);
  }
  auto browse = ui::row({
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceXs * scale,
      .flexGrow = 1.0F,
  });
  browse->addChild(
      ui::segmented({
          .out = &m_widgetCatalogSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = "",
                      .glyph = "clock",
                      .tooltip = i18n::tr("control-center.desktop.widgets-glanceable-tooltip"),
                  },
                  {
                      .label = "",
                      .glyph = "volume",
                      .tooltip = i18n::tr("control-center.desktop.widgets-sound-tooltip"),
                  },
                  {
                      .label = "",
                      .glyph = "puzzle",
                      .tooltip = i18n::tr("control-center.desktop.widgets-tools-tooltip"),
                  },
              },
          .selectedIndex = indexFromWidgetCatalog(m_widgetCatalog),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = opacity,
          .equalSegmentWidths = false,
          .onChange = [this](std::size_t index) { applyWidgetCatalog(index); },
      })
  );
  browse->addChild(
      ui::row(
          {.align = FlexAlign::Center, .flexGrow = 1.0F},
          ui::input({
              .out = &m_widgetFilter,
              .placeholder = i18n::tr("control-center.desktop.search"),
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeightSm * scale,
              .clearButtonEnabled = true,
              .minLayoutWidth = 128.0F * scale,
              .surfaceOpacity = opacity,
              .height = 0.0F,
              .flexGrow = 1.0F,
              .onChange =
                  [this](const std::string& text) {
                    if (m_widgetQuery == text) {
                      return;
                    }
                    m_widgetQuery = text;
                    rebuildWidgetGrid();
                    PanelManager::instance().requestLayout();
                  },
          })
      )
  );
  browse->addChild(
      ui::label({
          .out = &m_widgetCount,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
      })
  );
  browse->addChild(
      ui::button({
          .out = &m_edit,
          .glyph = "pencil",
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.desktop.edit-tooltip"),
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [this]() { startEdit(); },
      })
  );
  widgetsToolbar->addChild(std::move(browse));
  compactCard->addChild(std::move(widgetsToolbar));

  const bool cornersOpen = m_section == Section::Corners;
  auto cornersToolbar = ui::column({
      .out = &m_cornersToolbar,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .visible = cornersOpen,
      .participatesInLayout = cornersOpen,
  });
  auto addNestedCatalog = [&](
                              Flex& parent,
                              Segmented** switchOut,
                              std::vector<ui::SegmentedOption> options,
                              std::size_t selected,
                              auto onChange
                          ) {
    auto chips = ui::row({
        .align = FlexAlign::Center,
        .wrap = true,
        .gap = Style::spaceXs * scale,
    });
    chips->addChild(
        ui::segmented({
            .out = switchOut,
            .options = std::move(options),
            .selectedIndex = selected,
            .scale = scale,
            .compact = true,
            .surfaceOpacity = opacity,
            .equalSegmentWidths = false,
            .onChange = std::move(onChange),
            .configure =
                [](Segmented& sources) {
                  sources.setWrap(true);
                  sources.setFillWidth(false);
                },
        })
    );
    parent.addChild(std::move(chips));
  };
  addNestedCatalog(
      *cornersToolbar, &m_cornersCatalogSwitch,
      {
          {
              .label = "",
              .glyph = "border-radius",
              .tooltip = i18n::tr("control-center.desktop.screen-corners-tooltip"),
          },
          {
              .label = "",
              .glyph = "click",
              .tooltip = i18n::tr("control-center.desktop.hot-corners-tooltip"),
          },
      },
      indexFromCornersCatalog(m_cornersCatalog), [this](std::size_t index) { applyCornersCatalog(index); }
  );
  compactCard->addChild(std::move(cornersToolbar));

  auto addCatalogToolbar = [&](
                               Flex** out,
                               Segmented** switchOut,
                               bool open,
                               std::vector<ui::SegmentedOption> options,
                               std::size_t selected,
                               auto onChange,
                               auto prefix
                           ) {
    auto toolbar = ui::column({
        .out = out,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .visible = open,
        .participatesInLayout = open,
    });
    prefix(*toolbar);
    addNestedCatalog(*toolbar, switchOut, std::move(options), selected, std::move(onChange));
    compactCard->addChild(std::move(toolbar));
  };
  auto noPrefix = [](Flex&) {};
  addCatalogToolbar(
      &m_barToolbar, &m_barCatalogSwitch, m_section == Section::Bar,
      {
          {.label = "", .glyph = "crop-3-2", .tooltip = i18n::tr("control-center.bar.layout-tooltip")},
          {.label = "", .glyph = "resize", .tooltip = i18n::tr("control-center.bar.shape-tooltip")},
      },
      m_bar != nullptr ? m_bar->currentSectionIndex() : 0,
      [this](std::size_t index) {
        if (m_bar != nullptr) {
          m_bar->applyOpenSection(index);
        }
      },
      noPrefix
  );
  addCatalogToolbar(
      &m_dockToolbar, &m_dockCatalogSwitch, m_section == Section::Dock,
      {
          {
              .label = "",
              .glyph = "layout-bottombar-inactive",
              .tooltip = i18n::tr("control-center.dock.layout-tooltip"),
          },
          {.label = "", .glyph = "pin", .tooltip = i18n::tr("control-center.dock.pinned-tooltip")},
      },
      m_dock != nullptr ? m_dock->currentSectionIndex() : 0,
      [this](std::size_t index) {
        if (m_dock != nullptr) {
          m_dock->applyOpenSection(index);
        }
      },
      noPrefix
  );
  if (compositors::isNiri()) {
    addCatalogToolbar(
        &m_windowsToolbar, &m_windowsCatalogSwitch, m_section == Section::Windows,
        {
            {.label = "", .glyph = "layout-grid", .tooltip = i18n::tr("control-center.windows.spacing-tooltip")},
            {.label = "", .glyph = "layout-board", .tooltip = i18n::tr("control-center.windows.columns-tooltip")},
        },
        m_windows != nullptr ? m_windows->currentSectionIndex() : 0,
        [this](std::size_t index) {
          if (m_windows != nullptr) {
            m_windows->applyOpenSection(index);
          }
        },
        noPrefix
    );
  }
  addCatalogToolbar(
      &m_panelsToolbar, &m_panelsCatalogSwitch, m_section == Section::Panels,
      {
          {
              .label = "",
              .glyph = "layers-intersect",
              .tooltip = i18n::tr("control-center.panels.placement-tooltip"),
          },
          {.label = "", .glyph = "border-all", .tooltip = i18n::tr("control-center.panels.chrome-tooltip")},
      },
      m_panels != nullptr ? m_panels->currentSectionIndex() : 0,
      [this](std::size_t index) {
        if (m_panels != nullptr) {
          m_panels->applyOpenSection(index);
        }
      },
      noPrefix
  );
  addCatalogToolbar(
      &m_osdToolbar, &m_osdCatalogSwitch, m_section == Section::Osd,
      {
          {.label = "", .glyph = "layout-navbar", .tooltip = i18n::tr("control-center.osd.placement-tooltip")},
          {.label = "", .glyph = "contrast", .tooltip = i18n::tr("control-center.osd.style-tooltip")},
      },
      m_osd != nullptr ? m_osd->currentSectionIndex() : 0,
      [this](std::size_t index) {
        if (m_osd != nullptr) {
          m_osd->applyOpenSection(index);
        }
      },
      [this, scale](Flex& toolbar) {
        const Config* liveCfg = m_config != nullptr ? &m_config->config() : nullptr;
        m_osdEnabled = addToggleRow(
            toolbar, scale, i18n::tr("control-center.osd.enable"),
            [this](bool value) {
              if (m_osd != nullptr) {
                m_osd->applyEnabled(value);
              }
              syncHosts();
            }
        );
        if (m_osdEnabled != nullptr) {
          m_osdEnabled->setCheckedImmediate(liveCfg != nullptr && liveCfg->osd.enabled);
        }
      }
  );
  live->addChild(std::move(compactCard));

  auto widgetsHost = ui::column({
      .out = &m_widgetsHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillHeight = true,
      .flexGrow = 1.0F,
      .visible = widgetsOpen,
      .participatesInLayout = widgetsOpen,
  });
  widgetsHost->addChild(
      ui::label({
          .out = &m_widgetEmpty,
          .text = i18n::tr("ui.controls.search-picker.empty"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  auto listScroll = ui::scrollView({
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure =
          [](ScrollView& scrollView) {
            scrollView.clearFill();
            scrollView.clearBorder();
          },
  });
  auto* content = listScroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  auto grid = std::make_unique<GridView>();
  grid->setColumns(2);
  grid->setColumnGap(Style::spaceSm * scale);
  grid->setRowGap(Style::spaceSm * scale);
  grid->setStretchItems(true);
  m_widgetGrid = grid.get();
  content->addChild(std::move(grid));
  widgetsHost->addChild(std::move(listScroll));
  live->addChild(std::move(widgetsHost));

  auto leftoverWell = [&](Flex** out, bool open, auto&& fill) {
    auto host = ui::column({
        .out = out,
        .align = FlexAlign::Stretch,
        .gap = 0.0F,
        .flexGrow = 1.0F,
        .visible = open,
        .participatesInLayout = open,
        .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
    });
    auto scroll = ui::scrollView({
        .contentScale = scale,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .flexGrow = 1.0F,
        .configure =
            [](ScrollView& scrollView) {
              scrollView.clearFill();
              scrollView.clearBorder();
            },
    });
    auto* leftover = scroll->content();
    leftover->setDirection(FlexDirection::Vertical);
    leftover->setAlign(FlexAlign::Stretch);
    leftover->setGap(Style::spaceSm * scale);
    fill(*leftover);
    host->addChild(std::move(scroll));
    live->addChild(std::move(host));
  };

  const bool screenOpen = m_section == Section::Corners && m_cornersCatalog == CornersCatalog::Screen;
  const bool hotOpen = m_section == Section::Corners && m_cornersCatalog == CornersCatalog::Hot;
  leftoverWell(&m_screenHost, screenOpen, [&](Flex& leftover) {
    m_screenCorners = addToggleRow(
        leftover, scale, i18n::tr("settings.schema.desktop.screen-corners-enabled.label"),
        [this](bool value) { applyScreenCorners(value); }
    );
    if (m_screenCorners != nullptr) {
      m_screenCorners->setCheckedImmediate(cornersEnabled);
    }
    auto screenDetails = ui::column({
        .out = &m_screenDetails,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .visible = cornersEnabled,
        .participatesInLayout = cornersEnabled,
    });
    addSliderSection(
        *screenDetails, scale, i18n::tr("settings.schema.desktop.screen-corners-size.label"), &m_cornerSize,
        &m_cornerSizeValue, sizeMin, sizeMax, sizeStep, cornerSize, pixelLabel, [this]() { commitScreenCornerSize(); }
    );
    leftover.addChild(std::move(screenDetails));
  });

  leftoverWell(&m_hotHost, hotOpen, [&](Flex& leftover) {
    m_hotCorners = addToggleRow(
        leftover, scale, i18n::tr("settings.schema.desktop.hot-corners-enabled.label"),
        [this](bool value) { applyHotCorners(value); }
    );
    if (m_hotCorners != nullptr) {
      m_hotCorners->setCheckedImmediate(hotEnabled);
    }
    auto hotDetails = ui::column({
        .out = &m_hotDetails,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .visible = hotEnabled,
        .participatesInLayout = hotEnabled,
    });
    addSliderSection(
        *hotDetails, scale, i18n::tr("settings.schema.desktop.hot-corners-delay-ms.label"), &m_hotDelay,
        &m_hotDelayValue, delayMin, delayMax, delayStep, hotDelay, delayLabel, [this]() { commitHotCornerDelay(); }
    );
    const auto actions = hotCornerActionLabels();
    m_topLeft = addSelectRow(
        *hotDetails, scale, i18n::tr("settings.schema.desktop.hot-corners-top-left.label"), actions, opacity,
        [this](std::size_t index) { applyHotCornerAction("top_left", index); }
    );
    m_topRight = addSelectRow(
        *hotDetails, scale, i18n::tr("settings.schema.desktop.hot-corners-top-right.label"), actions, opacity,
        [this](std::size_t index) { applyHotCornerAction("top_right", index); }
    );
    m_bottomLeft = addSelectRow(
        *hotDetails, scale, i18n::tr("settings.schema.desktop.hot-corners-bottom-left.label"), actions, opacity,
        [this](std::size_t index) { applyHotCornerAction("bottom_left", index); }
    );
    m_bottomRight = addSelectRow(
        *hotDetails, scale, i18n::tr("settings.schema.desktop.hot-corners-bottom-right.label"), actions, opacity,
        [this](std::size_t index) { applyHotCornerAction("bottom_right", index); }
    );
    leftover.addChild(std::move(hotDetails));
  });

  auto addHost = [&]<typename Hosted>(Flex** out, Hosted* child) {
    leftoverWell(out, false, [&](Flex& leftover) { leftover.addChild(child->createLiveBody(true)); });
  };
  addHost(&m_barHost, m_bar.get());
  addHost(&m_dockHost, m_dock.get());
  addHost(&m_windowsHost, m_windows.get());
  addHost(&m_panelsHost, m_panels.get());
  addHost(&m_osdHost, m_osd.get());

  rebuildWidgetGrid();
  syncHosts();
  auto wrapped = m_settings->wrapLive(std::move(live), scale, opacity);
  syncLiveControls();
  return wrapped;
}

std::unique_ptr<Flex> DesktopTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void DesktopTab::onClose() {
  if (m_bar != nullptr) {
    m_bar->onClose();
  }
  if (m_dock != nullptr) {
    m_dock->onClose();
  }
  if (m_windows != nullptr) {
    m_windows->onClose();
  }
  if (m_panels != nullptr) {
    m_panels->onClose();
  }
  if (m_osd != nullptr) {
    m_osd->onClose();
  }
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_sectionSwitch = nullptr;
  m_widgetsToolbar = nullptr;
  m_cornersToolbar = nullptr;
  m_barToolbar = nullptr;
  m_dockToolbar = nullptr;
  m_windowsToolbar = nullptr;
  m_panelsToolbar = nullptr;
  m_osdToolbar = nullptr;
  m_widgetsHost = nullptr;
  m_screenHost = nullptr;
  m_hotHost = nullptr;
  m_barHost = nullptr;
  m_dockHost = nullptr;
  m_windowsHost = nullptr;
  m_panelsHost = nullptr;
  m_osdHost = nullptr;
  m_screenDetails = nullptr;
  m_hotDetails = nullptr;
  m_widgetsEnabled = nullptr;
  m_osdEnabled = nullptr;
  m_screenCorners = nullptr;
  m_hotCorners = nullptr;
  m_cornerSize = nullptr;
  m_cornerSizeValue = nullptr;
  m_hotDelay = nullptr;
  m_hotDelayValue = nullptr;
  m_topLeft = nullptr;
  m_topRight = nullptr;
  m_bottomLeft = nullptr;
  m_bottomRight = nullptr;
  m_widgetCount = nullptr;
  m_widgetFilter = nullptr;
  m_widgetGrid = nullptr;
  m_widgetEmpty = nullptr;
  m_edit = nullptr;
  m_widgetCatalogSwitch = nullptr;
  m_cornersCatalogSwitch = nullptr;
  m_barCatalogSwitch = nullptr;
  m_dockCatalogSwitch = nullptr;
  m_windowsCatalogSwitch = nullptr;
  m_panelsCatalogSwitch = nullptr;
  m_osdCatalogSwitch = nullptr;
  m_widgetTiles.clear();
  m_tileTypes.clear();
  m_lastGridKey.clear();
}

void DesktopTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool DesktopTab::configureMode() const { return m_settings->configureMode(); }

void DesktopTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

SettingsPane* DesktopTab::settingsPane() { return m_settings.get(); }

void DesktopTab::setActive(bool active) {
  if (m_bar != nullptr) {
    m_bar->setActive(active && m_section == Section::Bar);
  }
  if (m_dock != nullptr) {
    m_dock->setActive(active && m_section == Section::Dock);
  }
  if (m_windows != nullptr) {
    m_windows->setActive(active && m_section == Section::Windows);
  }
  if (m_panels != nullptr) {
    m_panels->setActive(active && m_section == Section::Panels);
  }
  if (m_osd != nullptr) {
    m_osd->setActive(active && m_section == Section::Osd);
  }
}

void DesktopTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  if (m_widgetGrid != nullptr) {
    const float scale = contentScale();
    const std::size_t columns =
        leftoverCatalogColumns(contentWidth, 148.0F * scale, Style::spaceSm * scale, 2, 5);
    if (m_widgetGrid->columns() != columns) {
      m_widgetGrid->setColumns(columns);
    }
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  if (Tab* child = hostedTab(); child != nullptr) {
    Flex* host = hostNode();
    float hostWidth = contentWidth;
    float hostHeight = bodyHeight;
    if (host != nullptr && host->width() > 1.0F) {
      hostWidth = std::max(1.0F, host->width() - host->paddingLeft() - host->paddingRight());
    }
    if (host != nullptr && host->height() > 1.0F) {
      hostHeight = std::max(1.0F, host->height() - host->paddingTop() - host->paddingBottom());
    }
    child->layout(renderer, hostWidth, hostHeight);
  }
}

void DesktopTab::doUpdate(Renderer& renderer) {
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (Tab* child = hostedTab(); child != nullptr) {
    child->update(renderer);
    if (m_liveDirty) {
      m_liveDirty = false;
      syncLiveControls();
    }
    return;
  }
  (void)renderer;
  if (m_liveDirty) {
    m_liveDirty = false;
    rebuildWidgetGrid();
    syncLiveControls();
  }
}

void DesktopTab::onPanelCardOpacityChanged(float opacity) {
  if (m_sectionSwitch != nullptr) {
    m_sectionSwitch->setSurfaceOpacity(opacity);
  }
  if (m_widgetFilter != nullptr) {
    m_widgetFilter->setSurfaceOpacity(opacity);
  }
  if (m_widgetCatalogSwitch != nullptr) {
    m_widgetCatalogSwitch->setSurfaceOpacity(opacity);
  }
  if (m_cornersCatalogSwitch != nullptr) {
    m_cornersCatalogSwitch->setSurfaceOpacity(opacity);
  }
  if (m_barCatalogSwitch != nullptr) {
    m_barCatalogSwitch->setSurfaceOpacity(opacity);
  }
  if (m_dockCatalogSwitch != nullptr) {
    m_dockCatalogSwitch->setSurfaceOpacity(opacity);
  }
  if (m_windowsCatalogSwitch != nullptr) {
    m_windowsCatalogSwitch->setSurfaceOpacity(opacity);
  }
  if (m_panelsCatalogSwitch != nullptr) {
    m_panelsCatalogSwitch->setSurfaceOpacity(opacity);
  }
  if (m_osdCatalogSwitch != nullptr) {
    m_osdCatalogSwitch->setSurfaceOpacity(opacity);
  }
  if (m_topLeft != nullptr) {
    m_topLeft->setSurfaceOpacity(opacity);
  }
  if (m_topRight != nullptr) {
    m_topRight->setSurfaceOpacity(opacity);
  }
  if (m_bottomLeft != nullptr) {
    m_bottomLeft->setSurfaceOpacity(opacity);
  }
  if (m_bottomRight != nullptr) {
    m_bottomRight->setSurfaceOpacity(opacity);
  }
  syncChildChrome();
}

void DesktopTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }

  const Config& cfg = m_config->config();
  const bool widgetsOn = cfg.desktopWidgets.enabled;
  m_syncing = true;
  if (m_widgetsEnabled != nullptr) {
    m_widgetsEnabled->setCheckedImmediate(widgetsOn);
  }
  if (m_osdEnabled != nullptr) {
    m_osdEnabled->setCheckedImmediate(cfg.osd.enabled);
  }
  if (m_screenCorners != nullptr) {
    m_screenCorners->setCheckedImmediate(cfg.shell.screenCorners.enabled);
  }
  if (setShown(m_screenDetails, cfg.shell.screenCorners.enabled)) {
    PanelManager::instance().requestLayout();
  }
  if (m_hotCorners != nullptr) {
    m_hotCorners->setCheckedImmediate(cfg.hotCorners.enabled);
  }
  if (setShown(m_hotDetails, cfg.hotCorners.enabled)) {
    PanelManager::instance().requestLayout();
  }
  if (m_cornerSize != nullptr) {
    m_cornerSize->setValue(static_cast<double>(cfg.shell.screenCorners.size));
  }
  if (m_cornerSizeValue != nullptr) {
    m_cornerSizeValue->setText(pixelLabel(static_cast<double>(cfg.shell.screenCorners.size)));
  }
  if (m_hotDelay != nullptr) {
    m_hotDelay->setValue(static_cast<double>(cfg.hotCorners.delayMs));
  }
  if (m_hotDelayValue != nullptr) {
    m_hotDelayValue->setText(delayLabel(static_cast<double>(cfg.hotCorners.delayMs)));
  }
  if (m_topLeft != nullptr) {
    m_topLeft->setSelectedIndexSilently(hotCornerActionIndex(cfg.hotCorners.topLeft.action));
  }
  if (m_topRight != nullptr) {
    m_topRight->setSelectedIndexSilently(hotCornerActionIndex(cfg.hotCorners.topRight.action));
  }
  if (m_bottomLeft != nullptr) {
    m_bottomLeft->setSelectedIndexSilently(hotCornerActionIndex(cfg.hotCorners.bottomLeft.action));
  }
  if (m_bottomRight != nullptr) {
    m_bottomRight->setSelectedIndexSilently(hotCornerActionIndex(cfg.hotCorners.bottomRight.action));
  }
  if (m_widgetCount != nullptr) {
    if (!widgetsOn) {
      m_widgetCount->setText(i18n::tr("control-center.desktop.off"));
    } else {
      const std::size_t count = enabledWidgetCount(cfg.desktopWidgets);
      if (count == 0) {
        m_widgetCount->setText(i18n::tr("control-center.desktop.empty"));
      } else {
        m_widgetCount->setText(i18n::trp("control-center.desktop.widget-count", static_cast<long>(count)));
      }
    }
  }
  for (std::size_t i = 0; i < m_widgetTiles.size() && i < m_tileTypes.size(); ++i) {
    if (m_widgetTiles[i] != nullptr) {
      m_widgetTiles[i]->setSelected(enabledCountForType(cfg.desktopWidgets, m_tileTypes[i]) > 0);
    }
  }
  m_syncing = false;
}

void DesktopTab::syncHosts() {
  const bool widgetsOpen = m_section == Section::Widgets;
  const bool cornersOpen = m_section == Section::Corners;
  const bool screenOpen = cornersOpen && m_cornersCatalog == CornersCatalog::Screen;
  const bool hotOpen = cornersOpen && m_cornersCatalog == CornersCatalog::Hot;
  setShown(m_widgetsToolbar, widgetsOpen);
  setShown(m_cornersToolbar, cornersOpen);
  setShown(m_barToolbar, m_section == Section::Bar);
  setShown(m_dockToolbar, m_section == Section::Dock);
  setShown(m_windowsToolbar, m_section == Section::Windows);
  setShown(m_panelsToolbar, m_section == Section::Panels);
  setShown(m_osdToolbar, m_section == Section::Osd);
  setShown(m_widgetsHost, widgetsOpen);
  setShown(m_screenHost, screenOpen);
  setShown(m_hotHost, hotOpen);
  setShown(m_barHost, m_section == Section::Bar);
  setShown(m_dockHost, m_section == Section::Dock);
  setShown(m_windowsHost, m_section == Section::Windows);
  setShown(m_panelsHost, m_section == Section::Panels);
  setShown(m_osdHost, m_section == Section::Osd && m_config != nullptr && m_config->config().osd.enabled);
}

void DesktopTab::syncSettingsSection() {
  if (m_settings == nullptr) {
    return;
  }
  if (m_section == Section::Corners) {
    m_settings->setSection("desktop");
    if (m_cornersCatalog == CornersCatalog::Hot) {
      m_settings->setIncludedGroups({"hot-corners"});
      return;
    }
    m_settings->setIncludedGroups({"screen-corners"});
    return;
  }
  if (m_section == Section::Widgets) {
    m_settings->setSection("desktop");
    m_settings->setIncludedGroups({"widgets"});
    return;
  }
  m_settings->setSection(std::string(idFromSection(m_section)));
  m_settings->setIncludedGroups({});
}

void DesktopTab::syncChildChrome() {
  const float scale = contentScale();
  const float opacity = panelCardOpacity();
  const auto apply = [scale, opacity](Tab* child) {
    if (child == nullptr) {
      return;
    }
    child->setContentScale(scale);
    child->setPanelCardOpacity(opacity);
  };
  apply(m_bar.get());
  apply(m_dock.get());
  apply(m_windows.get());
  apply(m_panels.get());
  apply(m_osd.get());
}

Tab* DesktopTab::hostedTab() const noexcept {
  switch (m_section) {
  case Section::Bar:
    return m_bar.get();
  case Section::Dock:
    return m_dock.get();
  case Section::Windows:
    return m_windows.get();
  case Section::Panels:
    return m_panels.get();
  case Section::Osd:
    return m_osd.get();
  case Section::Widgets:
  case Section::Corners:
  default:
    return nullptr;
  }
}

Flex* DesktopTab::hostNode() const noexcept {
  switch (m_section) {
  case Section::Bar:
    return m_barHost;
  case Section::Dock:
    return m_dockHost;
  case Section::Windows:
    return m_windowsHost;
  case Section::Panels:
    return m_panelsHost;
  case Section::Osd:
    return m_osdHost;
  case Section::Widgets:
  case Section::Corners:
  default:
    return nullptr;
  }
}

void DesktopTab::persistSection() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("desktop_tab", "section", std::string(idFromSection(m_section)));
}

void DesktopTab::persistWidgetCatalog() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("desktop_tab", "widgets", std::string(idFromWidgetCatalog(m_widgetCatalog)));
}

void DesktopTab::persistCornersCatalog() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("desktop_tab", "corners", std::string(idFromCornersCatalog(m_cornersCatalog)));
}

void DesktopTab::rebuildWidgetGrid() {
  if (m_widgetGrid == nullptr) {
    return;
  }
  uiAssertNotRendering("DesktopTab::rebuildWidgetGrid");

  const auto options = desktop_settings::desktopWidgetTypeOptions();
  const std::string needle = StringUtils::toLower(StringUtils::trim(m_widgetQuery));
  std::vector<desktop_settings::DesktopWidgetTypeOption> entries;
  entries.reserve(options.size());
  for (const auto& option : options) {
    if (!widgetTypeInCatalog(option.value, m_widgetCatalog)) {
      continue;
    }
    if (!needle.empty() && StringUtils::toLower(option.label).find(needle) == std::string::npos
        && StringUtils::toLower(option.value).find(needle) == std::string::npos) {
      continue;
    }
    entries.push_back(option);
  }

  std::string gridKey = std::string(idFromWidgetCatalog(m_widgetCatalog)) + '|' + needle + '|';
  for (const auto& entry : entries) {
    gridKey += entry.value;
    gridKey += ',';
  }
  const DesktopWidgetsConfig widgets =
      m_config != nullptr ? m_config->config().desktopWidgets : DesktopWidgetsConfig{};
  for (const auto& entry : entries) {
    gridKey += std::to_string(enabledCountForType(widgets, entry.value));
    gridKey += ',';
  }
  if (gridKey == m_lastGridKey) {
    return;
  }
  m_lastGridKey = std::move(gridKey);

  const bool empty = entries.empty();
  setShown(m_widgetEmpty, empty);
  if (m_widgetGrid != nullptr) {
    m_widgetGrid->setVisible(!empty);
    m_widgetGrid->setParticipatesInLayout(!empty);
  }
  while (!m_widgetGrid->children().empty()) {
    m_widgetGrid->removeChild(m_widgetGrid->children().back().get());
  }

  m_widgetTiles.clear();
  m_tileTypes.clear();
  m_widgetTiles.reserve(entries.size());
  m_tileTypes.reserve(entries.size());

  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;
  for (const auto& entry : entries) {
    const std::size_t count = enabledCountForType(widgets, entry.value);
    std::string caption;
    if (count == 0) {
      caption = i18n::tr("control-center.desktop.add");
    } else {
      caption = i18n::trp("control-center.desktop.on-desktop", static_cast<long>(count));
    }
    Button* button = nullptr;
    auto tile = ui::button({
        .out = &button,
        .text = entry.label,
        .glyph = std::string(glyphForWidgetType(entry.value)),
        .fontSize = Style::fontSizeBody * scale,
        .selected = count > 0,
        .contentAlign = ButtonContentAlign::Start,
        .customPalette = catalogTilePalette(),
        .tooltip = i18n::tr("control-center.desktop.add-tooltip"),
        .padding = Style::spaceSm * scale,
        .gap = Style::spaceXs * scale,
        .radius = Style::scaledRadiusXl(scale),
        .onClick = [this, type = entry.value]() { addWidgetType(type); },
        .configure =
            [captionSize, caption](Button& host) {
              host.setDirection(FlexDirection::Vertical);
              host.setAlign(FlexAlign::Stretch);
              host.setJustify(FlexJustify::Center);
              if (host.label() != nullptr) {
                host.label()->setMaxLines(1);
                host.label()->setEllipsize(TextEllipsize::End);
                host.label()->setFontWeight(FontWeight::Medium);
              }
              host.addChild(
                  ui::label({
                      .text = caption,
                      .fontSize = captionSize,
                      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                      .maxLines = 1,
                      .ellipsize = TextEllipsize::End,
                  })
              );
            },
    });
    m_widgetTiles.push_back(button);
    m_tileTypes.push_back(entry.value);
    m_widgetGrid->addChild(std::move(tile));
  }
}

void DesktopTab::applySection(std::size_t index) {
  if (m_sectionSyncing) {
    return;
  }
  applyOpenSection(sectionFromIndex(index));
}

void DesktopTab::applyOpenSection(Section section, bool persist) {
  if (section == Section::Windows && !compositors::isNiri()) {
    section = Section::Widgets;
  }
  if (m_section != section) {
    m_section = section;
    if (persist) {
      persistSection();
    }
    if (m_sectionSwitch != nullptr) {
      m_sectionSyncing = true;
      m_sectionSwitch->setSelectedIndex(indexFromSection(m_section));
      m_sectionSyncing = false;
    }
  }
  syncHosts();
  syncSettingsSection();
  setActive(true);
  PanelManager::instance().requestLayout();
}

void DesktopTab::applyWidgetCatalog(std::size_t index) {
  if (m_widgetCatalogSyncing) {
    return;
  }
  const WidgetCatalog catalog = widgetCatalogFromIndex(index);
  if (m_widgetCatalog == catalog) {
    return;
  }
  m_widgetCatalog = catalog;
  persistWidgetCatalog();
  rebuildWidgetGrid();
  PanelManager::instance().requestLayout();
}

void DesktopTab::applyCornersCatalog(std::size_t index) {
  if (m_cornersCatalogSyncing) {
    return;
  }
  const CornersCatalog catalog = cornersCatalogFromIndex(index);
  if (m_cornersCatalog == catalog) {
    syncHosts();
    syncSettingsSection();
    return;
  }
  m_cornersCatalog = catalog;
  persistCornersCatalog();
  if (m_cornersCatalogSwitch != nullptr) {
    m_cornersCatalogSyncing = true;
    m_cornersCatalogSwitch->setSelectedIndex(indexFromCornersCatalog(m_cornersCatalog));
    m_cornersCatalogSyncing = false;
  }
  syncHosts();
  syncSettingsSection();
  syncLiveControls();
  PanelManager::instance().requestLayout();
}

void DesktopTab::applyOpenContext(std::string_view context, bool persist) {
  if (context.empty() || context == "desktop") {
    return;
  }
  applyOpenSection(sectionFromId(context), persist);
  if (m_section != Section::Corners) {
    return;
  }
  if (context == "corners") {
    return;
  }
  const CornersCatalog catalog = cornersCatalogFromId(context);
  if (m_cornersCatalog == catalog) {
    return;
  }
  m_cornersCatalog = catalog;
  if (persist) {
    persistCornersCatalog();
  }
  if (m_cornersCatalogSwitch != nullptr) {
    m_cornersCatalogSyncing = true;
    m_cornersCatalogSwitch->setSelectedIndex(indexFromCornersCatalog(m_cornersCatalog));
    m_cornersCatalogSyncing = false;
  }
  syncHosts();
  syncSettingsSection();
  syncLiveControls();
  PanelManager::instance().requestLayout();
}

bool DesktopTab::matchesOpenContext(std::string_view context) const noexcept {
  if (context.empty() || context == "desktop") {
    return true;
  }
  if (context == "widgets") {
    return m_section == Section::Widgets;
  }
  if (context == "corners") {
    return m_section == Section::Corners;
  }
  if (context == "hot-corners" || context == "hot_corners") {
    return m_section == Section::Corners && m_cornersCatalog == CornersCatalog::Hot;
  }
  if (context == "screen-corners" || context == "screen_corners") {
    return m_section == Section::Corners && m_cornersCatalog == CornersCatalog::Screen;
  }
  return m_section == sectionFromId(context);
}

void DesktopTab::applyWidgets(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"desktop_widgets", "enabled"}, enabled);
  syncLiveControls();
  PanelManager::instance().refresh();
}

void DesktopTab::applyScreenCorners(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "screen_corners", "enabled"}, enabled);
  syncLiveControls();
  PanelManager::instance().refresh();
}

void DesktopTab::applyHotCorners(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"hot_corners", "enabled"}, enabled);
  syncLiveControls();
  PanelManager::instance().refresh();
}

void DesktopTab::commitScreenCornerSize() {
  if (m_syncing || m_config == nullptr || m_cornerSize == nullptr) {
    return;
  }
  (void)m_config->setOverride(
      {"shell", "screen_corners", "size"}, static_cast<std::int64_t>(std::lround(m_cornerSize->value()))
  );
}

void DesktopTab::commitHotCornerDelay() {
  if (m_syncing || m_config == nullptr || m_hotDelay == nullptr) {
    return;
  }
  (void)m_config->setOverride(
      {"hot_corners", "delay_ms"}, static_cast<std::int64_t>(std::lround(m_hotDelay->value()))
  );
}

void DesktopTab::applyHotCornerAction(std::string_view corner, std::size_t index) {
  if (m_syncing || m_config == nullptr || index >= kHotCornerActions.size()) {
    return;
  }
  (void)m_config->setOverride(
      {"hot_corners", std::string(corner), "action"}, std::string(kHotCornerActions[index])
  );
}

void DesktopTab::addWidgetType(std::string type) {
  if (m_widgets == nullptr || type.empty()) {
    return;
  }
  const std::optional<std::string> added = m_widgets->addWidget(type);
  if (!added.has_value()) {
    return;
  }
  m_lastGridKey.clear();
  rebuildWidgetGrid();
  syncLiveControls();
  PanelManager::instance().refresh();
  if (!added->empty() && (type == "folder" || type == "file")) {
    chooseWidgetPath(*added, type == "folder");
  }
}

void DesktopTab::chooseWidgetPath(std::string widgetId, bool folder) {
  if (m_widgets == nullptr || widgetId.empty()) {
    return;
  }
  FileDialogOptions options;
  options.mode = folder ? FileDialogMode::SelectFolder : FileDialogMode::Open;
  options.title = i18n::tr(
      folder ? "desktop-widgets.editor.dialogs.select-folder" : "desktop-widgets.editor.dialogs.select-file"
  );
  const bool opened = FileDialog::open(
      std::move(options), [this, widgetId](std::optional<std::filesystem::path> result) {
        if (m_widgets == nullptr) {
          return;
        }
        if (!result.has_value()) {
          (void)m_widgets->removeWidget(widgetId);
        } else {
          (void)m_widgets->setWidgetSetting(widgetId, "path", result->string());
        }
        m_lastGridKey.clear();
        rebuildWidgetGrid();
        PanelManager::instance().refresh();
      }
  );
  if (!opened) {
    (void)m_widgets->removeWidget(widgetId);
  }
}

void DesktopTab::startEdit() {
  if (m_config != nullptr && !m_config->config().desktopWidgets.enabled) {
    (void)m_config->setOverride({"desktop_widgets", "enabled"}, true);
  }
  if (m_widgets != nullptr) {
    m_widgets->enterEdit();
  }
}
