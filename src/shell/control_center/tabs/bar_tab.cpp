#include "shell/control_center/tabs/bar_tab.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

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

BarTab::BarTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "bar"})) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> BarTab::create() {
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;
  const float controlHeight = Style::controlHeight * scale;

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
                                    void (BarTab::*apply)(bool)) {
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
            .onChange = [this, apply](bool value) { (this->*apply)(value); },
        })
    );
    parent.addChild(std::move(row));
  };

  const BarConfig* bar = m_config != nullptr ? firstBar(m_config->config()) : nullptr;
  const BarConfig defaultBar{};
  const BarConfig& barRef = bar != nullptr ? *bar : defaultBar;
  if (bar != nullptr) {
    m_barName = bar->name;
  }

  auto generalCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addTitle(*generalCard, i18n::tr("settings.navigation.sections.bar"), scale);
  addToggleRow(*generalCard, i18n::tr("control-center.bar.enable"), &m_enabled, barRef.enabled, &BarTab::applyEnabled);

  addCaption(*generalCard, i18n::tr("control-center.bar.position"));
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
          .selectedIndex = positionIndex(barRef.position),
          .fontSize = captionSize,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyPosition(index); },
      })
  );

  addCaption(*generalCard, i18n::tr("control-center.bar.auto-hide"));
  generalCard->addChild(
      ui::segmented({
          .out = &m_autoHide,
          .options =
              std::vector<ui::SegmentedOption>{
                  {.label = i18n::tr("settings.options.bar.auto-hide.off")},
                  {.label = i18n::tr("settings.options.bar.auto-hide.on")},
                  {.label = i18n::tr("settings.options.bar.auto-hide.smart")},
              },
          .selectedIndex = autoHideIndex(barRef),
          .fontSize = captionSize,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyAutoHide(index); },
      })
  );
  addToggleRow(
      *generalCard, i18n::tr("control-center.bar.reserve-space"), &m_reserveSpace, barRef.reserveSpace,
      &BarTab::applyReserveSpace
  );
  addToggleRow(*generalCard, i18n::tr("control-center.bar.shadow"), &m_shadow, barRef.shadow, &BarTab::applyShadow);
  content->addChild(std::move(generalCard));

  auto shapeCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addTitle(*shapeCard, i18n::tr("control-center.bar.thickness"), scale);

  auto thicknessHeader = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  thicknessHeader->addChild(
      ui::label({
          .text = i18n::tr("control-center.bar.thickness"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .flexGrow = 1.0F,
      })
  );
  thicknessHeader->addChild(
      ui::label({
          .out = &m_thicknessValue,
          .text = pixelLabel(static_cast<double>(barRef.thickness)),
          .fontSize = captionSize,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  shapeCard->addChild(std::move(thicknessHeader));
  shapeCard->addChild(
      ui::slider({
          .out = &m_thickness,
          .minValue = 10.0,
          .maxValue = 120.0,
          .step = 1.0,
          .value = static_cast<double>(barRef.thickness),
          .trackHeight = Style::sliderTrackHeight * scale,
          .thumbSize = Style::sliderThumbSize * scale,
          .controlHeight = controlHeight,
          .flexGrow = 1.0F,
          .onValueChanged =
              [this](double value) {
                if (m_thicknessValue != nullptr) {
                  m_thicknessValue->setText(pixelLabel(value));
                }
              },
          .onDragEnd = [this]() { commitThickness(); },
      })
  );

  auto opacityHeader = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  opacityHeader->addChild(
      ui::label({
          .text = i18n::tr("control-center.bar.opacity"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .flexGrow = 1.0F,
      })
  );
  opacityHeader->addChild(
      ui::label({
          .out = &m_opacityValue,
          .text = percentLabel(static_cast<double>(barRef.backgroundOpacity)),
          .fontSize = captionSize,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  shapeCard->addChild(std::move(opacityHeader));
  shapeCard->addChild(
      ui::slider({
          .out = &m_opacity,
          .minValue = 0.0,
          .maxValue = 1.0,
          .step = 0.01,
          .value = static_cast<double>(barRef.backgroundOpacity),
          .trackHeight = Style::sliderTrackHeight * scale,
          .thumbSize = Style::sliderThumbSize * scale,
          .controlHeight = controlHeight,
          .flexGrow = 1.0F,
          .onValueChanged =
              [this](double value) {
                if (m_opacityValue != nullptr) {
                  m_opacityValue->setText(percentLabel(value));
                }
              },
          .onDragEnd = [this]() { commitOpacity(); },
      })
  );
  content->addChild(std::move(shapeCard));

  live->addChild(std::move(scroll));
  auto wrapped = m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
}

std::unique_ptr<Flex> BarTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void BarTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_enabled = nullptr;
  m_position = nullptr;
  m_autoHide = nullptr;
  m_reserveSpace = nullptr;
  m_shadow = nullptr;
  m_thickness = nullptr;
  m_thicknessValue = nullptr;
  m_opacity = nullptr;
  m_opacityValue = nullptr;
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

void BarTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  const BarConfig* bar = firstBar(m_config->config());
  if (bar == nullptr) {
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
  (void)m_config->setOverride(
      {"bar", m_barName, "thickness"}, static_cast<std::int64_t>(m_thickness->value())
  );
}

void BarTab::commitOpacity() {
  if (m_syncing || m_config == nullptr || m_opacity == nullptr || m_barName.empty()) {
    return;
  }
  (void)m_config->setOverride({"bar", m_barName, "background_opacity"}, m_opacity->value());
}
