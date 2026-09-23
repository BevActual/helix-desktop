#include "shell/control_center/tabs/system_tab.h"

#include "config/config_service.h"
#include "core/build_info.h"
#include "i18n/i18n.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "system/distro_info.h"
#include "system/format_units.h"
#include "system/hardware_info.h"
#include "system/system_monitor_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/graph.h"
#include "ui/controls/segmented.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <format>
#include <optional>
#include <vector>

using namespace control_center;

namespace {

  constexpr float kGraphLineWidth = 0.75F;
  // Content width (logical px) from which the About well shows System and Resources side by side.
  constexpr float kAboutTwoColumnMinWidth = 720.0F;
  constexpr float kGraphFillOpacity = 0.15F;
  constexpr double kNetMinScaleBps = 10000.0;
  constexpr std::size_t kMaxDiskRows = 4;

  [[nodiscard]] bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return changed;
  }

  void requestPanelLayout() {
    if (auto* panels = PanelManager::current(); panels != nullptr) {
      panels->requestLayout();
    }
  }

  // Graph leftover fills remaining height. Keep a little gap under the plot for the legend.

  Flex* makeIconLabel(Flex& parent, const char* glyphName, float scale, Glyph** outIcon = nullptr) {
    Flex* ptr = nullptr;
    auto group = ui::row(
        {.out = &ptr, .align = FlexAlign::Center, .gap = Style::spaceXs * scale},
        ui::glyph({
            .out = outIcon,
            .glyph = glyphName,
            .glyphSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    parent.addChild(std::move(group));
    return ptr;
  }

  Label* makeValueLabel(Flex& parent, float scale) {
    Label* ptr = nullptr;
    parent.addChild(
        ui::label({
            .out = &ptr,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    return ptr;
  }

  // Legend row sits below the graph and centers its icon+value groups so wide
  // stats get the full card width instead of competing with the title.
  Flex* makeLegendRow(Flex& parent, float scale) {
    Flex* ptr = nullptr;
    auto row = ui::row({
        .out = &ptr,
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .gap = Style::spaceMd * scale,
    });
    parent.addChild(std::move(row));
    return ptr;
  }

  Graph* addGraph(Flex& parent) {
    auto graph = std::make_unique<Graph>();
    graph->setFillOpacity(kGraphFillOpacity);
    auto* ptr = graph.get();
    parent.addChild(std::move(graph));
    return ptr;
  }

  std::string formatMemoryUsedTotal(const SystemStats& stats) {
    if (stats.ramTotalMb == 0) {
      return memoryTotalLabel();
    }
    return FormatUnits::formatBinaryMibUsageAsGib(stats.ramUsedMb, stats.ramTotalMb);
  }

  std::string formatGpuVramUsed(const SystemStats& stats) {
    if (!stats.gpuVramUsedBytes.has_value()) {
      return "--";
    }
    return FormatUnits::formatBinaryBytesAsGib(*stats.gpuVramUsedBytes);
  }

  // Resource row: glyph + name (grows, left) + value (natural width, right-aligned).
  // A non-empty tooltip is attached to the row's text (labels opt out of hit-testing by default).
  Label* addResourceRow(
      Flex& card, const char* glyphName, const std::string& name, float scale, Flex** outRow = nullptr,
      const std::string& tooltip = {}
  ) {
    Label* value = nullptr;
    Label* nameLabel = nullptr;
    Flex* row = nullptr;
    card.addChild(
        ui::row(
            {.out = &row, .align = FlexAlign::Center, .gap = Style::spaceXs * scale},
            ui::glyph({
                .glyph = glyphName,
                .glyphSize = Style::fontSizeCaption * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            }),
            ui::label({
                .out = &nameLabel,
                .text = name,
                .fontSize = Style::fontSizeCaption * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .maxLines = 1,
                .flexGrow = 1.0F,
            }),
            ui::label({
                .out = &value,
                .fontSize = Style::fontSizeCaption * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .maxLines = 1,
            })
        )
    );
    if (!tooltip.empty()) {
      for (Label* label : {nameLabel, value}) {
        if (label != nullptr) {
          label->setTooltip(tooltip);
        }
      }
    }
    if (outRow != nullptr) {
      *outRow = row;
    }
    return value;
  }

  Flex* makeInfoCard(
      Flex& parent, const std::string& title, float scale, float grow, float fillOpacity, Label** outLines,
      int lineCount, const char* const* glyphs
  ) {
    // Stretch so each row spans the card: names sit left, values right, instead of every row
    // shrinking to its text and floating centred in a wide well.
    auto card = ui::column({
        .align = FlexAlign::Stretch,
        .flexGrow = grow,
        .configure =
            [scale, fillOpacity](Flex& section) {
              (void)fillOpacity;
              section.setGap(Style::spaceXs * scale);
            },
    });

    addTitle(*card, title, scale);

    for (int i = 0; i < lineCount; ++i) {
      card->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
              ui::glyph({
                  .glyph = glyphs[i],
                  .glyphSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              }),
              ui::label({
                  .out = &outLines[i],
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .maxLines = 1,
                  .flexGrow = 1.0F,
              })
          )
      );
    }

    auto* ptr = card.get();
    parent.addChild(std::move(card));
    return ptr;
  }

} // namespace

SystemTab::SystemTab(SystemMonitorService* monitor, const ControlCenterServices& services)
    : m_monitor(monitor), m_clipboard(services.clipboard), m_config(services.config),
      m_upgradeShell(services.upgradeShell), m_upgradeAvailable(services.upgradeAvailable),
      m_upgradeBusy(services.upgradeBusy),
      m_settings(std::make_unique<SettingsPane>(
          services,
          SettingsPaneOptions{
              .section = "system",
              .includedGroups = {"battery", "monitor", "monitor-polling", "monitor-thresholds"},
          }
      )) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> catalog = m_config->stateString("system_tab", "catalog")) {
      m_catalog = catalogFromState(*catalog);
      m_pinnedCatalog = m_catalog;
    }
  }
}

