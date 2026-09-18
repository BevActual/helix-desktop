#include "shell/control_center/tabs/roundness_tab.h"

#include "compositors/compositor_detect.h"
#include "compositors/niri/niri_window_rules.h"
#include "config/config_service.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cmath>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  [[nodiscard]] std::string multiplierLabel(double value) { return std::format("{:.2f}×", value); }
  [[nodiscard]] std::string pixelLabel(double value) { return std::format("{}px", static_cast<int>(value)); }

  [[nodiscard]] bool setShown(Node* node, bool shown) {
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

} // namespace

RoundnessTab::RoundnessTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "roundness", .showConfigureButton = false}
      )) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> catalog = m_config->stateString("roundness_tab", "catalog")) {
      m_catalog = catalogFromState(*catalog);
      m_pinnedCatalog = m_catalog;
    }
  }
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> RoundnessTab::create() {
  return m_settings->wrapLive(createLiveBody(), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> RoundnessTab::createLiveBody() {
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;
  const float chip = Style::controlHeight * scale;

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto compactCard = ui::column({
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applySectionCardStyle(section, scale, opacity);
      },
  });
  addSliderSection(
      *compactCard, scale, i18n::tr("settings.schema.appearance.corner-roundness.label"), &m_roundness,
      &m_roundnessValue, noctalia::config::schema::kCornerRadiusScaleRange.min.value_or(0.0F),
      noctalia::config::schema::kCornerRadiusScaleRange.max.value_or(2.0F),
      noctalia::config::schema::kCornerRadiusScaleRange.step.value_or(0.05F), 1.0, multiplierLabel,
      [this]() { commitRoundness(); }
  );
  auto preview = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceMd * scale,
  });
  auto makeRadiusChip = [&](Box** out, float radius, const std::string& caption) {
    auto cell = ui::column({
        .align = FlexAlign::Center,
        .gap = Style::spaceXs * scale,
        .flexGrow = 1.0F,
    });
    cell->addChild(
        ui::box({
            .out = out,
            .fill = colorSpecFromRole(ColorRole::Surface),
            .border = colorSpecFromRole(ColorRole::Outline, 0.45F),
            .borderWidth = 1.0F,
            .radius = radius,
            .width = chip,
            .height = chip,
        })
    );
    cell->addChild(
        ui::label({
            .text = caption,
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
            .textAlign = TextAlign::Center,
        })
    );
    return cell;
  };
  preview->addChild(
      makeRadiusChip(
          &m_radiusChipSm, Style::scaledRadiusSm(scale), i18n::tr("control-center.appearance.radius-small")
      )
  );
  preview->addChild(
      makeRadiusChip(
          &m_radiusChipMd, Style::scaledRadiusMd(scale), i18n::tr("control-center.appearance.radius-medium")
      )
  );
  preview->addChild(
      makeRadiusChip(
          &m_radiusChip2xl, Style::scaledRadius2xl(scale), i18n::tr("control-center.appearance.radius-large")
      )
  );
  compactCard->addChild(std::move(preview));
  std::vector<ui::SegmentedOption> catalogOptions{
      {
          .label = "",
          .glyph = "layout-navbar",
          .tooltip = i18n::tr("settings.navigation.sections.bar"),
      },
      {
          .label = "",
          .glyph = "layout-bottombar-inactive",
          .tooltip = i18n::tr("settings.navigation.sections.dock"),
      },
  };
  if (windowsAvailable()) {
    catalogOptions.push_back({
        .label = "",
        .glyph = "app-window",
        .tooltip = i18n::tr("control-center.roundness.windows-title"),
    });
  }
  compactCard->addChild(
      ui::segmented({
          .out = &m_catalogSwitch,
          .options = std::move(catalogOptions),
          .selectedIndex = indexFromCatalog(m_catalog),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .onChange = [this](std::size_t index) { applyCatalog(index, true); },
      })
  );
  compactCard->addChild(
      ui::label({
          .out = &m_detailLabel,
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
      })
  );
  tab->addChild(std::move(compactCard));

  auto leftover = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto makeDesktopWell = [&](float minHeight, bool grow) {
    return ui::column({
        .align = FlexAlign::Stretch,
        .justify = FlexJustify::Center,
        .padding = Style::spaceSm * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceVariant),
        .radius = Style::scaledRadiusXl(scale),
        .minHeight = minHeight,
        .fillWidth = true,
        .flexGrow = grow ? 1.0F : 0.0F,
    });
  };

  auto barGroup = ui::column({
      .out = &m_barHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
  });
  addSliderSection(
      *barGroup, scale, i18n::tr("settings.navigation.sections.bar"), &m_barRadius, &m_barRadiusValue, 0.0, 80.0, 1.0,
      0.0, pixelLabel, [this]() { commitBarRadius(); }
  );
  auto barWell = makeDesktopWell(Style::controlHeightLg * 1.8F * scale, true);
  barWell->addChild(
      ui::row({
          .out = &m_barChip,
          .fill = colorSpecFromRole(ColorRole::Surface),
          .radius = Style::scaledRadiusMd(scale),
          .border = colorSpecFromRole(ColorRole::Outline, 0.45F),
          .borderWidth = 1.0F,
          .minHeight = Style::controlHeight * 0.5F * scale,
          .fillWidth = true,
      })
  );
  barGroup->addChild(std::move(barWell));
  m_barDescription = addCaption(*barGroup, i18n::tr("settings.schema.bar.corner-radius.description"), scale);
  leftover->addChild(std::move(barGroup));

  auto dockGroup = ui::column({
      .out = &m_dockHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
      .visible = false,
      .participatesInLayout = false,
  });
  addSliderSection(
      *dockGroup, scale, i18n::tr("settings.navigation.sections.dock"), &m_dockRadius, &m_dockRadiusValue,
      static_cast<double>(noctalia::config::schema::kDockRadiusRange.min.value_or(0)),
      static_cast<double>(noctalia::config::schema::kDockRadiusRange.max.value_or(80)),
      static_cast<double>(noctalia::config::schema::kDockRadiusRange.step.value_or(1)), 0.0, pixelLabel,
      [this]() { commitDockRadius(); }
  );
  auto dockWell = makeDesktopWell(Style::controlHeightLg * 2.2F * scale, true);
  dockWell->setAlign(FlexAlign::Center);
  dockWell->addChild(
      ui::row({
          .out = &m_dockChip,
          .fill = colorSpecFromRole(ColorRole::Surface),
          .radius = Style::scaledRadiusMd(scale),
          .border = colorSpecFromRole(ColorRole::Outline, 0.45F),
          .borderWidth = 1.0F,
          .minHeight = Style::controlHeight * scale,
          .width = Style::controlHeight * 2.4F * scale,
      })
  );
  dockGroup->addChild(std::move(dockWell));
  addCaption(*dockGroup, i18n::tr("settings.schema.dock.corner-radius.description"), scale);
  leftover->addChild(std::move(dockGroup));

  if (windowsAvailable()) {
    auto windowGroup = ui::column({
        .out = &m_windowHost,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .flexGrow = 1.0F,
        .visible = false,
        .participatesInLayout = false,
    });
    addSliderSection(
        *windowGroup, scale, i18n::tr("control-center.roundness.windows-title"), &m_windowRadius, &m_windowRadiusValue,
        0.0, 40.0, 1.0, 0.0, pixelLabel, [this]() { commitWindowRadius(); }
    );
    auto windowWell = makeDesktopWell(Style::controlHeightLg * 3.2F * scale, true);
    auto window = ui::column({
        .out = &m_windowChip,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .padding = Style::spaceSm * scale,
        .fill = colorSpecFromRole(ColorRole::Surface),
        .radius = Style::scaledRadiusMd(scale),
        .border = colorSpecFromRole(ColorRole::Outline, 0.45F),
        .borderWidth = 1.0F,
        .minHeight = Style::controlHeightLg * 2.4F * scale,
        .fillWidth = true,
        .clipChildren = true,
        .flexGrow = 1.0F,
    });
    window->addChild(
        ui::row({
            .fill = colorSpecFromRole(ColorRole::Primary),
            .radius = 2.0F * scale,
            .minHeight = 8.0F * scale,
            .fillWidth = true,
        })
    );
    window->addChild(
        ui::row({
            .fill = colorSpecFromRole(ColorRole::Outline, 0.45F),
            .radius = 2.0F * scale,
            .minHeight = 6.0F * scale,
            .maxWidth = 96.0F * scale,
            .fillWidth = true,
        })
    );
    window->addChild(
        ui::row({
            .fill = colorSpecFromRole(ColorRole::Outline, 0.28F),
            .radius = 2.0F * scale,
            .minHeight = 6.0F * scale,
            .maxWidth = 64.0F * scale,
            .fillWidth = true,
        })
    );
    windowWell->addChild(std::move(window));
    windowGroup->addChild(std::move(windowWell));
    addCaption(*windowGroup, i18n::tr("control-center.roundness.windows-description"), scale);
    leftover->addChild(std::move(windowGroup));
  }

  tab->addChild(std::move(leftover));

  syncLiveControls();
  syncCatalog();
  return tab;
}

