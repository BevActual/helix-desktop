#include "shell/control_center/tabs/scale_tab.h"

#include "config/config_service.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
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
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "scale", .showConfigureButton = false}
      )) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> ScaleTab::create() {
  return m_settings->wrapLive(createLiveBody(), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> ScaleTab::createLiveBody(bool hosted) {
  const float scale = contentScale();

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
  addSliderSection(
      *compactCard, scale, i18n::tr("settings.schema.appearance.ui-scale.label"), &m_uiScale, &m_uiScaleValue,
      noctalia::config::schema::kScaleRange.min.value_or(0.5F),
      noctalia::config::schema::kScaleRange.max.value_or(2.5F),
      noctalia::config::schema::kScaleRange.step.value_or(0.05F), 1.0, multiplierLabel, [this]() { commitUiScale(); }
  );
  addCaption(*compactCard, i18n::tr("settings.schema.appearance.ui-scale.description"), scale);
  tab->addChild(std::move(compactCard));

  syncLiveControls();
  return tab;
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