SystemTab::~SystemTab() { retainStats(false); }

std::unique_ptr<Flex> SystemTab::create() {
  const float sc = contentScale();

  auto tab = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * sc,
      .flexGrow = 1.0F,
  });

  auto compactCard = ui::column({
      .out = &m_compactCard,
      .configure = [sc, opacity = panelCardOpacity()](Flex& card) { applyCompactCardStyle(card, sc, opacity, false); },
  });
  compactCard->addChild(
      ui::label({
          .out = &m_titleLabel,
          .text = distroLabel(),
          .fontSize = Style::fontSizeBody * sc,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .flexGrow = 1.0F,
      })
  );
  auto glance = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceLg * sc,
  });
  glance->addChild(
      ui::label({
          .out = &m_glanceCpuLabel,
          .text = "--%",
          .fontSize = Style::fontSizeTitle * sc,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Primary),
          .maxLines = 1,
      })
  );
  glance->addChild(
      ui::label({
          .out = &m_glanceRamLabel,
          .text = "--",
          .fontSize = Style::fontSizeBody * sc,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .flexGrow = 1.0F,
      })
  );
  compactCard->addChild(std::move(glance));
  compactCard->addChild(
      ui::segmented({
          .out = &m_catalogSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = "",
                      .glyph = "cpu-usage",
                      .tooltip = i18n::tr("control-center.system.titles.cpu"),
                  },
                  {
                      .label = "",
                      .glyph = "memory",
                      .tooltip = i18n::tr("control-center.system.titles.memory"),
                  },
                  {
                      .label = "",
                      .glyph = "video",
                      .tooltip = i18n::tr("control-center.system.titles.gpu"),
                  },
                  {
                      .label = "",
                      .glyph = "network",
                      .tooltip = i18n::tr("control-center.system.titles.network"),
                  },
                  {
                      .label = "",
                      .glyph = "list-details",
                      .tooltip = i18n::tr("control-center.system.titles.about"),
                  },
              },
          .selectedIndex = indexFromCatalog(m_catalog),
          .scale = sc,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .onChange = [this](std::size_t index) { applyCatalog(index, true); },
      })
  );
  compactCard->addChild(
      ui::label({
          .out = &m_detailLabel,
          .fontSize = Style::fontSizeCaption * sc,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
      })
  );
  tab->addChild(std::move(compactCard));

  auto addGraphHost = [&](Flex** hostOut, Graph** graphOut, Flex** legendOut, auto buildLegend) {
    auto host = ui::column({
        .out = hostOut,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * sc,
        .flexGrow = 1.0F,
        .configure =
            [sc](Flex& column) {
              applyContentCardStyle(column, sc);
              column.setClipChildren(true);
            },
    });
    *graphOut = addGraph(*host);
    auto* legend = makeLegendRow(*host, sc);
    *legendOut = legend;
    buildLegend(*legend);
    tab->addChild(std::move(host));
  };

  addGraphHost(&m_cpuHost, &m_cpuGraph, &m_cpuLegend, [&](Flex& legend) {
    auto* cpuPctGroup = makeIconLabel(legend, "cpu-usage", sc, &m_cpuPctIcon);
    m_cpuPctLabel = makeValueLabel(*cpuPctGroup, sc);
    auto* cpuTempGroup = makeIconLabel(legend, "cpu-temperature", sc, &m_cpuTempIcon);
    m_cpuTempLabel = makeValueLabel(*cpuTempGroup, sc);
  });
  addGraphHost(&m_ramHost, &m_ramGraph, &m_ramLegend, [&](Flex& legend) {
    auto* ramGroup = makeIconLabel(legend, "memory", sc, &m_ramIcon);
    m_ramLabel = makeValueLabel(*ramGroup, sc);
  });
  addGraphHost(&m_gpuHost, &m_gpuGraph, &m_gpuLegend, [&](Flex& legend) {
    m_gpuUsageGroup = makeIconLabel(legend, "gpu-usage", sc, &m_gpuUsageIcon);
    m_gpuUsageLabel = makeValueLabel(*m_gpuUsageGroup, sc);
    (void)setShown(m_gpuUsageGroup, false);
    m_gpuVramGroup = makeIconLabel(legend, "memory", sc, &m_gpuVramIcon);
    m_gpuVramLabel = makeValueLabel(*m_gpuVramGroup, sc);
    (void)setShown(m_gpuVramGroup, false);
    m_gpuTempGroup = makeIconLabel(legend, "temperature", sc, &m_gpuTempIcon);
    m_gpuTempLabel = makeValueLabel(*m_gpuTempGroup, sc);
    (void)setShown(m_gpuTempGroup, false);
  });
  addGraphHost(&m_netHost, &m_netGraph, &m_netLegend, [&](Flex& legend) {
    auto* rxGroup = makeIconLabel(legend, "download-speed", sc, &m_rxIcon);
    m_rxLabel = makeValueLabel(*rxGroup, sc);
    auto* txGroup = makeIconLabel(legend, "upload-speed", sc, &m_txIcon);
    m_txLabel = makeValueLabel(*txGroup, sc);
  });

  auto aboutHost = ui::column({
      .out = &m_aboutHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceLg * sc,
      .flexGrow = 1.0F,
      .configure = [sc](Flex& host) { applyContentCardStyle(host, sc); },
  });
  // System and Resources sit side by side when the well is wide (Control Center sized like a
  // Settings window) and stack in the compact panel; doLayout flips the direction.
  Flex* aboutColumns = nullptr;
  aboutHost->addChild(ui::row({
      .out = &aboutColumns,
      .align = FlexAlign::Start,
      .gap = Style::spaceLg * sc,
  }));
  m_aboutColumns = aboutColumns;
  static constexpr const char* kSystemGlyphs[] = {"cpu-usage",        "video",      "device-desktop",
                                                  "layers-intersect", "app-window", "clock"};
  auto* systemCard = makeInfoCard(
      *aboutColumns, i18n::tr("control-center.system.titles.system"), sc, 1.0F, panelCardOpacity(), m_systemLines,
      kSystemLines, kSystemGlyphs
  );
  if (m_upgradeAvailable && m_upgradeAvailable()) {
    const float controlHeight = Style::controlHeight * sc;
    systemCard->addChild(
        ui::label({
            .text = i18n::tr("control-center.system.upgrade-description"),
            .fontSize = Style::fontSizeCaption * sc,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 3,
        })
    );
    systemCard->addChild(
        ui::button({
            .out = &m_upgradeButton,
            .text = i18n::tr("control-center.system.upgrade"),
            .glyph = "refresh",
            .fontSize = Style::fontSizeBody * sc,
            .contentAlign = ButtonContentAlign::Start,
            .variant = ButtonVariant::Default,
            .minHeight = controlHeight,
            .paddingV = Style::spaceSm * sc,
            .paddingH = Style::spaceMd * sc,
            .radius = Style::scaledRadiusMd(sc),
            .flexGrow = 1.0F,
            .onClick =
                [this]() {
                  if (m_upgradeShell) {
                    m_upgradeShell();
                  }
                },
        })
    );
  }

  auto* resourcesCard = makeInfoCard(
      *aboutColumns, i18n::tr("control-center.system.titles.resources"), sc, 1.0F, panelCardOpacity(), nullptr, 0,
      nullptr
  );
  m_resourcesLines[0] = addResourceRow(
      *resourcesCard, "activity", i18n::tr("control-center.system.labels.load"), sc, nullptr,
      i18n::tr("control-center.system.tooltips.load")
  );
  m_resourcesLines[1] = addResourceRow(*resourcesCard, "memory", i18n::tr("control-center.system.labels.ram"), sc);
  m_swapLabel = addResourceRow(
      *resourcesCard, "arrows-exchange", i18n::tr("control-center.system.labels.swap"), sc, &m_swapRow
  );
  m_diskMountPoints = physicalDiskMountPoints();
  if (m_diskMountPoints.size() > kMaxDiskRows) {
    m_diskMountPoints.resize(kMaxDiskRows);
  }
  m_diskLabels.clear();
  m_diskLabels.reserve(m_diskMountPoints.size());
  for (const auto& mountPoint : m_diskMountPoints) {
    Label* usageLabel = nullptr;
    resourcesCard->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceXs * sc},
            ui::glyph({
                .glyph = "storage",
                .glyphSize = Style::fontSizeCaption * sc,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            }),
            ui::label({
                .text = mountPoint,
                .fontSize = Style::fontSizeCaption * sc,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .maxLines = 1,
                .ellipsize = TextEllipsize::Start,
                .flexGrow = 1.0F,
                .configure = [mountPoint](Label& label) { label.setTooltip(mountPoint); },
            }),
            ui::label({
                .out = &usageLabel,
                .fontSize = Style::fontSizeCaption * sc,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .maxLines = 1,
            })
        )
    );
    m_diskLabels.push_back(usageLabel);
  }
  tab->addChild(std::move(aboutHost));

  syncCatalog();
  return m_settings->wrapLive(std::move(tab), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> SystemTab::createHeaderActions() {
  const float scale = contentScale();
  auto live = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .glyph = "copy",
          .enabled = m_clipboard != nullptr,
          .tooltip = i18n::tr("control-center.system.copy-info"),
          .onClick = [this]() { copySystemInfo(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  return m_settings->embedHeader(std::move(live), scale);
}

void SystemTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool SystemTab::configureMode() const { return m_settings->configureMode(); }

void SystemTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

SettingsPane* SystemTab::settingsPane() { return m_settings.get(); }

SystemTab::Catalog SystemTab::catalogFromState(std::string_view value) noexcept {
  if (value == "memory") {
    return Catalog::Memory;
  }
  if (value == "gpu") {
    return Catalog::Gpu;
  }
  if (value == "network") {
    return Catalog::Network;
  }
  if (value == "about") {
    return Catalog::About;
  }
  return Catalog::Cpu;
}

std::string_view SystemTab::catalogState(Catalog catalog) noexcept {
  switch (catalog) {
  case Catalog::Memory:
    return "memory";
  case Catalog::Gpu:
    return "gpu";
  case Catalog::Network:
    return "network";
  case Catalog::About:
    return "about";
  case Catalog::Cpu:
    break;
  }
  return "cpu";
}

std::size_t SystemTab::indexFromCatalog(Catalog catalog) noexcept {
  switch (catalog) {
  case Catalog::Memory:
    return 1;
  case Catalog::Gpu:
    return 2;
  case Catalog::Network:
    return 3;
  case Catalog::About:
    return 4;
  case Catalog::Cpu:
    break;
  }
  return 0;
}

SystemTab::Catalog SystemTab::catalogFromIndex(std::size_t index) noexcept {
  switch (index) {
  case 1:
    return Catalog::Memory;
  case 2:
    return Catalog::Gpu;
  case 3:
    return Catalog::Network;
  case 4:
    return Catalog::About;
  default:
    return Catalog::Cpu;
  }
}

bool SystemTab::syncDetailLabel() {
  if (m_detailLabel == nullptr) {
    return false;
  }
  const char* key = "control-center.system.sections.cpu-caption";
  switch (m_catalog) {
  case Catalog::Memory:
    key = "control-center.system.sections.memory-caption";
    break;
  case Catalog::Gpu:
    key = "control-center.system.sections.gpu-caption";
    break;
  case Catalog::Network:
    key = "control-center.system.sections.network-caption";
    break;
  case Catalog::About:
    key = "control-center.system.sections.about-caption";
    break;
  case Catalog::Cpu:
    break;
  }
  m_detailLabel->setText(i18n::tr(key));
  return setShown(m_detailLabel, true);
}

void SystemTab::persistCatalog() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("system_tab", "catalog", std::string(catalogState(m_pinnedCatalog)));
}

void SystemTab::applyCatalog(std::size_t index, bool persist) {
  if (m_catalogSyncing) {
    return;
  }
  const Catalog next = catalogFromIndex(index);
  if (persist) {
    m_pinnedCatalog = next;
    persistCatalog();
  }
  if (next == m_catalog) {
    syncCatalog();
    return;
  }
  m_catalog = next;
  syncCatalog();
  requestPanelLayout();
}

void SystemTab::syncCatalog() {
  const bool cpu = m_catalog == Catalog::Cpu;
  const bool memory = m_catalog == Catalog::Memory;
  const bool gpu = m_catalog == Catalog::Gpu;
  const bool network = m_catalog == Catalog::Network;
  const bool about = m_catalog == Catalog::About;
  bool layoutDirty = false;
  layoutDirty |= setShown(m_cpuHost, cpu);
  layoutDirty |= setShown(m_ramHost, memory);
  layoutDirty |= setShown(m_gpuHost, gpu);
  layoutDirty |= setShown(m_netHost, network);
  layoutDirty |= setShown(m_aboutHost, about);
  layoutDirty = syncDetailLabel() || layoutDirty;
  if (m_catalogSwitch != nullptr && m_catalogSwitch->selectedIndex() != indexFromCatalog(m_pinnedCatalog)) {
    m_catalogSyncing = true;
    m_catalogSwitch->setSelectedIndex(indexFromCatalog(m_pinnedCatalog));
    m_catalogSyncing = false;
    layoutDirty = true;
  }
  if (layoutDirty) {
    requestPanelLayout();
  }
}

void SystemTab::onPanelCardOpacityChanged(float opacity) {
  if (m_catalogSwitch != nullptr) {
    m_catalogSwitch->setSurfaceOpacity(opacity);
  }
}

void SystemTab::setActive(bool active) {
  m_active = active;
  retainStats(active);
  if (!active) {
    m_redrawLimiter.reset();
    return;
  }
  if (m_config != nullptr) {
    if (const std::optional<std::string> catalog = m_config->stateString("system_tab", "catalog")) {
      m_catalog = catalogFromState(*catalog);
      m_pinnedCatalog = m_catalog;
      syncCatalog();
    }
  }
}

// The control center is built once at startup and lives for the whole session, so retaining for
// the tab's lifetime would sample CPU temperature and the GPU forever, for every user, whether or
// not this tab is ever opened. Polling a GPU keeps a discrete card at D0.
void SystemTab::retainStats(bool retain) {
  if (m_monitor == nullptr || retain == m_statsRetained) {
    return;
  }
  m_statsRetained = retain;

  if (retain) {
    m_monitor->retainCpuTemp();
    m_monitor->retainGpuTemp();
    m_monitor->retainGpuUsage();
    m_monitor->retainGpuVram();
    return;
  }
  m_monitor->releaseCpuTemp();
  m_monitor->releaseGpuTemp();
  m_monitor->releaseGpuUsage();
  m_monitor->releaseGpuVram();
}

void SystemTab::onClose() {
  m_settings->onClose();
  m_root = nullptr;
  m_compactCard = nullptr;
  m_catalogSwitch = nullptr;
  m_titleLabel = nullptr;
  m_glanceCpuLabel = nullptr;
  m_glanceRamLabel = nullptr;
  m_detailLabel = nullptr;
  m_cpuGraph = nullptr;
  m_ramGraph = nullptr;
  m_gpuGraph = nullptr;
  m_netGraph = nullptr;
  m_cpuHost = nullptr;
  m_ramHost = nullptr;
  m_gpuHost = nullptr;
  m_netHost = nullptr;
  m_aboutHost = nullptr;
  m_aboutColumns = nullptr;
  m_cpuLegend = nullptr;
  m_ramLegend = nullptr;
  m_gpuLegend = nullptr;
  m_netLegend = nullptr;
  m_cpuPctIcon = nullptr;
  m_cpuPctLabel = nullptr;
  m_cpuTempIcon = nullptr;
  m_cpuTempLabel = nullptr;
  m_gpuTempGroup = nullptr;
  m_gpuTempIcon = nullptr;
  m_gpuTempLabel = nullptr;
  m_gpuUsageGroup = nullptr;
  m_gpuUsageIcon = nullptr;
  m_gpuUsageLabel = nullptr;
  m_gpuVramGroup = nullptr;
  m_gpuVramIcon = nullptr;
  m_gpuVramLabel = nullptr;
  m_ramIcon = nullptr;
  m_ramLabel = nullptr;
  m_rxIcon = nullptr;
  m_rxLabel = nullptr;
  m_txIcon = nullptr;
  m_txLabel = nullptr;
  m_upgradeButton = nullptr;
  for (auto& l : m_systemLines)
    l = nullptr;
  for (auto& l : m_resourcesLines)
    l = nullptr;
  m_swapRow = nullptr;
  m_swapLabel = nullptr;
  m_diskLabels.clear();
  m_diskMountPoints.clear();
  m_graphInitialized = false;
  m_gpuVisible = false;
  m_lastSampleAt = {};
  m_scrollProgress = 1.0F;
  m_cpuTempMin = 30.0;
  m_cpuTempMax = 80.0;
  m_gpuTempMin = 30.0;
  m_gpuTempMax = 80.0;
  m_netPeak = 0.0;
}

void SystemTab::onFrameTick(float deltaMs) {
  (void)deltaMs;

  if (!m_active || m_monitor == nullptr || !m_monitor->isRunning()) {
    m_redrawLimiter.reset();
    return;
  }

  if (!m_redrawLimiter.shouldStep([]() { PanelManager::instance().requestRedraw(); })) {
    return;
  }

  const auto latestSampleAt = m_monitor->latest().sampledAt;
  if (latestSampleAt != std::chrono::steady_clock::time_point{} && latestSampleAt != m_lastSampleAt) {
    PanelManager::instance().requestUpdateOnly();
  }

  m_scrollProgress = scrollProgressForSample(m_lastSampleAt);

  if (m_cpuGraph != nullptr) {
    m_cpuGraph->setScroll(m_scrollProgress);
  }
  if (m_ramGraph != nullptr) {
    m_ramGraph->setScroll(m_scrollProgress);
  }
  if (m_gpuGraph != nullptr) {
    m_gpuGraph->setScroll(m_scrollProgress);
  }
  if (m_netGraph != nullptr) {
    m_netGraph->setScroll(m_scrollProgress);
  }

  PanelManager::instance().requestRedraw();
}

void SystemTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_root == nullptr) {
    return;
  }

  const float sc = contentScale();

  syncCatalog();
  if (m_aboutColumns != nullptr) {
    const bool wide = contentWidth >= kAboutTwoColumnMinWidth * sc;
    m_aboutColumns->setDirection(wide ? FlexDirection::Horizontal : FlexDirection::Vertical);
    m_aboutColumns->setAlign(wide ? FlexAlign::Start : FlexAlign::Stretch);
    for (const auto& child : m_aboutColumns->children()) {
      if (auto* column = dynamic_cast<Flex*>(child.get()); column != nullptr) {
        // Equal columns side by side; natural height stacked.
        column->setFlexGrow(wide ? 1.0F : 0.0F);
        column->setFillWidth(!wide);
      }
    }
  }
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);

  auto sizeGraph = [&](Graph* g, Flex* host, Flex* legend) {
    if (g == nullptr || host == nullptr || !host->visible()) {
      return;
    }
    const float graphW = std::max(0.0F, host->width() - (host->paddingLeft() + host->paddingRight()));
    const float usedAbove = g->y() - host->y();
    float usedBelow = host->paddingBottom();
    if (legend != nullptr && legend->visible() && legend->height() > 0.0F) {
      usedBelow += legend->height() + host->gap();
    }
    const float graphH = std::max(0.0F, host->height() - usedAbove - usedBelow);
    g->setSize(graphW, graphH);
    g->setLineWidth(kGraphLineWidth * sc);
  };

  sizeGraph(m_cpuGraph, m_cpuHost, m_cpuLegend);
  sizeGraph(m_ramGraph, m_ramHost, m_ramLegend);
  sizeGraph(m_gpuGraph, m_gpuHost, m_gpuLegend);
  sizeGraph(m_netGraph, m_netHost, m_netLegend);

  m_root->layout(renderer);
}

