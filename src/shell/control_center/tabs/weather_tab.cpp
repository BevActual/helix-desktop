#include "shell/control_center/tabs/weather_tab.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/animation/animation.h"
#include "render/animation/animation_manager.h"
#include "render/scene/effect_node.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "shell/tooltip/tooltip_content.h"
#include "system/weather_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/toggle.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <memory>
#include <string_view>

using namespace control_center;

namespace {

  // Set to a specific effect to bypass weather-code detection. Reset to None when done testing.
  constexpr EffectType kTestEffect = EffectType::None;

  constexpr float kCurrentGlyphSize = Style::controlHeightLg * 1.6F;

  void requestPanelLayout() {
    if (auto* panels = PanelManager::current(); panels != nullptr) {
      panels->requestLayout();
    }
  }

  [[nodiscard]] bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return changed;
  }

  std::string windDirectionLabel(int degrees) {
    static constexpr std::array<const char*, 8> kDirs = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const int normalized = ((degrees % 360) + 360) % 360;
    const int index = static_cast<int>(std::lround(normalized / 45.0)) % 8;
    return kDirs[static_cast<std::size_t>(index)];
  }

  // Location is the compact identity when the user shows it. Otherwise last-updated is the
  // caption. Never fall back to the tab title already in the panel header.
  void applyCompactMeta(
      Label* label, bool showLocation, std::string_view locationName, bool haveFetch,
      std::chrono::system_clock::time_point fetchedAt
  ) {
    std::string text;
    if (showLocation && !locationName.empty()) {
      text.assign(locationName);
    } else if (haveFetch) {
      text = formatTimeAgo(fetchedAt);
    }
    if (label != nullptr) {
      label->setText(text.empty() ? " " : text);
    }
    (void)setShown(label, !text.empty());
  }

} // namespace

WeatherTab::WeatherTab(WeatherService* weather, ConfigService* config, const ControlCenterServices& services)
    : m_weather(weather), m_config(config),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "location", .includedGroups = {"location", "weather"}}
      )) {
  m_forecastRows.fill(nullptr);
  m_forecastSeparators.fill(nullptr);
  m_forecastIconSlots.fill(nullptr);
  m_forecastGlyphs.fill(nullptr);
  m_forecastMetas.fill(nullptr);
  m_forecastDescs.fill(nullptr);
  m_forecastTemps.fill(nullptr);
  m_forecastHitAreas.fill(nullptr);
  if (m_config != nullptr) {
    if (const std::optional<std::string> catalog = m_config->stateString("weather_tab", "catalog")) {
      m_catalog = catalogFromState(*catalog);
      m_pinnedCatalog = m_catalog;
    }
  }
}