std::unique_ptr<Flex> RoundnessTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void RoundnessTab::onClose() {
  m_hoverEndTimer.stop();
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_roundness = nullptr;
  m_roundnessValue = nullptr;
  m_radiusChipSm = nullptr;
  m_radiusChipMd = nullptr;
  m_radiusChip2xl = nullptr;
  m_catalogSwitch = nullptr;
  m_detailLabel = nullptr;
  m_barHost = nullptr;
  m_barRadius = nullptr;
  m_barRadiusValue = nullptr;
  m_barDescription = nullptr;
  m_barChip = nullptr;
  m_dockHost = nullptr;
  m_dockRadius = nullptr;
  m_dockRadiusValue = nullptr;
  m_dockChip = nullptr;
  m_windowHost = nullptr;
  m_windowRadius = nullptr;
  m_windowRadiusValue = nullptr;
  m_windowChip = nullptr;
}

void RoundnessTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool RoundnessTab::configureMode() const { return m_settings->configureMode(); }

void RoundnessTab::setSettingsSearchQuery(std::string_view query) {
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* RoundnessTab::settingsPane() { return m_settings.get(); }

void RoundnessTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  syncCatalog();
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void RoundnessTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
  updateRadiusPreview();
}

void RoundnessTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  m_syncing = true;
  const Config& cfg = m_config->config();
  const float roundness = cfg.shell.cornerRadiusScale;
  if (m_roundness != nullptr) {
    m_roundness->setValue(roundness);
  }
  if (m_roundnessValue != nullptr) {
    m_roundnessValue->setText(multiplierLabel(roundness));
  }
  updateRadiusPreview();

  if (!cfg.bars.empty()) {
    m_barName = cfg.bars.front().name;
    const auto barRadius = static_cast<double>(cfg.bars.front().radius);
    if (m_barRadius != nullptr) {
      m_barRadius->setEnabled(true);
      m_barRadius->setValue(barRadius);
    }
    if (m_barRadiusValue != nullptr) {
      m_barRadiusValue->setText(pixelLabel(barRadius));
    }
    if (m_barDescription != nullptr) {
      m_barDescription->setText(i18n::tr("settings.schema.bar.corner-radius.description"));
    }
  } else {
    m_barName.clear();
    if (m_barRadius != nullptr) {
      m_barRadius->setEnabled(false);
    }
    if (m_barDescription != nullptr) {
      m_barDescription->setText(i18n::tr("control-center.roundness.bar-empty"));
    }
  }

  const auto dockRadius = static_cast<double>(cfg.dock.radius);
  if (m_dockRadius != nullptr) {
    m_dockRadius->setValue(dockRadius);
  }
  if (m_dockRadiusValue != nullptr) {
    m_dockRadiusValue->setText(pixelLabel(dockRadius));
  }

  const auto windowRadius = static_cast<double>(compositors::niri::currentWindowCornerRadius().value_or(0));
  if (m_windowRadius != nullptr) {
    m_windowRadius->setValue(windowRadius);
  }
  if (m_windowRadiusValue != nullptr) {
    m_windowRadiusValue->setText(pixelLabel(windowRadius));
  }
  m_syncing = false;
  syncCatalog();
}

