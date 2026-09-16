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
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "roundness"})) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> RoundnessTab::create() {
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
  addTitle(*card, i18n::tr("settings.schema.appearance.corner-roundness.label"), scale);
  auto roundnessHeader = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });
  roundnessHeader->addChild(
      ui::label({
          .text = i18n::tr("settings.schema.appearance.corner-roundness.description"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
          .flexGrow = 1.0F,
      })
  );
  roundnessHeader->addChild(
      ui::label({
          .out = &m_roundnessValue,
          .text = multiplierLabel(1.0),
          .fontSize = captionSize,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  card->addChild(std::move(roundnessHeader));
  card->addChild(
      ui::slider({
          .out = &m_roundness,
          .minValue = noctalia::config::schema::kCornerRadiusScaleRange.min.value_or(0.0F),
          .maxValue = noctalia::config::schema::kCornerRadiusScaleRange.max.value_or(2.0F),
          .step = noctalia::config::schema::kCornerRadiusScaleRange.step.value_or(0.05F),
          .value = 1.0,
          .trackHeight = Style::sliderTrackHeight * scale,
          .thumbSize = Style::sliderThumbSize * scale,
          .controlHeight = controlHeight,
          .flexGrow = 1.0F,
          .onValueChanged =
              [this](double value) {
                if (m_roundnessValue != nullptr) {
                  m_roundnessValue->setText(multiplierLabel(value));
                }
              },
          .onDragEnd = [this]() { commitRoundness(); },
      })
  );
  content->addChild(std::move(card));

  auto barCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addTitle(*barCard, i18n::tr("settings.navigation.sections.bar"), scale);
  auto barRadiusHeader = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });
  barRadiusHeader->addChild(
      ui::label({
          .out = &m_barDescription,
          .text = i18n::tr("settings.schema.bar.corner-radius.description"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
          .flexGrow = 1.0F,
      })
  );
  barRadiusHeader->addChild(
      ui::label({
          .out = &m_barRadiusValue,
          .text = pixelLabel(0.0),
          .fontSize = captionSize,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  barCard->addChild(std::move(barRadiusHeader));
  barCard->addChild(
      ui::slider({
          .out = &m_barRadius,
          .minValue = 0.0,
          .maxValue = 80.0,
          .step = 1.0,
          .value = 0.0,
          .trackHeight = Style::sliderTrackHeight * scale,
          .thumbSize = Style::sliderThumbSize * scale,
          .controlHeight = controlHeight,
          .flexGrow = 1.0F,
          .onValueChanged =
              [this](double value) {
                if (m_barRadiusValue != nullptr) {
                  m_barRadiusValue->setText(pixelLabel(value));
                }
              },
          .onDragEnd = [this]() { commitBarRadius(); },
      })
  );
  content->addChild(std::move(barCard));

  auto dockCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addTitle(*dockCard, i18n::tr("settings.navigation.sections.dock"), scale);
  auto dockRadiusHeader = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });
  dockRadiusHeader->addChild(
      ui::label({
          .text = i18n::tr("settings.schema.dock.corner-radius.description"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
          .flexGrow = 1.0F,
      })
  );
  dockRadiusHeader->addChild(
      ui::label({
          .out = &m_dockRadiusValue,
          .text = pixelLabel(0.0),
          .fontSize = captionSize,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  dockCard->addChild(std::move(dockRadiusHeader));
  dockCard->addChild(
      ui::slider({
          .out = &m_dockRadius,
          .minValue = static_cast<double>(noctalia::config::schema::kDockRadiusRange.min.value_or(0)),
          .maxValue = static_cast<double>(noctalia::config::schema::kDockRadiusRange.max.value_or(80)),
          .step = static_cast<double>(noctalia::config::schema::kDockRadiusRange.step.value_or(1)),
          .value = 0.0,
          .trackHeight = Style::sliderTrackHeight * scale,
          .thumbSize = Style::sliderThumbSize * scale,
          .controlHeight = controlHeight,
          .flexGrow = 1.0F,
          .onValueChanged =
              [this](double value) {
                if (m_dockRadiusValue != nullptr) {
                  m_dockRadiusValue->setText(pixelLabel(value));
                }
              },
          .onDragEnd = [this]() { commitDockRadius(); },
      })
  );
  content->addChild(std::move(dockCard));

  if (compositors::isNiri()) {
    auto windowCard = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .configure =
            [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
    });
    addTitle(*windowCard, i18n::tr("control-center.roundness.windows-title"), scale);
    auto windowRadiusHeader = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
    });
    windowRadiusHeader->addChild(
        ui::label({
            .text = i18n::tr("control-center.roundness.windows-description"),
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 2,
            .flexGrow = 1.0F,
        })
    );
    windowRadiusHeader->addChild(
        ui::label({
            .out = &m_windowRadiusValue,
            .text = pixelLabel(0.0),
            .fontSize = captionSize,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    windowCard->addChild(std::move(windowRadiusHeader));
    windowCard->addChild(
        ui::slider({
            .out = &m_windowRadius,
            .minValue = 0.0,
            .maxValue = 40.0,
            .step = 1.0,
            .value = 0.0,
            .trackHeight = Style::sliderTrackHeight * scale,
            .thumbSize = Style::sliderThumbSize * scale,
            .controlHeight = controlHeight,
            .flexGrow = 1.0F,
            .onValueChanged =
                [this](double value) {
                  if (m_windowRadiusValue != nullptr) {
                    m_windowRadiusValue->setText(pixelLabel(value));
                  }
                },
            .onDragEnd = [this]() { commitWindowRadius(); },
        })
    );
    content->addChild(std::move(windowCard));
  }

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
  m_barRadius = nullptr;
  m_barRadiusValue = nullptr;
  m_barDescription = nullptr;
  m_dockRadius = nullptr;
  m_dockRadiusValue = nullptr;
  m_windowRadius = nullptr;
  m_windowRadiusValue = nullptr;
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