std::unique_ptr<Flex> WeatherTab::create() {
  const float scale = contentScale();
  const float controlHeight = Style::controlHeight * scale;

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto compactCard = ui::column({
      .out = &m_compactCard,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) { applyCompactCardStyle(card, scale, opacity, false); },
  });

  auto currentCard = ui::row({
      .out = &m_currentCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .minHeight = Style::controlHeightLg * 2.4F * scale,
      .clipChildren = true,
      .configure = [scale](Flex& card) {
        card.setDirection(FlexDirection::Horizontal);
        card.setGap(Style::spaceSm * scale);
      },
  });

  auto effectNode = std::make_unique<EffectNode>();
  effectNode->setParticipatesInLayout(false);
  effectNode->setZIndex(-1);
  effectNode->setVisible(false);
  effectNode->setRadius(Style::scaledRadius2xl(scale));
  m_effectNode = static_cast<EffectNode*>(currentCard->addChild(std::move(effectNode)));

  auto glyphColumn = ui::row(
      {.out = &m_glyphColumn,
       .align = FlexAlign::Center,
       .justify = FlexJustify::End,
       .fillHeight = true,
       .flexGrow = 0.9F},
      ui::glyph({
          .out = &m_currentGlyph,
          .glyph = "weather-cloud",
          .glyphSize = kCurrentGlyphSize * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
      })
  );
  currentCard->addChild(std::move(glyphColumn));

  auto currentText = ui::column(
      {.out = &m_currentText,
       .align = FlexAlign::Stretch,
       .justify = FlexJustify::Center,
       .gap = Style::spaceXs * scale,
       .fillWidth = true,
       .flexGrow = 1.0F}
  );

  currentText->addChild(
      ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale},
          ui::label({
              .out = &m_currentTempLabel,
              .text = "--°C",
              .fontSize = Style::fontSizeTitle * 2.35F * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
          }),
          ui::label({
              .out = &m_currentHiLoLabel,
              .text = "--↑ --↓",
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
              .maxLines = 1,
          })
      )
  );

  currentText->addChild(
      ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * 0.5F * scale},
          ui::label({
              .out = &m_currentDescLabel,
              .text = i18n::tr("control-center.weather.waiting"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
          }),
          ui::label({
              .out = &m_updatedLabel,
              .text = " ",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .visible = false,
              .participatesInLayout = false,
          }),
          ui::label({
              .out = &m_statusLabel,
              .text = " ",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .visible = false,
          })
      )
  );

  currentCard->addChild(std::move(currentText));

  auto locationPrompt = ui::row(
      {.out = &m_locationPrompt,
       .align = FlexAlign::Center,
       .justify = FlexJustify::Center,
       .gap = Style::spaceMd * scale,
       .fillWidth = true,
       .fillHeight = true,
       .flexGrow = 1.0F,
       .visible = false},
      ui::glyph({
          .out = &m_locationPromptGlyph,
          .glyph = "map-pin-off",
          .glyphSize = Style::controlHeightLg * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      }),
      ui::column(
          {.align = FlexAlign::Stretch, .justify = FlexJustify::Center, .gap = Style::spaceXs * scale},
          ui::label({
              .text = i18n::tr("control-center.weather.no-location-title"),
              .fontSize = Style::fontSizeBody * 1.1F * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
          }),
          ui::label({
              .out = &m_locationPromptBody,
              .text = i18n::tr("control-center.weather.no-location-body"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 2,
          })
      )
  );
  currentCard->addChild(std::move(locationPrompt));
  compactCard->addChild(std::move(currentCard));
  compactCard->addChild(
      ui::segmented({
          .out = &m_catalogSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = "",
                      .glyph = "calendar-week",
                      .tooltip = i18n::tr("control-center.weather.sections.daily"),
                  },
                  {
                      .label = "",
                      .glyph = "clock",
                      .tooltip = i18n::tr("control-center.weather.sections.hourly"),
                  },
                  {
                      .label = "",
                      .glyph = "list-details",
                      .tooltip = i18n::tr("control-center.weather.sections.details"),
                  },
              },
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
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
      })
  );

  auto enableRow = ui::column({
      .out = &m_enableRow,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .visible = false,
      .participatesInLayout = false,
  });
  auto enableToggleRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .minHeight = controlHeight});
  enableToggleRow->addChild(
      ui::label({
          .text = i18n::tr("control-center.weather.enable"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0F,
      })
  );
  enableToggleRow->addChild(
      ui::toggle({
          .out = &m_enableToggle,
          .checkedImmediate = m_weather != nullptr && m_weather->enabled(),
          .toggleSize = ToggleSize::Medium,
          .scale = scale,
          .onChange =
              [this](bool value) {
                if (m_config != nullptr) {
                  (void)m_config->setOverride({"weather", "enabled"}, value);
                }
              },
      })
  );
  enableRow->addChild(std::move(enableToggleRow));
  enableRow->addChild(
      ui::label({
          .text = i18n::tr("control-center.weather.disabled-hint"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
      })
  );
  compactCard->addChild(std::move(enableRow));
  tab->addChild(std::move(compactCard));

  auto detailsHost = ui::column({
      .out = &m_detailsHost,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
      .configure = [scale](Flex& host) { applyContentCardStyle(host, scale); },
  });
  auto detailsCard = ui::column({
      .out = &m_detailsCard,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
  });
  const float detailKeyWidth = Style::controlHeightLg * 2.0F * scale;
  // Detail rows are label/value pairs, not touch targets: they size to the text plus a small
  // margin around the row glyph, not to a control height.
  const float detailRowHeight = (Style::fontSizeBody + Style::spaceMd) * scale;

  auto addDetailRow = [&](std::string_view iconName, std::string_view key, Label*& valueOut) -> Flex* {
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = (Style::spaceSm + Style::spaceXs) * scale,
        .minHeight = detailRowHeight,
        .flexGrow = 0.0F,
    });
    Flex* rowPtr = row.get();
    row->addChild(
        ui::glyph({
            .glyph = std::string(iconName),
            .glyphSize = (Style::fontSizeBody + Style::spaceXs) * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    row->addChild(
        ui::label({
            .text = std::string(key),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .minWidth = detailKeyWidth - (Style::fontSizeBody + Style::spaceXs) * scale - Style::spaceSm * scale,
        })
    );
    row->addChild(
        ui::label({
            .out = &valueOut,
            .text = "--",
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .textAlign = TextAlign::End,
            .flexGrow = 1.0F,
        })
    );
    detailsCard->addChild(std::move(row));
    return rowPtr;
  };

  addDetailRow("thermometer", i18n::tr("control-center.weather.details.feels-like"), m_feelsLikeLabel);
  addDetailRow("temperature", i18n::tr("control-center.weather.details.temp-min"), m_tempMinLabel);
  addDetailRow("temperature-sun", i18n::tr("control-center.weather.details.temp-max"), m_tempMaxLabel);
  addDetailRow("wind", i18n::tr("control-center.weather.details.wind"), m_windLabel);
  addDetailRow("weather-sunrise", i18n::tr("control-center.weather.details.sunrise"), m_sunriseLabel);
  addDetailRow("weather-sunset", i18n::tr("control-center.weather.details.sunset"), m_sunsetLabel);
  addDetailRow("mountain", i18n::tr("control-center.weather.details.elevation"), m_elevationLabel);
  addDetailRow("sun", i18n::tr("control-center.weather.details.uv-index"), m_uvIndexLabel);
  m_timeZoneRow = addDetailRow("clock", i18n::tr("control-center.weather.details.timezone"), m_timeZoneLabel);
  detailsHost->addChild(std::move(detailsCard));
  tab->addChild(std::move(detailsHost));

  auto forecastHost = ui::column({
      .out = &m_forecastHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .flexGrow = 1.0F,
      .configure =
          [scale](Flex& host) {
            applyContentCardStyle(host, scale);
            host.setClipChildren(true);
          },
  });

  auto forecastRowsContainer = ui::column({
      .out = &m_forecastRowsContainer,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
      .flexGrow = 1.0F,
  });

  for (std::size_t i = 0; i < kForecastRowCount; ++i) {
    auto row = ui::column(
        {.out = &m_forecastRows[i],
         .align = FlexAlign::Stretch,
         .justify = FlexJustify::Center,
         .gap = Style::spaceXs * 0.5F * scale,
         .flexGrow = 1.0F,
         .configure = [scale](Flex& dayRow) { dayRow.setPadding(Style::spaceXs * scale, 0.0F); }}
    );

    auto daySlot = ui::row(
        {.out = &m_forecastIconSlots[i], .align = FlexAlign::Center, .gap = Style::spaceXs * scale, .flexGrow = 1.0F},
        ui::glyph({
            .out = &m_forecastGlyphs[i],
            .glyph = "weather-cloud",
            .glyphSize = Style::fontSizeBody * 1.2F * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        }),
        ui::label({
            .out = &m_forecastMetas[i],
            .text = i18n::tr("control-center.weather.forecast-placeholder.day"),
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
        })
    );
    auto topRow = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::SpaceBetween,
        .gap = Style::spaceSm * scale,
    });
    topRow->addChild(std::move(daySlot));

    topRow->addChild(
        ui::label({
            .out = &m_forecastTemps[i],
            .text = i18n::tr("control-center.weather.forecast-placeholder.temperature"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
            .textAlign = TextAlign::End,
        })
    );

    row->addChild(std::move(topRow));
    row->addChild(
        ui::label({
            .out = &m_forecastDescs[i],
            .text = i18n::tr("control-center.weather.forecast-placeholder.description"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
        })
    );

    auto hitArea = ui::inputArea({});
    hitArea->setAcceptedButtons(0);
    hitArea->setParticipatesInLayout(false);
    hitArea->setZIndex(2);
    m_forecastHitAreas[i] = static_cast<InputArea*>(row->addChild(std::move(hitArea)));

    forecastRowsContainer->addChild(std::move(row));

    if (i + 1 < kForecastRowCount) {
      forecastRowsContainer->addChild(
          ui::separator({
              .out = &m_forecastSeparators[i],
              .thickness = std::max(1.0F, scale),
          })
      );
    }
  }

  forecastHost->addChild(std::move(forecastRowsContainer));
  tab->addChild(std::move(forecastHost));

  syncCatalog();
  return m_settings->wrapLive(std::move(tab), scale, panelCardOpacity());
}

std::unique_ptr<Flex> WeatherTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void WeatherTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool WeatherTab::configureMode() const { return m_settings->configureMode(); }

SettingsPane* WeatherTab::settingsPane() { return m_settings.get(); }

void WeatherTab::setSettingsSearchQuery(std::string_view query) {
  if (m_settings != nullptr) {
    m_settings->setSearchQuery(std::string(query));
  }
}

WeatherTab::Catalog WeatherTab::catalogFromState(std::string_view value) noexcept {
  if (value == "hourly") {
    return Catalog::Hourly;
  }
  if (value == "details") {
    return Catalog::Details;
  }
  return Catalog::Daily;
}

std::string_view WeatherTab::catalogState(Catalog catalog) noexcept {
  switch (catalog) {
  case Catalog::Hourly:
    return "hourly";
  case Catalog::Details:
    return "details";
  case Catalog::Daily:
    break;
  }
  return "daily";
}

std::size_t WeatherTab::indexFromCatalog(Catalog catalog) noexcept {
  switch (catalog) {
  case Catalog::Hourly:
    return 1;
  case Catalog::Details:
    return 2;
  case Catalog::Daily:
    break;
  }
  return 0;
}

WeatherTab::Catalog WeatherTab::catalogFromIndex(std::size_t index) noexcept {
  switch (index) {
  case 1:
    return Catalog::Hourly;
  case 2:
    return Catalog::Details;
  default:
    return Catalog::Daily;
  }
}

bool WeatherTab::isForecastCatalog(Catalog catalog) noexcept {
  return catalog == Catalog::Daily || catalog == Catalog::Hourly;
}

bool WeatherTab::syncDetailLabel() {
  if (m_detailLabel == nullptr) {
    return false;
  }
  const char* key = "control-center.weather.sections.daily-caption";
  switch (m_catalog) {
  case Catalog::Hourly:
    key = "control-center.weather.sections.hourly-caption";
    break;
  case Catalog::Details:
    key = "control-center.weather.sections.details-caption";
    break;
  case Catalog::Daily:
    break;
  }
  m_detailLabel->setText(i18n::tr(key));
  return setShown(m_detailLabel, true);
}

void WeatherTab::persistCatalog() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("weather_tab", "catalog", std::string(catalogState(m_pinnedCatalog)));
}

void WeatherTab::applyCatalog(std::size_t index, bool persist) {
  if (m_catalogSyncing) {
    return;
  }
  const Catalog next = catalogFromIndex(index);
  if (persist) {
    m_pinnedCatalog = next;
    persistCatalog();
  }
  if (next == m_catalog) {
    syncCatalog();
    return;
  }
  const bool slide = persist && isForecastCatalog(m_catalog) && isForecastCatalog(next);
  if (slide) {
    beginForecastSlideOut(next);
    return;
  }
  cancelForecastSlide();
  m_catalog = next;
  syncCatalog();
  requestPanelLayout();
}

void WeatherTab::syncCatalog() {
  const bool weatherOn = m_weather != nullptr && m_weather->enabled();
  const bool locationReady = weatherOn && m_weather != nullptr && m_weather->locationConfigured();
  const bool details = m_catalog == Catalog::Details;
  const bool forecast = isForecastCatalog(m_catalog);
  bool layoutDirty = setShown(m_enableRow, !weatherOn) || setShown(m_currentCard, weatherOn)
      || setShown(m_catalogSwitch, locationReady) || setShown(m_forecastHost, locationReady && forecast)
      || setShown(m_detailsHost, locationReady && details);
  layoutDirty = syncDetailLabel() || layoutDirty;
  layoutDirty = setShown(m_detailLabel, locationReady) || layoutDirty;
  if (m_enableToggle != nullptr) {
    m_enableToggle->setChecked(weatherOn);
  }
  if (m_catalogSwitch != nullptr && m_catalogSwitch->selectedIndex() != indexFromCatalog(m_pinnedCatalog)) {
    m_catalogSyncing = true;
    m_catalogSwitch->setSelectedIndex(indexFromCatalog(m_pinnedCatalog));
    m_catalogSyncing = false;
    layoutDirty = true;
  }
  if (layoutDirty) {
    requestPanelLayout();
  }
}

void WeatherTab::onPanelCardOpacityChanged(float opacity) {
  if (m_catalogSwitch != nullptr) {
    m_catalogSwitch->setSurfaceOpacity(opacity);
  }
}

void WeatherTab::cancelForecastSlide() {
  const bool wasAnimating = m_forecastSlideAnimId != 0;
  if (wasAnimating && m_forecastHost != nullptr) {
    AnimationManager* animations = m_forecastHost->animationManager();
    if (animations != nullptr) {
      animations->cancel(m_forecastSlideAnimId);
    }
    m_forecastSlideAnimId = 0;
  }
  m_startForecastSlideIn = false;
  if (wasAnimating && m_forecastRowsContainer != nullptr) {
    m_forecastRowsContainer->setPosition(m_forecastRowsBaseX, m_forecastRowsBaseY);
    m_forecastRowsContainer->setOpacity(1.0F);
  }
}

void WeatherTab::applyForecastSlide(float progress, bool slidingIn) {
  if (m_forecastRowsContainer == nullptr || m_forecastHost == nullptr) {
    return;
  }

  const float travel = m_forecastHost->width();
  if (travel <= 0.0F) {
    return;
  }

  const auto direction = static_cast<float>(m_forecastSlideDirection);
  const float baseX = m_forecastRowsBaseX;
  const float baseY = m_forecastRowsBaseY;
  if (slidingIn) {
    m_forecastRowsContainer->setPosition(baseX + direction * travel * (1.0F - progress), baseY);
    m_forecastRowsContainer->setOpacity(0.7F + 0.3F * progress);
  } else {
    m_forecastRowsContainer->setPosition(baseX - direction * travel * progress, baseY);
    m_forecastRowsContainer->setOpacity(1.0F - 0.3F * progress);
  }
}

void WeatherTab::beginForecastSlideOut(Catalog nextView) {
  cancelForecastSlide();

  AnimationManager* animations = m_forecastHost != nullptr ? m_forecastHost->animationManager() : nullptr;
  if (animations == nullptr || m_forecastRowsContainer == nullptr) {
    m_catalog = nextView;
    syncCatalog();
    PanelManager::instance().refresh();
    return;
  }

  m_forecastRowsBaseX = m_forecastRowsContainer->x();
  m_forecastRowsBaseY = m_forecastRowsContainer->y();
  m_forecastSlideDirection = nextView == Catalog::Hourly ? 1 : -1;
  m_pendingCatalog = nextView;

  PanelManager::instance().requestFrameTick();
  m_forecastSlideAnimId = animations->animate(
      0.0F, 1.0F, static_cast<float>(Style::animFast), Easing::EaseOutCubic,
      [this](float progress) {
        applyForecastSlide(progress, false);
        PanelManager::instance().requestRedraw();
      },
      [this]() {
        m_forecastSlideAnimId = 0;
        m_catalog = m_pendingCatalog;
        m_startForecastSlideIn = true;
        syncCatalog();
        PanelManager::instance().refresh();
      },
      m_forecastHost
  );
}

void WeatherTab::beginForecastSlideIn() {
  AnimationManager* animations = m_forecastHost != nullptr ? m_forecastHost->animationManager() : nullptr;
  if (animations == nullptr || m_forecastRowsContainer == nullptr) {
    return;
  }

  applyForecastSlide(0.0F, true);
  PanelManager::instance().requestFrameTick();
  m_forecastSlideAnimId = animations->animate(
      0.0F, 1.0F, static_cast<float>(Style::animFast), Easing::EaseOutCubic,
      [this](float progress) {
        applyForecastSlide(progress, true);
        PanelManager::instance().requestRedraw();
      },
      [this]() {
        m_forecastSlideAnimId = 0;
        if (m_forecastRowsContainer != nullptr) {
          m_forecastRowsContainer->setPosition(m_forecastRowsBaseX, m_forecastRowsBaseY);
          m_forecastRowsContainer->setOpacity(1.0F);
        }
      },
      m_forecastHost
  );
}

void WeatherTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }

  const bool slidingOut = m_forecastSlideAnimId != 0 && !m_startForecastSlideIn;
  const float scale = contentScale();

  syncCatalog();

  if (slidingOut) {
    m_rootLayout->layout(renderer);
    return;
  }

  for (auto* label : m_forecastTemps) {
    if (label != nullptr) {
      label->setMaxWidth(0.0F);
      label->setMinWidth(0.0F);
    }
  }

  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);

  const float leftoverWidth = m_forecastHost != nullptr && m_forecastHost->visible()
      ? std::max(0.0F, m_forecastHost->width() - (m_forecastHost->paddingLeft() + m_forecastHost->paddingRight()))
      : (m_detailsHost != nullptr
             ? std::max(0.0F, m_detailsHost->width() - (m_detailsHost->paddingLeft() + m_detailsHost->paddingRight()))
             : contentWidth);

  if (m_statusLabel != nullptr) {
    m_statusLabel->setMaxWidth(contentWidth);
  }
  for (auto* label :
       {m_feelsLikeLabel, m_tempMinLabel, m_tempMaxLabel, m_windLabel, m_sunriseLabel, m_sunsetLabel, m_elevationLabel,
        m_uvIndexLabel, m_timeZoneLabel}) {
    if (label != nullptr) {
      label->setMaxWidth(leftoverWidth);
    }
  }

  if (m_currentGlyph != nullptr && m_currentCard != nullptr) {
    const float cardInnerHeight =
        std::max(0.0F, m_currentCard->height() - (m_currentCard->paddingTop() + m_currentCard->paddingBottom()));
    const float desiredGlyph =
        std::max(Style::controlHeightLg * 1.2F * scale, std::min(kCurrentGlyphSize * scale, cardInnerHeight * 0.85F));
    m_currentGlyph->setGlyphSize(desiredGlyph);
  }

  std::size_t visibleForecastDays = 0;
  for (std::size_t i = 0; i < kForecastRowCount; ++i) {
    if (m_forecastRows[i] != nullptr && m_forecastRows[i]->visible()) {
      ++visibleForecastDays;
    }
  }

  if (m_forecastHost != nullptr && m_forecastHost->visible() && visibleForecastDays > 0) {
    const float separatorThickness = std::max(1.0F, scale);
    std::size_t visibleSeparators = 0;
    for (auto* separator : m_forecastSeparators) {
      if (separator != nullptr) {
        separator->setThickness(separatorThickness);
        if (separator->visible()) {
          ++visibleSeparators;
        }
      }
    }
    const float forecastInnerHeight = std::max(
        0.0F, m_forecastHost->height() - (m_forecastHost->paddingTop() + m_forecastHost->paddingBottom())
    );
    const float separatorsTotal = separatorThickness * static_cast<float>(visibleSeparators);
    const float gapsTotal = m_forecastHost->gap() * static_cast<float>(visibleForecastDays + visibleSeparators);
    const float rowHeight = std::max(
        Style::controlHeightLg * scale,
        (forecastInnerHeight - separatorsTotal - gapsTotal) / static_cast<float>(visibleForecastDays)
    );

    for (std::size_t i = 0; i < kForecastRowCount; ++i) {
      if (m_forecastRows[i] == nullptr) {
        continue;
      }
      m_forecastRows[i]->setMinHeight(m_forecastRows[i]->visible() ? rowHeight : 0.0F);
    }
  }

  float forecastTempColumnWidth = 0.0F;
  for (std::size_t i = 0; i < kForecastRowCount; ++i) {
    if (m_forecastRows[i] != nullptr && m_forecastRows[i]->visible() && m_forecastTemps[i] != nullptr) {
      m_forecastTemps[i]->measure(renderer);
      forecastTempColumnWidth = std::max(forecastTempColumnWidth, m_forecastTemps[i]->width());
    }
  }

  const float forecastInnerWidth = leftoverWidth;
  for (std::size_t i = 0; i < kForecastRowCount; ++i) {
    if (m_forecastRows[i] == nullptr || !m_forecastRows[i]->visible()) {
      continue;
    }
    m_forecastRows[i]->setMinWidth(forecastInnerWidth);
    if (m_forecastTemps[i] != nullptr) {
      m_forecastTemps[i]->setMinWidth(forecastTempColumnWidth);
    }
    if (m_forecastMetas[i] != nullptr) {
      const float glyphWidth = m_forecastGlyphs[i] != nullptr ? m_forecastGlyphs[i]->width() : 0.0F;
      const float daySlotGap = m_forecastIconSlots[i] != nullptr ? m_forecastIconSlots[i]->gap() : 0.0F;
      const float topRowGap = Style::spaceSm * scale;
      const float metaMaxWidth = forecastInnerWidth - forecastTempColumnWidth - topRowGap - glyphWidth - daySlotGap;
      m_forecastMetas[i]->setMaxWidth(std::max(1.0F, metaMaxWidth));
    }
    if (m_forecastDescs[i] != nullptr) {
      m_forecastDescs[i]->setMaxWidth(std::max(1.0F, forecastInnerWidth));
    }
  }

  if (m_locationPrompt != nullptr && m_locationPrompt->visible() && m_locationPromptBody != nullptr) {
    const float cardPadding =
        m_currentCard != nullptr ? m_currentCard->paddingLeft() + m_currentCard->paddingRight() : 0.0F;
    const float glyphWidth = m_locationPromptGlyph != nullptr ? m_locationPromptGlyph->width() : 0.0F;
    const float promptGap = Style::spaceMd * scale;
    const float textWidth = std::max(1.0F, contentWidth - cardPadding - glyphWidth - promptGap);
    m_locationPromptBody->setMaxWidth(textWidth);
  }

  if (m_effectNode != nullptr && m_currentCard != nullptr) {
    m_effectNode->setPosition(0.0F, 0.0F);
    m_effectNode->setFrameSize(m_currentCard->width(), m_currentCard->height());
  }

  m_rootLayout->layout(renderer);

  if (m_effectNode != nullptr && m_currentCard != nullptr) {
    m_effectNode->setFrameSize(m_currentCard->width(), m_currentCard->height());
  }

  for (std::size_t i = 0; i < kForecastRowCount; ++i) {
    if (m_forecastRows[i] == nullptr || m_forecastHitAreas[i] == nullptr) {
      continue;
    }
    const bool visible = m_forecastRows[i]->visible();
    m_forecastHitAreas[i]->setVisible(visible);
    m_forecastHitAreas[i]->setPosition(0.0F, 0.0F);
    m_forecastHitAreas[i]->setSize(
        visible ? m_forecastRows[i]->width() : 0.0F, visible ? m_forecastRows[i]->height() : 0.0F
    );
  }

  if (m_startForecastSlideIn) {
    m_startForecastSlideIn = false;
    if (m_forecastRowsContainer != nullptr) {
      m_forecastRowsBaseX = m_forecastRowsContainer->x();
      m_forecastRowsBaseY = m_forecastRowsContainer->y();
    }
    beginForecastSlideIn();
  }
}