void SystemTab::doUpdate(Renderer& renderer) {
  if (!m_active) {
    return;
  }

  if (m_upgradeButton != nullptr) {
    const bool busy = m_upgradeBusy && m_upgradeBusy();
    m_upgradeButton->setEnabled(!busy);
    m_upgradeButton->setText(
        i18n::tr(busy ? "control-center.system.upgrade-busy" : "control-center.system.upgrade")
    );
  }

  if (m_monitor == nullptr) {
    return;
  }

  const bool monitorRunning = m_monitor->isRunning();

  if (m_cpuGraph != nullptr) {
    m_cpuGraph->setColor(colorSpecFromRole(ColorRole::Primary));
    m_cpuGraph->setColor2(colorSpecFromRole(ColorRole::Error));
  }
  if (m_cpuPctIcon != nullptr) {
    m_cpuPctIcon->setColor(colorSpecFromRole(ColorRole::Primary));
  }
  if (m_cpuPctLabel != nullptr) {
    m_cpuPctLabel->setColor(colorSpecFromRole(ColorRole::Primary));
  }
  if (m_cpuTempIcon != nullptr) {
    m_cpuTempIcon->setColor(colorSpecFromRole(ColorRole::Error));
  }
  if (m_cpuTempLabel != nullptr) {
    m_cpuTempLabel->setColor(colorSpecFromRole(ColorRole::Error));
  }

  if (m_ramGraph != nullptr) {
    m_ramGraph->setColor(colorSpecFromRole(ColorRole::Secondary));
  }
  if (m_ramIcon != nullptr) {
    m_ramIcon->setColor(colorSpecFromRole(ColorRole::Secondary));
  }
  if (m_ramLabel != nullptr) {
    m_ramLabel->setColor(colorSpecFromRole(ColorRole::Secondary));
  }

  if (m_gpuGraph != nullptr) {
    m_gpuGraph->setColor(colorSpecFromRole(ColorRole::Primary));
    m_gpuGraph->setColor2(colorSpecFromRole(ColorRole::Secondary));
    m_gpuGraph->setColor3(colorSpecFromRole(ColorRole::Error));
  }
  if (m_gpuUsageIcon != nullptr) {
    m_gpuUsageIcon->setColor(colorSpecFromRole(ColorRole::Primary));
  }
  if (m_gpuUsageLabel != nullptr) {
    m_gpuUsageLabel->setColor(colorSpecFromRole(ColorRole::Primary));
  }
  if (m_gpuVramIcon != nullptr) {
    m_gpuVramIcon->setColor(colorSpecFromRole(ColorRole::Secondary));
  }
  if (m_gpuVramLabel != nullptr) {
    m_gpuVramLabel->setColor(colorSpecFromRole(ColorRole::Secondary));
  }
  if (m_gpuTempIcon != nullptr) {
    m_gpuTempIcon->setColor(colorSpecFromRole(ColorRole::Error));
  }
  if (m_gpuTempLabel != nullptr) {
    m_gpuTempLabel->setColor(colorSpecFromRole(ColorRole::Error));
  }

  if (m_netGraph != nullptr) {
    m_netGraph->setColor(colorSpecFromRole(ColorRole::Tertiary));
    m_netGraph->setColor2(colorSpecFromRole(ColorRole::Secondary));
  }
  if (m_rxIcon != nullptr) {
    m_rxIcon->setColor(colorSpecFromRole(ColorRole::Tertiary));
  }
  if (m_rxLabel != nullptr) {
    m_rxLabel->setColor(colorSpecFromRole(ColorRole::Tertiary));
  }
  if (m_txIcon != nullptr) {
    m_txIcon->setColor(colorSpecFromRole(ColorRole::Secondary));
  }
  if (m_txLabel != nullptr) {
    m_txLabel->setColor(colorSpecFromRole(ColorRole::Secondary));
  }

  if (monitorRunning) {
    updateGraphs(renderer);
  } else {
    auto clearGraph = [&renderer](Graph* g) {
      if (g != nullptr) {
        g->setValues({});
        g->setValues2({});
        g->setValues3({});
        g->sync(renderer);
      }
    };
    clearGraph(m_cpuGraph);
    clearGraph(m_ramGraph);
    clearGraph(m_gpuGraph);
    clearGraph(m_netGraph);
    m_graphInitialized = false;
    m_lastSampleAt = {};
    m_scrollProgress = 1.0F;
  }
  syncLabels();
}