void RoundnessTab::updateRadiusPreview() {
  const float scale = contentScale();
  const float roundness = m_roundness != nullptr ? static_cast<float>(m_roundness->value()) : Style::cornerRadiusScale();
  if (m_radiusChipSm != nullptr) {
    m_radiusChipSm->setRadius(Style::radiusSm * scale * roundness);
  }
  if (m_radiusChipMd != nullptr) {
    m_radiusChipMd->setRadius(Style::radiusMd * scale * roundness);
  }
  if (m_radiusChip2xl != nullptr) {
    m_radiusChip2xl->setRadius(Style::radius2xl * scale * roundness);
  }
  if (m_barChip != nullptr && m_barRadius != nullptr) {
    m_barChip->setRadius(static_cast<float>(m_barRadius->value()) * scale);
  }
  if (m_dockChip != nullptr && m_dockRadius != nullptr) {
    m_dockChip->setRadius(static_cast<float>(m_dockRadius->value()) * scale);
  }
  if (m_windowChip != nullptr && m_windowRadius != nullptr) {
    m_windowChip->setRadius(static_cast<float>(m_windowRadius->value()) * scale);
  }
}

void RoundnessTab::commitRoundness() {
  if (m_syncing || m_config == nullptr || m_roundness == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "corner_radius_scale"}, m_roundness->value());
}