void WeatherTab::doUpdate(Renderer& renderer) { sync(renderer); }

void WeatherTab::setForecastVisibleRowCount(std::size_t count) {
  const std::size_t visibleCount = std::min(count, kForecastRowCount);
  bool layoutDirty = false;
  for (std::size_t i = 0; i < kForecastRowCount; ++i) {
    const bool show = i < visibleCount;
    layoutDirty = setShown(m_forecastRows[i], show) || layoutDirty;
    if (m_forecastHitAreas[i] != nullptr) {
      m_forecastHitAreas[i]->setVisible(show);
      if (!show) {
        m_forecastHitAreas[i]->clearTooltip();
      }
    }
    if (i + 1 < kForecastRowCount) {
      layoutDirty = setShown(m_forecastSeparators[i], i + 1 < visibleCount) || layoutDirty;
    }
  }
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
}

void WeatherTab::showLocationPrompt(bool show) {
  const bool layoutDirty = setShown(m_locationPrompt, show) || setShown(m_glyphColumn, !show) ||
                            setShown(m_currentText, !show);
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
}

void WeatherTab::onClose() {
  m_settings->onClose();
  cancelForecastSlide();
  m_rootLayout = nullptr;
  m_compactCard = nullptr;
  m_catalogSwitch = nullptr;
  m_detailLabel = nullptr;
  m_enableRow = nullptr;
  m_enableToggle = nullptr;
  m_currentCard = nullptr;
  m_glyphColumn = nullptr;
  m_detailsHost = nullptr;
  m_detailsCard = nullptr;
  m_currentText = nullptr;
  m_locationPrompt = nullptr;
  m_locationPromptGlyph = nullptr;
  m_locationPromptBody = nullptr;
  m_forecastHost = nullptr;
  m_forecastRowsContainer = nullptr;
  m_statusLabel = nullptr;
  m_currentGlyph = nullptr;
  m_currentTempLabel = nullptr;
  m_currentHiLoLabel = nullptr;
  m_currentDescLabel = nullptr;
  m_updatedLabel = nullptr;
  m_windLabel = nullptr;
  m_sunriseLabel = nullptr;
  m_sunsetLabel = nullptr;
  m_tempMaxLabel = nullptr;
  m_tempMinLabel = nullptr;
  m_feelsLikeLabel = nullptr;
  m_elevationLabel = nullptr;
  m_uvIndexLabel = nullptr;
  m_timeZoneLabel = nullptr;
  m_timeZoneRow = nullptr;
  m_forecastRows.fill(nullptr);
  m_forecastSeparators.fill(nullptr);
  m_forecastIconSlots.fill(nullptr);
  m_forecastGlyphs.fill(nullptr);
  m_forecastMetas.fill(nullptr);
  m_forecastDescs.fill(nullptr);
  m_forecastTemps.fill(nullptr);
  m_forecastHitAreas.fill(nullptr);
  m_effectNode = nullptr;
  m_activeEffect = EffectType::None;
  m_shaderTime = 0.0F;
}