void SystemTab::updateGraphs(Renderer& renderer) {
  if (m_monitor == nullptr || !m_monitor->isRunning()) {
    return;
  }

  const auto hist = m_monitor->history();
  if (hist.size() < 4) {
    return;
  }

  const auto latestSampleAt = hist.back().sampledAt;
  const bool newData = latestSampleAt != m_lastSampleAt;
  if (!newData && m_graphInitialized) {
    return;
  }

  const auto n = hist.size();

  // CPU: usage (primary) + CPU temp (secondary)
  if (m_cpuGraph != nullptr) {
    std::vector<float> usage(n);
    std::vector<float> cpuTemp(n);
    for (std::size_t i = 0; i < n; ++i) {
      const auto& s = hist[i];
      usage[i] = static_cast<float>(std::clamp(s.cpuUsagePercent / 100.0, 0.0, 1.0));

      if (s.cpuTempC.has_value()) {
        const double t = *s.cpuTempC;
        m_cpuTempMin = std::min(m_cpuTempMin, t);
        m_cpuTempMax = std::max(m_cpuTempMax, t);
        const double range = m_cpuTempMax - m_cpuTempMin;
        cpuTemp[i] = range > 0.0 ? static_cast<float>(std::clamp((t - m_cpuTempMin) / range, 0.0, 1.0)) : 0.5F;
      }
    }
    m_cpuGraph->setValues(std::move(usage));
    m_cpuGraph->setValues2(std::move(cpuTemp));
    m_cpuGraph->sync(renderer);
  }

  // Memory
  if (m_ramGraph != nullptr) {
    std::vector<float> ram(n);
    for (std::size_t i = 0; i < n; ++i) {
      ram[i] = static_cast<float>(std::clamp(hist[i].ramUsagePercent / 100.0, 0.0, 1.0));
    }
    m_ramGraph->setValues(std::move(ram));
    m_ramGraph->sync(renderer);
  }

  // GPU: usage (primary) + VRAM (secondary) + temperature (tertiary)
  if (m_gpuGraph != nullptr) {
    bool hasGpuUsage = false;
    bool hasGpuTemp = false;
    bool hasGpuVram = false;
    std::vector<float> gpuUsage(n);
    std::vector<float> gpuVram(n);
    std::vector<float> gpuTemp(n);
    for (std::size_t i = 0; i < n; ++i) {
      const auto& s = hist[i];
      if (s.gpuUsagePercent.has_value()) {
        hasGpuUsage = true;
        gpuUsage[i] = static_cast<float>(std::clamp(*s.gpuUsagePercent / 100.0, 0.0, 1.0));
      }
      if (s.gpuVramUsedBytes.has_value() && s.gpuVramTotalBytes.has_value() && *s.gpuVramTotalBytes > 0) {
        hasGpuVram = true;
        gpuVram[i] = static_cast<float>(
            std::clamp(static_cast<double>(*s.gpuVramUsedBytes) / static_cast<double>(*s.gpuVramTotalBytes), 0.0, 1.0)
        );
      }
      if (s.gpuTempC.has_value()) {
        hasGpuTemp = true;
        const double t = *s.gpuTempC;
        m_gpuTempMin = std::min(m_gpuTempMin, t);
        m_gpuTempMax = std::max(m_gpuTempMax, t);
        const double range = m_gpuTempMax - m_gpuTempMin;
        gpuTemp[i] = range > 0.0 ? static_cast<float>(std::clamp((t - m_gpuTempMin) / range, 0.0, 1.0)) : 0.5F;
      }
    }
    m_gpuGraph->setValues(hasGpuUsage ? std::move(gpuUsage) : std::vector<float>{});
    m_gpuGraph->setValues2(hasGpuVram ? std::move(gpuVram) : std::vector<float>{});
    m_gpuGraph->setValues3(hasGpuTemp ? std::move(gpuTemp) : std::vector<float>{});
    m_gpuGraph->sync(renderer);
    const bool hasGpuData = hasGpuUsage || hasGpuVram || hasGpuTemp;
    if (hasGpuData != m_gpuVisible) {
      m_gpuVisible = hasGpuData;
      updateGpuVisibility();
    }
  }

  // Network
  if (m_netGraph != nullptr) {
    double maxVal = kNetMinScaleBps;
    for (std::size_t i = 0; i < n; ++i) {
      const auto& s = hist[i];
      maxVal = std::max({maxVal, s.netRxBytesPerSec, s.netTxBytesPerSec});
    }
    m_netPeak = maxVal;

    std::vector<float> rx(n);
    std::vector<float> tx(n);
    for (std::size_t i = 0; i < n; ++i) {
      const auto& s = hist[i];
      rx[i] = static_cast<float>(std::clamp(s.netRxBytesPerSec / m_netPeak, 0.0, 1.0));
      tx[i] = static_cast<float>(std::clamp(s.netTxBytesPerSec / m_netPeak, 0.0, 1.0));
    }
    m_netGraph->setValues(std::move(rx));
    m_netGraph->setValues2(std::move(tx));
    m_netGraph->sync(renderer);
  }

  m_graphInitialized = true;
  m_lastSampleAt = latestSampleAt;
  m_scrollProgress = scrollProgressForSample(m_lastSampleAt);

  if (m_cpuGraph != nullptr) {
    m_cpuGraph->setScroll(m_scrollProgress);
  }
  if (m_ramGraph != nullptr) {
    m_ramGraph->setScroll(m_scrollProgress);
  }
  if (m_gpuGraph != nullptr) {
    m_gpuGraph->setScroll(m_scrollProgress);
  }
  if (m_netGraph != nullptr) {
    m_netGraph->setScroll(m_scrollProgress);
  }

  PanelManager::instance().requestLayout();
  if (m_scrollProgress < 1.0F) {
    PanelManager::instance().requestRedraw();
  }
}

