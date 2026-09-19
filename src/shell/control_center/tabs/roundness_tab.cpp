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
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cmath>
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

std::unique_ptr<Flex> RoundnessTab::createLiveBody(bool hosted) {
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto compactCard = ui::column({
      .configure =
          [scale, opacity = panelCardOpacity(), hosted](Flex& section) {
            applyCompactCardStyle(section, scale, opacity, hosted);
          },
  });
  std::vector<ui::SegmentedOption> catalogOptions{
      {
          .label = "",
          .glyph = "border-radius",
          .tooltip = i18n::tr("settings.schema.appearance.corner-roundness.label"),
      },
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
          .onChange = [this](std::size_t index) { applyCatalog(index); },
      })
  );
  auto makeHost = [&](Flex** out, bool visible) {
    return ui::column({
        .out = out,
        .align = FlexAlign::Stretch,
        .visible = visible,
        .participatesInLayout = visible,
    });
  };

  auto shellGroup = makeHost(&m_shellHost, true);
  addSliderSection(
      *shellGroup, scale, i18n::tr("settings.schema.appearance.corner-roundness.label"), &m_roundness,
      &m_roundnessValue, noctalia::config::schema::kCornerRadiusScaleRange.min.value_or(0.0F),
      noctalia::config::schema::kCornerRadiusScaleRange.max.value_or(2.0F),
      noctalia::config::schema::kCornerRadiusScaleRange.step.value_or(0.05F), 1.0, multiplierLabel,
      [this]() { commitRoundness(); }
  );
  compactCard->addChild(std::move(shellGroup));

  auto barGroup = makeHost(&m_barHost, false);
  addSliderSection(
      *barGroup, scale, i18n::tr("settings.navigation.sections.bar"), &m_barRadius, &m_barRadiusValue, 0.0, 80.0, 1.0,
      0.0, pixelLabel, [this]() { commitBarRadius(); }
  );
  compactCard->addChild(std::move(barGroup));

  auto dockGroup = makeHost(&m_dockHost, false);
  addSliderSection(
      *dockGroup, scale, i18n::tr("settings.navigation.sections.dock"), &m_dockRadius, &m_dockRadiusValue,
      static_cast<double>(noctalia::config::schema::kDockRadiusRange.min.value_or(0)),
      static_cast<double>(noctalia::config::schema::kDockRadiusRange.max.value_or(80)),
      static_cast<double>(noctalia::config::schema::kDockRadiusRange.step.value_or(1)), 0.0, pixelLabel,
      [this]() { commitDockRadius(); }
  );
  compactCard->addChild(std::move(dockGroup));

  if (windowsAvailable()) {
    auto windowGroup = makeHost(&m_windowHost, false);
    addSliderSection(
        *windowGroup, scale, i18n::tr("control-center.roundness.windows-title"), &m_windowRadius, &m_windowRadiusValue,
        0.0, 40.0, 1.0, 0.0, pixelLabel, [this]() { commitWindowRadius(); }
    );
    compactCard->addChild(std::move(windowGroup));
  }

  compactCard->addChild(
      ui::label({
          .out = &m_detailLabel,
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
      })
  );
  tab->addChild(std::move(compactCard));

  syncLiveControls();
  syncCatalog();
  return tab;
}

std::unique_ptr<Flex> RoundnessTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void RoundnessTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_catalogSwitch = nullptr;
  m_detailLabel = nullptr;
  m_shellHost = nullptr;
  m_roundness = nullptr;
  m_roundnessValue = nullptr;
  m_barHost = nullptr;
  m_barRadius = nullptr;
  m_barRadiusValue = nullptr;
  m_dockHost = nullptr;
  m_dockRadius = nullptr;
  m_dockRadiusValue = nullptr;
  m_windowHost = nullptr;
  m_windowRadius = nullptr;
  m_windowRadiusValue = nullptr;
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
  } else {
    m_barName.clear();
    if (m_barRadius != nullptr) {
      m_barRadius->setEnabled(false);
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
  if (value == "bar") {
    return Catalog::Bar;
  }
  if (value == "dock") {
    return Catalog::Dock;
  }
  if (value == "windows" && windowsAvailable()) {
    return Catalog::Windows;
  }
  return Catalog::Shell;
}

std::string_view RoundnessTab::catalogState(Catalog catalog) noexcept {
  switch (catalog) {
  case Catalog::Bar:
    return "bar";
  case Catalog::Dock:
    return "dock";
  case Catalog::Windows:
    return "windows";
  case Catalog::Shell:
    break;
  }
  return "shell";
}

std::size_t RoundnessTab::indexFromCatalog(Catalog catalog) const noexcept {
  switch (catalog) {
  case Catalog::Bar:
    return 1;
  case Catalog::Dock:
    return 2;
  case Catalog::Windows:
    return windowsAvailable() ? 3 : 0;
  case Catalog::Shell:
    break;
  }
  return 0;
}

RoundnessTab::Catalog RoundnessTab::catalogFromIndex(std::size_t index) const noexcept {
  if (index == 1) {
    return Catalog::Bar;
  }
  if (index == 2) {
    return Catalog::Dock;
  }
  if (index == 3 && windowsAvailable()) {
    return Catalog::Windows;
  }
  return Catalog::Shell;
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
  const char* key = "settings.schema.appearance.corner-roundness.description";
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
  case Catalog::Shell:
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
    m_catalog = Catalog::Shell;
    m_pinnedCatalog = Catalog::Shell;
  }
  const bool shell = m_catalog == Catalog::Shell;
  const bool bar = m_catalog == Catalog::Bar;
  const bool dock = m_catalog == Catalog::Dock;
  const bool windows = m_catalog == Catalog::Windows;
  bool dirty = setShown(m_shellHost, shell) || setShown(m_barHost, bar) || setShown(m_dockHost, dock)
      || setShown(m_windowHost, windows);
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

void RoundnessTab::applyCatalog(std::size_t index) {
  if (m_catalogSyncing) {
    return;
  }
  const Catalog next = catalogFromIndex(index);
  m_pinnedCatalog = next;
  persistCatalog();
  if (next == m_catalog) {
    syncCatalog();
    return;
  }
  m_catalog = next;
  syncCatalog();
}

void RoundnessTab::onPanelCardOpacityChanged(float opacity) {
  if (m_catalogSwitch != nullptr) {
    m_catalogSwitch->setSurfaceOpacity(opacity);
  }
}