void WeatherTab::sync(Renderer& renderer) {
  if (m_statusLabel == nullptr
      || m_currentGlyph == nullptr
      || m_currentTempLabel == nullptr
      || m_currentDescLabel == nullptr
      || m_updatedLabel == nullptr) {
    return;
  }

  const bool weatherOn = m_weather != nullptr && m_weather->enabled();
  syncCatalog();
  if (!weatherOn) {
    hideEffect();
    return;
  }

  showLocationPrompt(false);

  const bool showLocation = m_config == nullptr || m_config->config().shell.showLocation;
  const bool locationLayoutDirty = setShown(m_timeZoneRow, showLocation);
  if (locationLayoutDirty) {
    PanelManager::instance().requestLayout();
  }

  if (!m_weather->locationConfigured()) {
    showLocationPrompt(true);
    if (m_windLabel != nullptr) {
      m_windLabel->setText("--");
    }
    if (m_sunriseLabel != nullptr) {
      m_sunriseLabel->setText("--");
    }
    if (m_sunsetLabel != nullptr) {
      m_sunsetLabel->setText("--");
    }
    if (m_tempMaxLabel != nullptr) {
      m_tempMaxLabel->setText("--");
    }
    if (m_tempMinLabel != nullptr) {
      m_tempMinLabel->setText("--");
    }
    if (m_feelsLikeLabel != nullptr) {
      m_feelsLikeLabel->setText("--");
    }
    if (m_elevationLabel != nullptr) {
      m_elevationLabel->setText("--");
    }
    if (m_uvIndexLabel != nullptr) {
      m_uvIndexLabel->setText("--");
    }
    if (m_timeZoneLabel != nullptr) {
      m_timeZoneLabel->setText("--");
    }
    applyCompactMeta(m_updatedLabel, false, {}, false, {});
    setForecastVisibleRowCount(0);
    hideEffect();
    syncCatalog();
    return;
  }

  const auto& snapshot = m_weather->snapshot();
  if (!snapshot.valid) {
    m_currentGlyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    m_currentTempLabel->setText(std::format("--{}", m_weather->displayTemperatureUnit()));
    if (m_currentHiLoLabel != nullptr) {
      m_currentHiLoLabel->setText("-- / --");
    }
    m_currentDescLabel->setText(
        m_weather->loading() ? i18n::tr("control-center.weather.fetching")
                             : i18n::tr("control-center.weather.data-unavailable")
    );
    applyCompactMeta(m_updatedLabel, showLocation, snapshot.locationName, false, {});
    m_statusLabel->setText(m_weather->error());
    (void)setShown(m_statusLabel, !m_weather->error().empty());
    if (m_windLabel != nullptr) {
      m_windLabel->setText("--");
    }
    if (m_sunriseLabel != nullptr) {
      m_sunriseLabel->setText("--");
    }
    if (m_sunsetLabel != nullptr) {
      m_sunsetLabel->setText("--");
    }
    if (m_tempMaxLabel != nullptr) {
      m_tempMaxLabel->setText("--");
    }
    if (m_tempMinLabel != nullptr) {
      m_tempMinLabel->setText("--");
    }
    if (m_feelsLikeLabel != nullptr) {
      m_feelsLikeLabel->setText("--");
    }
    if (m_elevationLabel != nullptr) {
      m_elevationLabel->setText("--");
    }
    if (m_uvIndexLabel != nullptr) {
      m_uvIndexLabel->setText("--");
    }
    if (m_timeZoneLabel != nullptr) {
      m_timeZoneLabel->setText("--");
    }
    setForecastVisibleRowCount(0);
    hideEffect();
    return;
  }

  m_currentGlyph->setGlyph(WeatherService::glyphForCode(snapshot.current.weatherCode, snapshot.current.isDay));
  m_currentGlyph->setColor(colorSpecFromRole(snapshot.current.isDay ? ColorRole::Primary : ColorRole::Secondary));
  m_currentTempLabel->setText(
      std::format(
          "{}{}", static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.current.temperatureC))),
          m_weather->displayTemperatureUnit()
      )
  );
  if (m_currentHiLoLabel != nullptr) {
    if (!snapshot.forecastDays.empty()) {
      m_currentHiLoLabel->setText(
          std::format(
              "{} / {}{}",
              static_cast<int>(
                  std::lround(m_weather->displayTemperature(snapshot.forecastDays.front().temperatureMinC))
              ),
              static_cast<int>(
                  std::lround(m_weather->displayTemperature(snapshot.forecastDays.front().temperatureMaxC))
              ),
              m_weather->displayTemperatureUnit()
          )
      );
    } else {
      m_currentHiLoLabel->setText("-- / --");
    }
  }
  m_currentDescLabel->setText(WeatherService::descriptionForCode(snapshot.current.weatherCode));
  applyCompactMeta(m_updatedLabel, showLocation, snapshot.locationName, true, snapshot.fetchedAt);
  const std::string status = m_weather->loading() ? i18n::tr("control-center.weather.refreshing")
                                                  : (snapshot.valid ? std::string{} : m_weather->error());
  m_statusLabel->setText(status);
  m_statusLabel->setColor(
      colorSpecFromRole(m_weather->error().empty() ? ColorRole::OnSurfaceVariant : ColorRole::Error)
  );
  (void)setShown(m_statusLabel, !status.empty());
  if (m_windLabel != nullptr) {
    const bool imperial = m_weather->useImperial();
    const double windSpeed = imperial ? snapshot.current.windSpeedKmh * 0.621371 : snapshot.current.windSpeedKmh;
    const char* windUnit =
        imperial ? "mph" : (snapshot.currentUnits.windSpeed.empty() ? "km/h" : snapshot.currentUnits.windSpeed.c_str());
    m_windLabel->setText(
        std::format(
            "{} {} {}", static_cast<int>(std::lround(windSpeed)), windUnit,
            windDirectionLabel(snapshot.current.windDirectionDeg)
        )
    );
  }
  if (m_sunriseLabel != nullptr) {
    const auto& fmt = m_config->config().shell.timeFormat;
    m_sunriseLabel->setText(
        !snapshot.forecastDays.empty() ? formatIsoTime(snapshot.forecastDays.front().sunriseIso, fmt.c_str())
                                       : std::string("--")
    );
  }
  if (m_sunsetLabel != nullptr) {
    const auto& fmt = m_config->config().shell.timeFormat;
    m_sunsetLabel->setText(
        !snapshot.forecastDays.empty() ? formatIsoTime(snapshot.forecastDays.front().sunsetIso, fmt.c_str())
                                       : std::string("--")
    );
  }
  auto unit = m_weather->displayTemperatureUnit();
  if (m_feelsLikeLabel != nullptr) {
    const auto& apparent = snapshot.current.apparentTemperatureC;
    m_feelsLikeLabel->setText(
        apparent.has_value()
            ? std::format("{}{}", static_cast<int>(std::lround(m_weather->displayTemperature(*apparent))), unit)
            : std::string("--")
    );
  }
  if (m_tempMinLabel != nullptr) {
    if (!snapshot.forecastDays.empty()) {
      const int temp =
          static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.forecastDays.front().temperatureMinC)));
      m_tempMinLabel->setText(std::format("{}{}", temp, unit));
    } else {
      m_tempMinLabel->setText("--");
    }
  }
  if (m_tempMaxLabel != nullptr) {
    if (!snapshot.forecastDays.empty()) {
      const int temp =
          static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.forecastDays.front().temperatureMaxC)));
      m_tempMaxLabel->setText(std::format("{}{}", temp, unit));
    } else {
      m_tempMaxLabel->setText("--");
    }
  }
  if (m_elevationLabel != nullptr) {
    const bool imperial = m_weather->useImperial();
    const int elevation = static_cast<int>(imperial ? snapshot.elevationM * 3.28084 : snapshot.elevationM);
    m_elevationLabel->setText(std::format("{}{}", elevation, imperial ? "ft" : "m"));
  }
  if (m_uvIndexLabel != nullptr) {
    m_uvIndexLabel->setText(std::format("{:.1F}", snapshot.current.uvIndex));
  }
  if (m_timeZoneLabel != nullptr) {
    // Use the last component of the IANA path ("America/Toronto" → "Toronto") to keep
    // the label short enough to remain right-aligned without elision in most cases.
    std::string tzCity = snapshot.timezone;
    if (const auto slash = tzCity.rfind('/'); slash != std::string::npos) {
      tzCity = tzCity.substr(slash + 1);
    }
    m_timeZoneLabel->setText(
        snapshot.timezoneAbbreviation.empty() ? (snapshot.timezone.empty() ? std::string("--") : snapshot.timezone)
                                              : std::format("{} ({})", snapshot.timezoneAbbreviation, tzCity)
    );
  }

  syncCatalog();
  if (m_catalog == Catalog::Hourly) {
    syncHourlyForecast(renderer, snapshot);
  } else if (m_catalog == Catalog::Daily) {
    syncDailyForecast(renderer, snapshot);
  }

  if (m_effectNode != nullptr) {
    const EffectType newEffect = kTestEffect != EffectType::None
        ? kTestEffect
        : (m_weather->effectsEnabled() ? effectForWeatherCode(snapshot.current.weatherCode, snapshot.current.isDay)
                                       : EffectType::None);
    if (newEffect != m_activeEffect) {
      m_activeEffect = newEffect;
      m_shaderTime = 0.0F;
    }
    m_effectNode->setEffectType(m_activeEffect);
    m_effectNode->setBgColor(colorForRole(ColorRole::Surface));
    m_effectNode->setRadius(Style::scaledRadius2xl(contentScale()));
    m_effectNode->setVisible(m_activeEffect != EffectType::None);
  }
}