void SystemTab::updateGpuVisibility() {
  if (m_catalog == Catalog::Gpu) {
    requestPanelLayout();
  }
}

void SystemTab::syncLabels() {
  if (m_monitor == nullptr || !m_monitor->isRunning()) {
    if (m_cpuPctLabel != nullptr) {
      m_cpuPctLabel->setText("--");
    }
    if (m_glanceCpuLabel != nullptr) {
      m_glanceCpuLabel->setText("--%");
    }
    if (m_cpuTempLabel != nullptr) {
      m_cpuTempLabel->setText("--");
    }
    if (m_gpuTempLabel != nullptr) {
      m_gpuTempLabel->setText("--");
    }
    if (m_gpuUsageLabel != nullptr) {
      m_gpuUsageLabel->setText("--");
    }
    if (m_gpuVramLabel != nullptr) {
      m_gpuVramLabel->setText("--");
    }
    if (m_ramLabel != nullptr) {
      m_ramLabel->setText("--");
    }
    if (m_glanceRamLabel != nullptr) {
      m_glanceRamLabel->setText("--");
    }
    if (m_rxLabel != nullptr) {
      m_rxLabel->setText("--");
    }
    if (m_txLabel != nullptr) {
      m_txLabel->setText("--");
    }
    return;
  }

  const auto stats = m_monitor->latest();

  if (m_cpuPctLabel != nullptr) {
    m_cpuPctLabel->setText(std::format("{:.0F}%", stats.cpuUsagePercent));
  }
  if (m_glanceCpuLabel != nullptr) {
    m_glanceCpuLabel->setText(std::format("{:.0F}%", stats.cpuUsagePercent));
  }
  if (m_cpuTempLabel != nullptr) {
    if (stats.cpuTempC.has_value()) {
      m_cpuTempLabel->setText(std::format("{:.0F}°C", *stats.cpuTempC));
    } else {
      m_cpuTempLabel->setText("--");
    }
  }
  if (m_gpuTempGroup != nullptr) {
    const bool hasTempData = stats.gpuTempC.has_value();
    (void)setShown(m_gpuTempGroup, hasTempData);
    if (hasTempData && m_gpuTempLabel != nullptr) {
      m_gpuTempLabel->setText(std::format("{:.0F}°C", *stats.gpuTempC));
    }
  }
  if (m_gpuUsageGroup != nullptr) {
    const bool hasUsageData = stats.gpuUsagePercent.has_value();
    (void)setShown(m_gpuUsageGroup, hasUsageData);
    if (hasUsageData && m_gpuUsageLabel != nullptr) {
      m_gpuUsageLabel->setText(std::format("{:.0F}%", *stats.gpuUsagePercent));
    }
  }
  if (m_gpuVramGroup != nullptr) {
    const bool hasVramData = stats.gpuVramUsedBytes.has_value();
    (void)setShown(m_gpuVramGroup, hasVramData);
    if (hasVramData && m_gpuVramLabel != nullptr) {
      m_gpuVramLabel->setText(formatGpuVramUsed(stats));
    }
  }
  if (m_ramLabel != nullptr) {
    m_ramLabel->setText(
        FormatUnits::formatBinaryMibAsGib(stats.ramUsedMb) + std::format(" · {:.0F}%", stats.ramUsagePercent)
    );
  }
  if (m_glanceRamLabel != nullptr) {
    m_glanceRamLabel->setText(
        FormatUnits::formatBinaryMibAsGib(stats.ramUsedMb) + std::format(" · {:.0F}%", stats.ramUsagePercent)
    );
  }
  if (m_rxLabel != nullptr) {
    m_rxLabel->setText(FormatUnits::formatDecimalBytesPerSecond(stats.netRxBytesPerSec));
  }
  if (m_txLabel != nullptr) {
    m_txLabel->setText(FormatUnits::formatDecimalBytesPerSecond(stats.netTxBytesPerSec));
  }

  // System info

  if (m_systemLines[0] != nullptr) {
    m_systemLines[0]->setText(cpuModelName());
  }
  if (m_systemLines[1] != nullptr) {
    m_systemLines[1]->setText(gpuLabel());
  }
  if (m_systemLines[2] != nullptr) {
    m_systemLines[2]->setText(distroLabel());
  }
  if (m_titleLabel != nullptr) {
    m_titleLabel->setText(distroLabel());
  }
  if (m_systemLines[3] != nullptr) {
    m_systemLines[3]->setText(kernelLabel());
  }
  if (m_systemLines[4] != nullptr) {
    m_systemLines[4]->setText(compositorLabel());
  }
  if (m_systemLines[5] != nullptr) {
    const auto uptime = systemUptime();
    const std::string uptimeText =
        uptime.has_value() ? formatDuration(*uptime) : i18n::tr("control-center.system.unknown");
    m_systemLines[5]->setText(
        i18n::tr("control-center.system.uptime-prefix", "uptime", uptimeText, "osAge", osAgeLabel())
    );
  }

  // Resources info
  if (m_resourcesLines[0] != nullptr) {
    m_resourcesLines[0]->setText(
        std::format("{:.2F} / {:.2F} / {:.2F}", stats.loadAvg1, stats.loadAvg5, stats.loadAvg15)
    );
  }
  if (m_resourcesLines[1] != nullptr) {
    m_resourcesLines[1]->setText(formatMemoryUsedTotal(stats));
  }
  if (m_swapRow != nullptr) {
    const bool hasSwap = stats.swapTotalMb > 0;
    (void)setShown(m_swapRow, hasSwap);
    if (hasSwap && m_swapLabel != nullptr) {
      m_swapLabel->setText(FormatUnits::formatBinaryMibUsageAsGib(stats.swapUsedMb, stats.swapTotalMb));
    }
  }
  for (std::size_t i = 0; i < m_diskLabels.size(); ++i) {
    if (m_diskLabels[i] != nullptr) {
      m_diskLabels[i]->setText(diskUsageLabel(m_diskMountPoints[i]));
    }
  }
}

