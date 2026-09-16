#include "shell/control_center/tabs/scale_tab.h"

#include "config/config_service.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <format>
#include <string>
#include <utility>

using namespace control_center;

namespace {

  [[nodiscard]] std::string multiplierLabel(double value) { return std::format("{:.2f}×", value); }

} // namespace

ScaleTab::ScaleTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "scale"})) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> ScaleTab::create() {
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
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  auto* content = scroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Style::spaceMd * scale);

  auto card = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addTitle(*card, i18n::tr("settings.schema.appearance.ui-scale.label"), scale);
  card->addChild(
      ui::label({
          .text = i18n::tr("settings.schema.appearance.ui-scale.description"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
      })
  );

  auto scaleHeader = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });
  scaleHeader->addChild(
      ui::label({
          .text = i18n::tr("settings.schema.appearance.ui-scale.label"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .flexGrow = 1.0F,
      })
  );
  scaleHeader->addChild(
      ui::label({
          .out = &m_uiScaleValue,
          .text = multiplierLabel(1.0),
          .fontSize = captionSize,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  card->addChild(std::move(scaleHeader));
  card->addChild(
      ui::slider({
          .out = &m_uiScale,
          .minValue = noctalia::config::schema::kScaleRange.min.value_or(0.5F),
          .maxValue = noctalia::config::schema::kScaleRange.max.value_or(2.5F),
          .step = noctalia::config::schema::kScaleRange.step.value_or(0.05F),
          .value = 1.0,
          .trackHeight = Style::sliderTrackHeight * scale,
          .thumbSize = Style::sliderThumbSize * scale,
          .controlHeight = controlHeight,
          .flexGrow = 1.0F,
          .onValueChanged =
              [this](double value) {
                if (m_uiScaleValue != nullptr) {
                  m_uiScaleValue->setText(multiplierLabel(value));
                }
              },
          .onDragEnd = [this]() { commitUiScale(); },
      })
  );
  content->addChild(std::move(card));

  live->addChild(std::move(scroll));
  auto wrapped = m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
}

std::unique_ptr<Flex> ScaleTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void ScaleTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_uiScale = nullptr;
  m_uiScaleValue = nullptr;
}

void ScaleTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool ScaleTab::configureMode() const { return m_settings->configureMode(); }

void ScaleTab::setSettingsSearchQuery(std::string_view query) {
  if (!query.empty()) {
    m_settings->setConfigureMode(true);
  }
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* ScaleTab::settingsPane() { return m_settings.get(); }

void ScaleTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void ScaleTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
}

void ScaleTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  m_syncing = true;
  const float uiScale = m_config->config().accessibility.uiScale;
  if (m_uiScale != nullptr) {
    m_uiScale->setValue(uiScale);
  }
  if (m_uiScaleValue != nullptr) {
    m_uiScaleValue->setText(multiplierLabel(uiScale));
  }
  m_syncing = false;
}

void ScaleTab::commitUiScale() {
  if (m_syncing || m_config == nullptr || m_uiScale == nullptr) {
    return;
  }
  (void)m_config->setOverride({"accessibility", "ui_scale"}, m_uiScale->value());
}