void WeatherTab::syncDailyForecast(Renderer& renderer, const WeatherSnapshot& snapshot) {
  const bool firstForecastIsToday =
      !snapshot.forecastDays.empty() && snapshot.forecastDays.front().dateIso == todayIso(snapshot.utcOffsetSeconds);
  const std::size_t forecastStart = firstForecastIsToday ? 1 : 0;
  const std::size_t visibleForecastCount = forecastStart < snapshot.forecastDays.size()
      ? std::min(kForecastRowCount, snapshot.forecastDays.size() - forecastStart)
      : 0;

  setForecastVisibleRowCount(visibleForecastCount);
  const std::string timeFormat = m_config != nullptr && !m_config->config().shell.timeFormat.empty()
      ? m_config->config().shell.timeFormat
      : std::string("%H:%M");
  for (std::size_t i = 0; i < kForecastRowCount; ++i) {
    if (i >= visibleForecastCount) {
      continue;
    }

    const auto& day = snapshot.forecastDays[i + forecastStart];
    const std::string condition = WeatherService::shortDescriptionForCode(day.weatherCode);
    const std::string tempHigh = std::format(
        "{}{}", static_cast<int>(std::lround(m_weather->displayTemperature(day.temperatureMaxC))),
        m_weather->displayTemperatureUnit()
    );
    const std::string tempLow = std::format(
        "{}{}", static_cast<int>(std::lround(m_weather->displayTemperature(day.temperatureMinC))),
        m_weather->displayTemperatureUnit()
    );
    const std::string sunrise =
        day.sunriseIso.empty() ? std::string("--") : formatIsoTime(day.sunriseIso, timeFormat.c_str());
    const std::string sunset =
        day.sunsetIso.empty() ? std::string("--") : formatIsoTime(day.sunsetIso, timeFormat.c_str());
    const std::vector<TooltipRow> tooltipRows{
        {i18n::tr("control-center.weather.daily.tooltip.condition"), condition},
        {i18n::tr("control-center.weather.daily.tooltip.low"), tempLow},
        {i18n::tr("control-center.weather.daily.tooltip.high"), tempHigh},
        {i18n::tr("control-center.weather.daily.tooltip.sunrise"), sunrise},
        {i18n::tr("control-center.weather.daily.tooltip.sunset"), sunset},
    };
    if (m_forecastHitAreas[i] != nullptr) {
      m_forecastHitAreas[i]->setTooltip(tooltipRows);
    }
    if (m_forecastGlyphs[i] != nullptr) {
      m_forecastGlyphs[i]->setGlyph(WeatherService::glyphForCode(day.weatherCode, true));
      m_forecastGlyphs[i]->setColor(colorSpecFromRole(ColorRole::Primary));
      m_forecastGlyphs[i]->measure(renderer);
    }
    if (m_forecastMetas[i] != nullptr) {
      m_forecastMetas[i]->setText(weekdayLabel(day.dateIso));
      m_forecastMetas[i]->clearTooltip();
      m_forecastMetas[i]->measure(renderer);
    }
    if (m_forecastTemps[i] != nullptr) {
      m_forecastTemps[i]->setText(std::format("{} / {}", tempLow, tempHigh));
      m_forecastTemps[i]->clearTooltip();
      m_forecastTemps[i]->measure(renderer);
    }
    if (m_forecastDescs[i] != nullptr) {
      m_forecastDescs[i]->setText(condition);
      m_forecastDescs[i]->clearTooltip();
      m_forecastDescs[i]->measure(renderer);
    }
  }
}

