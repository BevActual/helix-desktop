#include "shell/control_center/tabs/scale_tab.h"

#include "config/config_service.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
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
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "scale", .showConfigureButton = false}
      )) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> ScaleTab::create() {
  const float scale = contentScale();

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
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addSliderSection(
      *card, scale, i18n::tr("settings.schema.appearance.ui-scale.label"), &m_uiScale, &m_uiScaleValue,
      noctalia::config::schema::kScaleRange.min.value_or(0.5F),
      noctalia::config::schema::kScaleRange.max.value_or(2.5F),
      noctalia::config::schema::kScaleRange.step.value_or(0.05F), 1.0, multiplierLabel, [this]() { commitUiScale(); }
  );
  auto preview = ui::row({
      .align = FlexAlign::End,
      .gap = Style::spaceMd * scale,
  });
  auto makeScaleSpecimen = [&](Label** labelOut, float fontSize, FontWeight weight, const std::string& caption) {
    auto cell = ui::column({
        .align = FlexAlign::Center,
        .gap = Style::spaceXs * scale,
        .flexGrow = 1.0F,
    });
    cell->addChild(
        ui::label({
            .out = labelOut,
            .text = "Aa",
            .fontSize = fontSize,
            .fontWeight = weight,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .textAlign = TextAlign::Center,
        })
    );
    cell->addChild(
        ui::label({
            .text = caption,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
            .textAlign = TextAlign::Center,
        })
    );
    return cell;
  };
  preview->addChild(
      makeScaleSpecimen(
          &m_scalePreviewCaption, Style::fontSizeCaption * scale, FontWeight::Normal,
          i18n::tr("control-center.appearance.specimen-caption")
      )
  );
  preview->addChild(
      makeScaleSpecimen(
          &m_scalePreviewBody, Style::fontSizeBody * scale, FontWeight::Normal,
          i18n::tr("control-center.appearance.specimen-body")
      )
  );
  preview->addChild(
      makeScaleSpecimen(
          &m_scalePreviewTitle, Style::fontSizeTitle * scale, FontWeight::Medium,
          i18n::tr("control-center.appearance.specimen-title")
      )
  );
  card->addChild(std::move(preview));
  card->addChild(
      ui::label({
          .out = &m_scalePreviewPangram,
          .text = i18n::tr("control-center.appearance.font-preview"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 2,
      })
  );
  addCaption(*card, i18n::tr("settings.schema.appearance.ui-scale.description"), scale);
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
  m_scalePreviewCaption = nullptr;
  m_scalePreviewBody = nullptr;
  m_scalePreviewTitle = nullptr;
  m_scalePreviewPangram = nullptr;
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
  updateScalePreview();
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
  m_previewScale = -1.0F;
  updateScalePreview();
  m_syncing = false;
}

void ScaleTab::updateScalePreview() {
  const float next = m_uiScale != nullptr ? static_cast<float>(m_uiScale->value()) : contentScale();
  if (m_previewScale == next) {
    return;
  }
  m_previewScale = next;
  if (m_scalePreviewCaption != nullptr) {
    m_scalePreviewCaption->setFontSize(Style::fontSizeCaption * next);
  }
  if (m_scalePreviewBody != nullptr) {
    m_scalePreviewBody->setFontSize(Style::fontSizeBody * next);
  }
  if (m_scalePreviewTitle != nullptr) {
    m_scalePreviewTitle->setFontSize(Style::fontSizeTitle * next);
  }
  if (m_scalePreviewPangram != nullptr) {
    m_scalePreviewPangram->setFontSize(Style::fontSizeBody * next);
  }
  PanelManager::instance().requestLayout();
}

void ScaleTab::commitUiScale() {
  if (m_syncing || m_config == nullptr || m_uiScale == nullptr) {
    return;
  }
  (void)m_config->setOverride({"accessibility", "ui_scale"}, m_uiScale->value());
}
