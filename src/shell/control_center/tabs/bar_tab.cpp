#include "shell/control_center/tabs/bar_tab.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
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

  [[nodiscard]] std::size_t autoHideIndex(const BarConfig& bar) {
    if (bar.smartAutoHide) {
      return kAutoHideSmart;
    }
    if (bar.autoHide) {
      return kAutoHideOn;
    }
    return kAutoHideOff;
  }

  constexpr std::array<const char*, 4> kPositionValues = {"top", "bottom", "left", "right"};

  [[nodiscard]] std::size_t positionIndex(const std::string& position) {
    for (std::size_t i = 0; i < kPositionValues.size(); ++i) {
      if (position == kPositionValues[i]) {
        return i;
      }
    }
    return 0;
  }

  [[nodiscard]] const BarConfig* firstBar(const Config& cfg) {
    return cfg.bars.empty() ? nullptr : &cfg.bars.front();
  }

  [[nodiscard]] std::string pixelLabel(double value) { return std::format("{}px", static_cast<int>(value)); }
  [[nodiscard]] std::string percentLabel(double value) { return std::format("{}%", static_cast<int>(value * 100.0)); }

} // namespace

std::size_t BarTab::indexFromSection(Section section) noexcept {
  return section == Section::Shape ? 1 : 0;
}

BarTab::Section BarTab::sectionFromIndex(std::size_t index) noexcept {
  return index == 1 ? Section::Shape : Section::Layout;
}

BarTab::Section BarTab::sectionFromId(std::string_view id) noexcept {
  return id == "shape" ? Section::Shape : Section::Layout;
}

std::string_view BarTab::idFromSection(Section section) noexcept {
  return section == Section::Shape ? "shape" : "layout";
}

BarTab::BarTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "bar", .showConfigureButton = false}
      )) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> section = m_config->stateString("bar_tab", "section")) {
      m_section = sectionFromId(*section);
    }
  }
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> BarTab::create() {
  return m_settings->wrapLive(createLiveBody(), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> BarTab::createLiveBody(bool hosted) {
  m_hosted = hosted;
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;
  const float opacity = panelCardOpacity();

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = hosted ? 0.0F : 1.0F,
  });

  const BarConfig* bar = m_config != nullptr ? firstBar(m_config->config()) : nullptr;
  const BarConfig defaultBar{};
  const BarConfig& barRef = bar != nullptr ? *bar : defaultBar;
  const bool hasBar = bar != nullptr;
  const bool barOn = hasBar && barRef.enabled;
  if (bar != nullptr) {
    m_barName = bar->name;
  }

  auto compactCard = ui::column({
      .configure =
          [scale, opacity, hosted](Flex& card) {
            applyCompactCardStyle(card, scale, opacity, hosted);
          },
  });
  compactCard->addChild(
      ui::label({
          .out = &m_empty,
          .text = i18n::tr("control-center.bar.empty"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
          .visible = !hasBar,
          .participatesInLayout = !hasBar,
      })
  );
  auto enableHost = ui::column({
      .out = &m_enableRow,
      .align = FlexAlign::Stretch,
      .visible = hasBar,
      .participatesInLayout = hasBar,
  });
  m_enabled = addToggleRow(*enableHost, scale, i18n::tr("control-center.bar.enable"), [this](bool value) {
    applyEnabled(value);
  });
  if (m_enabled != nullptr) {
    m_enabled->setCheckedImmediate(barRef.enabled);
  }
  compactCard->addChild(std::move(enableHost));

  if (!hosted) {
    auto sectionToolbar = ui::column({
        .out = &m_sectionToolbar,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .visible = barOn,
        .participatesInLayout = barOn,
    });
    sectionToolbar->addChild(
        ui::segmented({
            .out = &m_sectionSwitch,
            .options =
                std::vector<ui::SegmentedOption>{
                    {
                        .label = "",
                        .glyph = "crop-3-2",
                        .tooltip = i18n::tr("control-center.bar.layout-tooltip"),
                    },
                    {
                        .label = "",
                        .glyph = "resize",
                        .tooltip = i18n::tr("control-center.bar.shape-tooltip"),
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

  const bool layoutOpen = barOn && m_section == Section::Layout;
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
  addCaption(*layoutHost, i18n::tr("control-center.bar.position"), scale);
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
          .selectedIndex = positionIndex(barRef.position),
          .fontSize = captionSize,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = opacity,
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyPosition(index); },
      })
  );
  addCaption(*layoutHost, i18n::tr("control-center.bar.auto-hide"), scale);
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
          .selectedIndex = autoHideIndex(barRef),
          .fontSize = captionSize,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = opacity,
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyAutoHide(index); },
      })
  );
  m_reserveSpace = addToggleRow(
      *layoutHost, scale, i18n::tr("control-center.bar.reserve-space"), [this](bool value) { applyReserveSpace(value); }
  );
  if (m_reserveSpace != nullptr) {
    m_reserveSpace->setCheckedImmediate(barRef.reserveSpace);
  }
  live->addChild(std::move(layoutHost));

  const bool shapeOpen = barOn && m_section == Section::Shape;
  auto shapeHost = ui::column({
      .out = &m_shapeHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = hosted ? 0.0F : 1.0F,
      .visible = shapeOpen,
      .participatesInLayout = shapeOpen,
      .configure =
          [scale, hosted](Flex& section) {
            if (!hosted) {
              applyContentCardStyle(section, scale);
            }
          },
  });
  addSliderSection(
      *shapeHost, scale, i18n::tr("control-center.bar.thickness"), &m_thickness, &m_thicknessValue, 10.0, 120.0, 1.0,
      static_cast<double>(barRef.thickness), pixelLabel, [this]() { commitThickness(); }
  );
  addSliderSection(
      *shapeHost, scale, i18n::tr("control-center.bar.opacity"), &m_opacity, &m_opacityValue, 0.0, 1.0, 0.01,
      static_cast<double>(barRef.backgroundOpacity), percentLabel, [this]() { commitOpacity(); }
  );
  m_shadow = addToggleRow(*shapeHost, scale, i18n::tr("control-center.bar.shadow"), [this](bool value) {
    applyShadow(value);
  });
  if (m_shadow != nullptr) {
    m_shadow->setCheckedImmediate(barRef.shadow);
  }
  live->addChild(std::move(shapeHost));

  syncLiveControls();
  return live;
}