void WeatherTab::syncHourlyForecast(Renderer& renderer, const WeatherSnapshot& snapshot) {
  const std::size_t visibleForecastCount = std::min(kForecastRowCount, snapshot.forecastHours.size());
  const bool imperial = m_weather != nullptr && m_weather->useImperial();
  const char* windUnit =
      imperial ? "mph" : (snapshot.hourlyUnits.windSpeed.empty() ? "km/h" : snapshot.hourlyUnits.windSpeed.c_str());
  const std::string timeFormat = m_config != nullptr && !m_config->config().shell.timeFormat.empty()
      ? m_config->config().shell.timeFormat
      : std::string("%H:%M");

  setForecastVisibleRowCount(visibleForecastCount);
  for (std::size_t i = 0; i < kForecastRowCount; ++i) {
    if (i >= visibleForecastCount) {
      continue;
    }

    const auto& hour = snapshot.forecastHours[i];
    const int displayTemp = static_cast<int>(std::lround(m_weather->displayTemperature(hour.temperatureC)));
    const double displayWind = imperial ? hour.windSpeedKmh * 0.621371 : hour.windSpeedKmh;
    const std::string displayWindText = std::format("{} {}", static_cast<int>(std::lround(displayWind)), windUnit);
    const std::string condition = WeatherService::shortDescriptionForCode(hour.weatherCode);
    const std::vector<TooltipRow> tooltipRows{
        {i18n::tr("control-center.weather.hourly.tooltip.condition"), condition},
        {i18n::tr("control-center.weather.hourly.tooltip.rain"),
         std::format("{}%", hour.precipitationProbabilityPercent)},
        {i18n::tr("control-center.weather.hourly.tooltip.humidity"), std::format("{}%", hour.relativeHumidityPercent)},
        {i18n::tr("control-center.weather.hourly.tooltip.wind"), displayWindText},
    };
    if (m_forecastHitAreas[i] != nullptr) {
      m_forecastHitAreas[i]->setTooltip(tooltipRows);
    }
    if (m_forecastGlyphs[i] != nullptr) {
      m_forecastGlyphs[i]->setGlyph(WeatherService::glyphForCode(hour.weatherCode, hour.isDay));
      m_forecastGlyphs[i]->setColor(colorSpecFromRole(hour.isDay ? ColorRole::Primary : ColorRole::Secondary));
      m_forecastGlyphs[i]->measure(renderer);
    }
    if (m_forecastMetas[i] != nullptr) {
      m_forecastMetas[i]->setText(hourLabel(hour.timeIso, timeFormat));
      m_forecastMetas[i]->clearTooltip();
      m_forecastMetas[i]->measure(renderer);
    }
    if (m_forecastTemps[i] != nullptr) {
      m_forecastTemps[i]->setText(std::format("{}{}", displayTemp, m_weather->displayTemperatureUnit()));
      m_forecastTemps[i]->clearTooltip();
      m_forecastTemps[i]->measure(renderer);
    }
    if (m_forecastDescs[i] != nullptr) {
      m_forecastDescs[i]->setText(
          i18n::tr(
              "control-center.weather.hourly.summary", "condition", condition, "precip",
              hour.precipitationProbabilityPercent
          )
      );
      m_forecastDescs[i]->clearTooltip();
      m_forecastDescs[i]->measure(renderer);
    }
  }
}