void RoundnessTab::commitBarRadius() {
  if (m_syncing || m_config == nullptr || m_barRadius == nullptr || m_barName.empty()) {
    return;
  }
  (void)m_config->setOverride({"bar", m_barName, "radius"}, static_cast<std::int64_t>(m_barRadius->value()));
}

void RoundnessTab::commitDockRadius() {
  if (m_syncing || m_config == nullptr || m_dockRadius == nullptr) {
    return;
  }
  (void)m_config->setOverride({"dock", "radius"}, static_cast<std::int64_t>(m_dockRadius->value()));
}

void RoundnessTab::commitWindowRadius() {
  if (m_syncing || m_windowRadius == nullptr) {
    return;
  }
  std::optional<bool> drawBorder;
  if (m_config != nullptr) {
    drawBorder = m_config->config().theme.niriLayout.drawBorderWithBackground;
  }
  const auto result = compositors::niri::saveWindowRules({
      .cornerRadius = static_cast<int>(std::lround(m_windowRadius->value())),
      .drawBorderWithBackground = drawBorder,
  });
  if (!result.ok) {
    notify::error(
        "Noctalia", i18n::tr("control-center.roundness.windows-save-title"),
        result.error.empty() ? i18n::tr("control-center.roundness.windows-save-failed") : result.error
    );
  }
}

bool RoundnessTab::windowsAvailable() const noexcept { return compositors::isNiri(); }

RoundnessTab::Catalog RoundnessTab::catalogFromState(std::string_view value) const noexcept {
  if (value == "dock") {
    return Catalog::Dock;
  }
  if (value == "windows" && windowsAvailable()) {
    return Catalog::Windows;
  }
  return Catalog::Bar;
}

