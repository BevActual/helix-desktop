#include "shell/control_center/tabs/panels_tab.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
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
#include <string>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  constexpr std::array<const char*, 2> kLayerValues = {"top", "overlay"};

  template <std::size_t N>
  [[nodiscard]] std::size_t indexOf(const std::array<const char*, N>& values, const std::string& value) {
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (value == values[i]) {
        return i;
      }
    }
    return 0;
  }

} // namespace

PanelsTab::PanelsTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "panels"})) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> PanelsTab::create() {
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
                                    void (PanelsTab::*apply)(bool)) {
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

  const ShellConfig::PanelConfig panel = m_config != nullptr ? m_config->config().shell.panel : ShellConfig::PanelConfig{};

  auto generalCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addTitle(*generalCard, i18n::tr("settings.navigation.sections.panels"), scale);
  addCaption(*generalCard, i18n::tr("settings.schema.panels.floating-layer.label"));
  generalCard->addChild(
      ui::segmented({
          .out = &m_floatingLayer,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = i18n::tr("settings.options.layer.top"),
                      .tooltip = i18n::tr("control-center.panels.layer-top-tooltip"),
                  },
                  {
                      .label = i18n::tr("settings.options.layer.overlay"),
                      .tooltip = i18n::tr("control-center.panels.layer-overlay-tooltip"),
                  },
              },
          .selectedIndex = indexOf(kLayerValues, panel.floatingLayer),
          .fontSize = captionSize,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyFloatingLayer(index); },
      })
  );
  content->addChild(std::move(generalCard));

  auto effectsCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addToggleRow(*effectsCard, i18n::tr("settings.schema.panels.borders.label"), &m_borders, panel.borders, &PanelsTab::applyBorders);
  addToggleRow(*effectsCard, i18n::tr("settings.schema.shared.shadow.label"), &m_shadow, panel.shadow, &PanelsTab::applyShadow);
  addToggleRow(
      *effectsCard, i18n::tr("control-center.panels.list-rows"), &m_listItemBackground,
      panel.listItemBackground, &PanelsTab::applyListItemBackground
  );

  auto offsetHeader = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  offsetHeader->addChild(
      ui::label({
          .text = i18n::tr("settings.schema.panels.floating-offset.label"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .flexGrow = 1.0F,
      })
  );
  offsetHeader->addChild(
      ui::label({
          .out = &m_floatingOffsetValue,
          .text = std::to_string(panel.floatingOffset) + "px",
          .fontSize = captionSize,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  effectsCard->addChild(std::move(offsetHeader));
  effectsCard->addChild(
      ui::slider({
          .out = &m_floatingOffset,
          .minValue = 0.0,
          .maxValue = 100.0,
          .step = 1.0,
          .value = static_cast<double>(panel.floatingOffset),
          .trackHeight = Style::sliderTrackHeight * scale,
          .thumbSize = Style::sliderThumbSize * scale,
          .controlHeight = controlHeight,
          .flexGrow = 1.0F,
          .onValueChanged =
              [this](double value) {
                if (m_floatingOffsetValue != nullptr) {
                  m_floatingOffsetValue->setText(std::to_string(static_cast<int>(value)) + "px");
                }
              },
          .onDragEnd = [this]() { commitFloatingOffset(); },
      })
  );
  content->addChild(std::move(effectsCard));

  live->addChild(std::move(scroll));
  auto wrapped = m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
}

std::unique_ptr<Flex> PanelsTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void PanelsTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_floatingLayer = nullptr;
  m_borders = nullptr;
  m_shadow = nullptr;
  m_listItemBackground = nullptr;
  m_floatingOffset = nullptr;
  m_floatingOffsetValue = nullptr;
}

void PanelsTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool PanelsTab::configureMode() const { return m_settings->configureMode(); }

void PanelsTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

SettingsPane* PanelsTab::settingsPane() { return m_settings.get(); }

void PanelsTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void PanelsTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
}

void PanelsTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  const ShellConfig::PanelConfig& panel = m_config->config().shell.panel;
  m_syncing = true;
  if (m_floatingLayer != nullptr) {
    m_floatingLayer->setSelectedIndex(indexOf(kLayerValues, panel.floatingLayer));
  }
  if (m_borders != nullptr) {
    m_borders->setCheckedImmediate(panel.borders);
  }
  if (m_shadow != nullptr) {
    m_shadow->setCheckedImmediate(panel.shadow);
  }
  if (m_listItemBackground != nullptr) {
    m_listItemBackground->setCheckedImmediate(panel.listItemBackground);
  }
  if (m_floatingOffset != nullptr) {
    m_floatingOffset->setValue(panel.floatingOffset);
  }
  if (m_floatingOffsetValue != nullptr) {
    m_floatingOffsetValue->setText(std::to_string(panel.floatingOffset) + "px");
  }
  m_syncing = false;
}

void PanelsTab::applyFloatingLayer(std::size_t index) {
  if (m_syncing || m_config == nullptr || index >= kLayerValues.size()) {
    return;
  }
  (void)m_config->setOverride({"shell", "panel", "floating_layer"}, std::string(kLayerValues[index]));
}

void PanelsTab::applyBorders(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "panel", "borders"}, enabled);
}

void PanelsTab::applyShadow(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "panel", "shadow"}, enabled);
}

void PanelsTab::applyListItemBackground(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "panel", "list_item_background"}, enabled);
}

void PanelsTab::commitFloatingOffset() {
  if (m_syncing || m_config == nullptr || m_floatingOffset == nullptr) {
    return;
  }
  (void)m_config->setOverride(
      {"shell", "panel", "floating_offset"}, static_cast<std::int64_t>(m_floatingOffset->value())
  );
}