std::string WeatherTab::todayIso(std::int32_t utcOffsetSeconds) {
  const auto now = std::chrono::system_clock::now() + std::chrono::seconds{utcOffsetSeconds};
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&time, &tm);

  return formatStrftime("%Y-%m-%d", tm);
}

std::string WeatherTab::hourLabel(const std::string& isoTime, const std::string& timeFormat) {
  if (isoTime.size() < 16) {
    return isoTime;
  }
  const std::string formatted = formatIsoTime(isoTime, timeFormat.empty() ? "%H:%M" : timeFormat.c_str());
  return formatted.empty() ? isoTime : formatted;
}

std::string WeatherTab::weekdayLabel(const std::string& isoDate) {
  if (isoDate.size() != 10) {
    return isoDate;
  }

  std::tm tm{};
  tm.tm_year = std::stoi(isoDate.substr(0, 4)) - 1900;
  tm.tm_mon = std::stoi(isoDate.substr(5, 2)) - 1;
  tm.tm_mday = std::stoi(isoDate.substr(8, 2));
  if (std::mktime(&tm) == -1) {
    return isoDate;
  }

  const std::string weekday = formatStrftime("%A", tm);
  if (weekday.empty()) {
    return isoDate;
  }
  return weekday;
}

void WeatherTab::hideEffect() {
  m_activeEffect = EffectType::None;
  m_shaderTime = 0.0F;
  if (m_effectNode != nullptr) {
    m_effectNode->setEffectType(EffectType::None);
    m_effectNode->setVisible(false);
  }
}

void WeatherTab::onFrameTick(float deltaMs) {
  if (m_effectNode == nullptr || !m_effectNode->visible() || m_activeEffect == EffectType::None) {
    return;
  }
  m_shaderTime += deltaMs * 0.001F;
  m_effectNode->setTime(m_shaderTime);
}

EffectType WeatherTab::effectForWeatherCode(std::int32_t code, bool isDay) {
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    return EffectType::Rain;
  }
  if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) {
    return EffectType::Snow;
  }
  if (code == 3) {
    return EffectType::Cloud;
  }
  if (code >= 40 && code <= 49) {
    return EffectType::Fog;
  }
  if (code == 0 && isDay) {
    return EffectType::Sun;
  }
  if (code == 0 && !isDay) {
    return EffectType::Stars;
  }
  return EffectType::None;
}