std::string_view RoundnessTab::catalogState(Catalog catalog) noexcept {
  switch (catalog) {
  case Catalog::Dock:
    return "dock";
  case Catalog::Windows:
    return "windows";
  case Catalog::Bar:
    break;
  }
  return "bar";
}

std::size_t RoundnessTab::indexFromCatalog(Catalog catalog) const noexcept {
  switch (catalog) {
  case Catalog::Dock:
    return 1;
  case Catalog::Windows:
    return windowsAvailable() ? 2 : 0;
  case Catalog::Bar:
    break;
  }
  return 0;
}

RoundnessTab::Catalog RoundnessTab::catalogFromIndex(std::size_t index) const noexcept {
  if (index == 1) {
    return Catalog::Dock;
  }
  if (index == 2 && windowsAvailable()) {
    return Catalog::Windows;
  }
  return Catalog::Bar;
}

void RoundnessTab::persistCatalog() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("roundness_tab", "catalog", std::string(catalogState(m_pinnedCatalog)));
}

bool RoundnessTab::syncDetailLabel() {
  if (m_detailLabel == nullptr) {
    return false;
  }
  const char* key = "settings.schema.bar.corner-radius.description";
  switch (m_catalog) {
  case Catalog::Dock:
    key = "settings.schema.dock.corner-radius.description";
    break;
  case Catalog::Windows:
    key = "control-center.roundness.windows-description";
    break;
  case Catalog::Bar:
    key = m_barName.empty() ? "control-center.roundness.bar-empty"
                            : "settings.schema.bar.corner-radius.description";
    break;
  }
  const std::string text = i18n::tr(key);
  const bool changed = m_detailLabel->text() != text;
  if (changed) {
    m_detailLabel->setText(text);
  }
  return changed;
}

void RoundnessTab::syncCatalog() {
  if (m_catalog == Catalog::Windows && !windowsAvailable()) {
    m_catalog = Catalog::Bar;
    m_pinnedCatalog = Catalog::Bar;
  }
  const bool bar = m_catalog == Catalog::Bar;
  const bool dock = m_catalog == Catalog::Dock;
  const bool windows = m_catalog == Catalog::Windows;
  bool dirty = setShown(m_barHost, bar) || setShown(m_dockHost, dock) || setShown(m_windowHost, windows);
  dirty = syncDetailLabel() || dirty;
  if (m_catalogSwitch != nullptr && m_catalogSwitch->selectedIndex() != indexFromCatalog(m_pinnedCatalog)) {
    m_catalogSyncing = true;
    m_catalogSwitch->setSelectedIndex(indexFromCatalog(m_pinnedCatalog));
    m_catalogSyncing = false;
    dirty = true;
  }
  if (dirty) {
    PanelManager::instance().requestLayout();
  }
}

void RoundnessTab::applyCatalog(std::size_t index, bool persist) {
  if (m_catalogSyncing) {
    return;
  }
  const Catalog next = catalogFromIndex(index);
  if (persist) {
    m_hoverEndTimer.stop();
    m_pinnedCatalog = next;
    persistCatalog();
  }
  if (next == m_catalog) {
    syncCatalog();
    return;
  }
  m_catalog = next;
  syncCatalog();
}

void RoundnessTab::previewCatalog(std::size_t index) {
  m_hoverEndTimer.stop();
  applyCatalog(index, false);
  PanelManager::instance().refresh();
}

void RoundnessTab::restorePinnedCatalog() {
  m_hoverEndTimer.stop();
  m_hoverEndTimer.start(std::chrono::milliseconds(40), [this]() {
    applyCatalog(indexFromCatalog(m_pinnedCatalog), false);
    PanelManager::instance().refresh();
  });
}

void RoundnessTab::onPanelCardOpacityChanged(float opacity) {
  if (m_catalogSwitch != nullptr) {
    m_catalogSwitch->setSurfaceOpacity(opacity);
  }
}