std::unique_ptr<Flex> BarTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void BarTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_enableRow = nullptr;
  m_sectionToolbar = nullptr;
  m_layoutHost = nullptr;
  m_shapeHost = nullptr;
  m_enabled = nullptr;
  m_sectionSwitch = nullptr;
  m_position = nullptr;
  m_autoHide = nullptr;
  m_reserveSpace = nullptr;
  m_shadow = nullptr;
  m_thickness = nullptr;
  m_thicknessValue = nullptr;
  m_opacity = nullptr;
  m_opacityValue = nullptr;
  m_empty = nullptr;
  m_caption = nullptr;
}

void BarTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool BarTab::configureMode() const { return m_settings->configureMode(); }

void BarTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

SettingsPane* BarTab::settingsPane() { return m_settings.get(); }

void BarTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
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

void BarTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
}

void BarTab::onPanelCardOpacityChanged(float opacity) {
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

void BarTab::syncHosts() {
  const BarConfig* bar = m_config != nullptr ? firstBar(m_config->config()) : nullptr;
  const bool hasBar = bar != nullptr;
  const bool barOn = hasBar && bar->enabled;
  const bool layoutOpen = barOn && m_section == Section::Layout;
  const bool shapeOpen = barOn && m_section == Section::Shape;
  bool layoutDirty = false;
  layoutDirty |= setShown(m_empty, !hasBar);
  layoutDirty |= setShown(m_enableRow, hasBar);
  layoutDirty |= setShown(m_sectionToolbar, barOn);
  layoutDirty |= setShown(m_layoutHost, layoutOpen);
  layoutDirty |= setShown(m_shapeHost, shapeOpen);
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
}

void BarTab::persistSection() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("bar_tab", "section", std::string(idFromSection(m_section)));
}

