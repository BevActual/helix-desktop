#include "shell/control_center/tabs/roundness_tab.h"

#include "compositors/compositor_detect.h"
#include "compositors/niri/niri_window_rules.h"
#include "config/config_service.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>

using namespace control_center;

namespace {

  [[nodiscard]] std::string multiplierLabel(double value) { return std::format("{:.2f}×", value); }
  [[nodiscard]] std::string pixelLabel(double value) { return std::format("{}px", static_cast<int>(value)); }

} // namespace

RoundnessTab::RoundnessTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "roundness", .showConfigureButton = false}
      )) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> RoundnessTab::create() {
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

  const float opacity = panelCardOpacity();
  auto card = ui::column({
      .align = FlexAlign::Stretch,
      .configure = [scale, opacity](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });

  addSliderSection(
      *card, scale, i18n::tr("settings.schema.appearance.corner-roundness.label"), &m_roundness, &m_roundnessValue,
      noctalia::config::schema::kCornerRadiusScaleRange.min.value_or(0.0F),
      noctalia::config::schema::kCornerRadiusScaleRange.max.value_or(2.0F),
      noctalia::config::schema::kCornerRadiusScaleRange.step.value_or(0.05F), 1.0, multiplierLabel,
      [this]() { commitRoundness(); }
  );
  auto preview = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceMd * scale,
  });
  const float chip = Style::controlHeight * scale;
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
            .fontSize = Style::fontSizeCaption * scale,
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
  card->addChild(std::move(preview));
  addCaption(*card, i18n::tr("settings.schema.appearance.corner-roundness.description"), scale);

  addWellDivider(*card, scale);
  addSliderSection(
      *card, scale, i18n::tr("settings.navigation.sections.bar"), &m_barRadius, &m_barRadiusValue, 0.0, 80.0, 1.0, 0.0,
      pixelLabel, [this]() { commitBarRadius(); }
  );
  card->addChild(
      ui::box({
          .out = &m_barChip,
          .fill = colorSpecFromRole(ColorRole::Surface),
          .border = colorSpecFromRole(ColorRole::Outline, 0.45F),
          .borderWidth = 1.0F,
          .radius = Style::scaledRadiusMd(scale),
          .width = Style::controlHeight * 2.75F * scale,
          .height = Style::controlHeight * 0.5F * scale,
      })
  );
  m_barDescription = addCaption(*card, i18n::tr("settings.schema.bar.corner-radius.description"), scale);

  addWellDivider(*card, scale);
  addSliderSection(
      *card, scale, i18n::tr("settings.navigation.sections.dock"), &m_dockRadius, &m_dockRadiusValue,
      static_cast<double>(noctalia::config::schema::kDockRadiusRange.min.value_or(0)),
      static_cast<double>(noctalia::config::schema::kDockRadiusRange.max.value_or(80)),
      static_cast<double>(noctalia::config::schema::kDockRadiusRange.step.value_or(1)), 0.0, pixelLabel,
      [this]() { commitDockRadius(); }
  );
  card->addChild(
      ui::box({
          .out = &m_dockChip,
          .fill = colorSpecFromRole(ColorRole::Surface),
          .border = colorSpecFromRole(ColorRole::Outline, 0.45F),
          .borderWidth = 1.0F,
          .radius = Style::scaledRadiusMd(scale),
          .width = Style::controlHeight * scale,
          .height = Style::controlHeight * scale,
      })
  );
  addCaption(*card, i18n::tr("settings.schema.dock.corner-radius.description"), scale);

  if (compositors::isNiri()) {
    addWellDivider(*card, scale);
    addSliderSection(
        *card, scale, i18n::tr("control-center.roundness.windows-title"), &m_windowRadius, &m_windowRadiusValue, 0.0,
        40.0, 1.0, 0.0, pixelLabel, [this]() { commitWindowRadius(); }
    );
    card->addChild(
        ui::box({
            .out = &m_windowChip,
            .fill = colorSpecFromRole(ColorRole::Surface),
            .border = colorSpecFromRole(ColorRole::Outline, 0.45F),
            .borderWidth = 1.0F,
            .radius = Style::scaledRadiusMd(scale),
            .width = Style::controlHeight * 1.75F * scale,
            .height = Style::controlHeight * scale,
        })
    );
    addCaption(*card, i18n::tr("control-center.roundness.windows-description"), scale);
  }

  content->addChild(std::move(card));

  live->addChild(std::move(scroll));
  auto wrapped = m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
}

std::unique_ptr<Flex> RoundnessTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void RoundnessTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_roundness = nullptr;
  m_roundnessValue = nullptr;
  m_radiusChipSm = nullptr;
  m_radiusChipMd = nullptr;
  m_radiusChip2xl = nullptr;
  m_barRadius = nullptr;
  m_barRadiusValue = nullptr;
  m_barDescription = nullptr;
  m_barChip = nullptr;
  m_dockRadius = nullptr;
  m_dockRadiusValue = nullptr;
  m_dockChip = nullptr;
  m_windowRadius = nullptr;
  m_windowRadiusValue = nullptr;
  m_windowChip = nullptr;
}

void RoundnessTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool RoundnessTab::configureMode() const { return m_settings->configureMode(); }

void RoundnessTab::setSettingsSearchQuery(std::string_view query) {
  if (!query.empty()) {
    m_settings->setConfigureMode(true);
  }
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