float SystemTab::scrollProgressForSample(std::chrono::steady_clock::time_point sampledAt) const {
  if (sampledAt == std::chrono::steady_clock::time_point{}) {
    return 1.0F;
  }

  const auto sampleInterval = m_monitor != nullptr ? m_monitor->historySampleInterval()
                                                   : std::chrono::steady_clock::duration{std::chrono::seconds(1)};
  if (sampleInterval.count() <= 0) {
    return 1.0F;
  }

  const auto elapsed = std::chrono::steady_clock::now() - sampledAt;
  const auto clamped = std::clamp(elapsed, std::chrono::steady_clock::duration::zero(), sampleInterval);
  return std::chrono::duration<float>(clamped).count() / std::chrono::duration<float>(sampleInterval).count();
}

std::string SystemTab::systemInfoText() const {
  std::string text;
  text += i18n::tr("brand.version", "version", noctalia::build_info::displayVersion());
  text += '\n';
  text += cpuModelName();
  text += '\n';
  text += gpuLabel();
  text += '\n';
  text += distroLabel();
  text += '\n';
  text += kernelLabel();
  text += '\n';
  text += compositorLabel();
  text += '\n';
  const auto uptime = systemUptime();
  const std::string uptimeText =
      uptime.has_value() ? formatDuration(*uptime) : i18n::tr("control-center.system.unknown");
  text += i18n::tr("control-center.system.uptime-prefix", "uptime", uptimeText, "osAge", osAgeLabel());
  return text;
}

void SystemTab::copySystemInfo() {
  if (m_clipboard == nullptr) {
    return;
  }
  (void)m_clipboard->copyText(systemInfoText());
}