void BarTab::applySection(std::size_t index) {
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

void BarTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  const BarConfig* bar = firstBar(m_config->config());
  const bool hasBar = bar != nullptr;
  syncHosts();
  const auto setInteractive = [hasBar](auto* control) {
    if (control != nullptr) {
      control->setEnabled(hasBar);
    }
  };
  setInteractive(m_enabled);
  setInteractive(m_position);
  setInteractive(m_autoHide);
  setInteractive(m_reserveSpace);
  setInteractive(m_shadow);
  setInteractive(m_thickness);
  setInteractive(m_opacity);
  if (m_caption != nullptr) {
    m_caption->setText(
        m_section == Section::Shape ? i18n::tr("control-center.bar.shape-caption")
                                    : i18n::tr("control-center.bar.layout-caption")
    );
  }
  if (!hasBar) {
    m_barName.clear();
    return;
  }
  m_barName = bar->name;
  m_syncing = true;
  if (m_enabled != nullptr) {
    m_enabled->setCheckedImmediate(bar->enabled);
  }
  if (m_position != nullptr) {
    m_position->setSelectedIndex(positionIndex(bar->position));
  }
  if (m_autoHide != nullptr) {
    m_autoHide->setSelectedIndex(autoHideIndex(*bar));
  }
  if (m_reserveSpace != nullptr) {
    m_reserveSpace->setCheckedImmediate(bar->reserveSpace);
  }
  if (m_shadow != nullptr) {
    m_shadow->setCheckedImmediate(bar->shadow);
  }
  if (m_thickness != nullptr) {
    m_thickness->setValue(bar->thickness);
  }
  if (m_thicknessValue != nullptr) {
    m_thicknessValue->setText(pixelLabel(bar->thickness));
  }
  if (m_opacity != nullptr) {
    m_opacity->setValue(bar->backgroundOpacity);
  }
  if (m_opacityValue != nullptr) {
    m_opacityValue->setText(percentLabel(bar->backgroundOpacity));
  }
  m_syncing = false;
}

void BarTab::applyEnabled(bool enabled) {
  if (m_syncing || m_config == nullptr || m_barName.empty()) {
    return;
  }
  (void)m_config->setOverride({"bar", m_barName, "enabled"}, enabled);
  syncLiveControls();
  PanelManager::instance().refresh();
}

void BarTab::applyPosition(std::size_t index) {
  if (m_syncing || m_config == nullptr || m_barName.empty()) {
    return;
  }
  const std::string value = index < kPositionValues.size() ? kPositionValues[index] : kPositionValues[0];
  (void)m_config->setOverride({"bar", m_barName, "position"}, value);
  PanelManager::instance().refresh();
}

void BarTab::applyAutoHide(std::size_t index) {
  if (m_syncing || m_config == nullptr || m_barName.empty()) {
    return;
  }
  (void)m_config->setOverrides({
      {{"bar", m_barName, "auto_hide"}, index == kAutoHideOn},
      {{"bar", m_barName, "smart_auto_hide"}, index == kAutoHideSmart},
  });
  PanelManager::instance().refresh();
}

void BarTab::applyReserveSpace(bool enabled) {
  if (m_syncing || m_config == nullptr || m_barName.empty()) {
    return;
  }
  (void)m_config->setOverride({"bar", m_barName, "reserve_space"}, enabled);
  PanelManager::instance().refresh();
}

void BarTab::applyShadow(bool enabled) {
  if (m_syncing || m_config == nullptr || m_barName.empty()) {
    return;
  }
  (void)m_config->setOverride({"bar", m_barName, "shadow"}, enabled);
  PanelManager::instance().refresh();
}

void BarTab::commitThickness() {
  if (m_syncing || m_config == nullptr || m_thickness == nullptr || m_barName.empty()) {
    return;
  }
  (void)m_config->setOverride({"bar", m_barName, "thickness"}, static_cast<std::int64_t>(m_thickness->value()));
}

void BarTab::commitOpacity() {
  if (m_syncing || m_config == nullptr || m_opacity == nullptr || m_barName.empty()) {
    return;
  }
  (void)m_config->setOverride({"bar", m_barName, "background_opacity"}, m_opacity->value());
}
