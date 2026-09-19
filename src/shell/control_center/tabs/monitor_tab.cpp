#include "shell/control_center/tabs/monitor_tab.h"

#include "compositors/compositor_platform.h"
#include "compositors/niri/niri_config_outputs.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "system/brightness_service.h"
#include "system/display_config_service.h"
#include "system/gamma_service.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include "cursor-shape-v1-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace control_center;

namespace {

  constexpr float kBrightnessSyncEpsilon = 0.005F;
  constexpr float kScaleSyncEpsilon = 0.02F;
  constexpr float kLayoutMapMinHeight = 168.0F;
  constexpr float kLayoutMapMaxHeight = 240.0F;
  constexpr float kLayoutMinTileSide = 64.0F;
  constexpr float kLayoutMenuBarHeight = 10.0F;
  constexpr auto kBrightnessCommitInterval = std::chrono::milliseconds(16);
  constexpr auto kBrightnessStateHoldoff = std::chrono::milliseconds(180);
  constexpr double kMinScale = 0.5;
  constexpr double kMaxScale = 3.0;
  constexpr double kScaleStep = 0.05;

  bool setShown(Node* node, bool shown) {
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

  constexpr display_config::Transform kTransforms[] = {
      display_config::Transform::Normal,     display_config::Transform::Rotate90,
      display_config::Transform::Rotate180, display_config::Transform::Rotate270,
      display_config::Transform::Flipped,    display_config::Transform::Flipped90,
      display_config::Transform::Flipped180, display_config::Transform::Flipped270,
  };

  std::string buildDisplayListKey(
      const std::vector<BrightnessDisplay>& displays, const DisplayConfigService* displayConfig
  ) {
    std::string key;
    for (const auto& d : displays) {
      key += d.id;
      key += d.controllable ? ":1" : ":0";
      key += ';';
    }
    key += '|';
    if (displayConfig != nullptr) {
      for (const auto& output : displayConfig->outputs()) {
        key += output.name;
        key += output.enabled ? ":1" : ":0";
        key += std::format(
            ";{}x{}@{};{:.4f};{};{};", output.current.width, output.current.height, output.current.refreshMilliHz,
            output.scale, static_cast<int>(output.transform), output.vrrEnabled ? 1 : 0
        );
      }
    }
    return key;
  }

  std::string formatBrightnessDisplayInfo(const BrightnessDisplay& display) {
    const int physicalWidth = display.physicalWidth;
    const int physicalHeight = display.physicalHeight;
    const int width = physicalWidth > 0 ? physicalWidth : display.logicalWidth;
    const int height = physicalHeight > 0 ? physicalHeight : display.logicalHeight;
    std::string resolutionText = i18n::tr("control-center.display.unknown-resolution");
    if (width > 0 && height > 0) {
      resolutionText = std::to_string(width) + "x" + std::to_string(height);
    }

    const int logicalWidth = display.logicalWidth;
    const int logicalHeight = display.logicalHeight;
    if (physicalWidth <= 0 || physicalHeight <= 0 || logicalWidth <= 0 || logicalHeight <= 0) {
      return resolutionText;
    }

    const double scaleX = static_cast<double>(physicalWidth) / static_cast<double>(logicalWidth);
    const double scaleY = static_cast<double>(physicalHeight) / static_cast<double>(logicalHeight);
    const double scale = std::max(0.01, (scaleX + scaleY) * 0.5);
    const int scalePercent = static_cast<int>(std::lround(scale * 100.0));
    return resolutionText + " @ " + std::to_string(scalePercent) + "%";
  }

  std::string formatOutputInfo(const display_config::Output& output) {
    if (!output.enabled) {
      return i18n::tr("control-center.display.disabled");
    }
    if (output.current.width <= 0 || output.current.height <= 0) {
      return i18n::tr("control-center.display.unknown-resolution");
    }
    return display_config::formatResolution(output.current.width, output.current.height) + " @ "
        + display_config::formatRefresh(output.current.refreshMilliHz) + " · "
        + display_config::formatScale(output.scale);
  }

  std::string formatBrightnessValue(const BrightnessDisplay& display, float brightness) {
    if (!display.controllable) {
      return i18n::tr("control-center.display.disabled");
    }
    return std::to_string(static_cast<int>(std::round(brightness * 100.0F))) + "%";
  }

  std::string transformLabel(display_config::Transform transform) {
    switch (transform) {
    case display_config::Transform::Normal:
      return i18n::tr("control-center.display.transform-normal");
    case display_config::Transform::Rotate90:
      return i18n::tr("control-center.display.transform-90");
    case display_config::Transform::Rotate180:
      return i18n::tr("control-center.display.transform-180");
    case display_config::Transform::Rotate270:
      return i18n::tr("control-center.display.transform-270");
    case display_config::Transform::Flipped:
      return i18n::tr("control-center.display.transform-flipped");
    case display_config::Transform::Flipped90:
      return i18n::tr("control-center.display.transform-flipped-90");
    case display_config::Transform::Flipped180:
      return i18n::tr("control-center.display.transform-flipped-180");
    case display_config::Transform::Flipped270:
      return i18n::tr("control-center.display.transform-flipped-270");
    }
    return i18n::tr("control-center.display.transform-normal");
  }

  std::vector<std::string> transformLabels() {
    std::vector<std::string> labels;
    labels.reserve(std::size(kTransforms));
    for (const auto transform : kTransforms) {
      labels.push_back(transformLabel(transform));
    }
    return labels;
  }

  std::size_t transformIndex(display_config::Transform transform) {
    for (std::size_t index = 0; index < std::size(kTransforms); ++index) {
      if (kTransforms[index] == transform) {
        return index;
      }
    }
    return 0;
  }

  std::string defaultMovingName(const std::vector<display_config::Output>& outputs) {
    const display_config::Output* best = nullptr;
    int bestArea = std::numeric_limits<int>::max();
    for (const auto& output : outputs) {
      if (!output.enabled) {
        continue;
      }
      const auto size = display_config::logicalSize(output);
      const int area = size.width * size.height;
      if (best == nullptr || area < bestArea) {
        best = &output;
        bestArea = area;
      }
    }
    return best != nullptr ? best->name : std::string{};
  }

  const display_config::Output* referenceOutput(
      const std::vector<display_config::Output>& outputs, std::string_view movingName
  ) {
    const display_config::Output* best = nullptr;
    int bestArea = -1;
    for (const auto& output : outputs) {
      if (!output.enabled || output.name == movingName) {
        continue;
      }
      const auto size = display_config::logicalSize(output);
      const int area = size.width * size.height;
      if (best == nullptr || area > bestArea) {
        best = &output;
        bestArea = area;
      }
    }
    return best;
  }

  const display_config::Output* originOutput(const std::vector<display_config::Output>& outputs) {
    const display_config::Output* origin = nullptr;
    const display_config::Output* best = nullptr;
    for (const auto& output : outputs) {
      if (!output.enabled) {
        continue;
      }
      if (output.x == 0 && output.y == 0) {
        origin = &output;
        break;
      }
      if (best == nullptr || output.x < best->x || (output.x == best->x && output.y < best->y)) {
        best = &output;
      }
    }
    return origin != nullptr ? origin : best;
  }

  std::string layoutArrowTooltip(
      display_config::LayoutDirection direction, const display_config::Output* /*moving*/,
      const display_config::Output* /*reference*/
  ) {
    switch (direction) {
    case display_config::LayoutDirection::Top:
      return i18n::tr("control-center.display.place-top");
    case display_config::LayoutDirection::Bottom:
      return i18n::tr("control-center.display.place-bottom");
    case display_config::LayoutDirection::Left:
      return i18n::tr("control-center.display.place-left");
    case display_config::LayoutDirection::Right:
      return i18n::tr("control-center.display.place-right");
    }
    return {};
  }

  struct CardSource {
    std::string id;
    std::string label;
    const BrightnessDisplay* brightness = nullptr;
    const display_config::Output* output = nullptr;
  };

  std::vector<CardSource>
  collectCardSources(const BrightnessService* brightness, const DisplayConfigService* displayConfig) {
    std::vector<CardSource> sources;
    if (displayConfig != nullptr && displayConfig->available()) {
      for (const auto& output : displayConfig->outputs()) {
        CardSource source;
        source.id = output.name;
        source.label = display_config::displayLabel(output);
        source.output = &output;
        if (brightness != nullptr) {
          source.brightness = brightness->findDisplay(output.name);
        }
        if (source.brightness != nullptr && !source.brightness->label.empty()
            && (output.model.empty() || output.model == "Unknown")) {
          source.label = source.brightness->label;
        }
        sources.push_back(std::move(source));
      }
    }

    if (brightness == nullptr) {
      return sources;
    }

    for (const auto& display : brightness->displays()) {
      const bool already =
          std::ranges::any_of(sources, [&](const CardSource& source) { return source.id == display.id; });
      if (already) {
        continue;
      }
      sources.push_back({
          .id = display.id,
          .label = display.label,
          .brightness = &display,
          .output = nullptr,
      });
    }
    return sources;
  }

  void addLiveSlider(
      Flex& parent, float scale, const std::string& title, Slider** sliderOut, Label** valueOut, double minValue,
      double maxValue, double step, double value, bool enabled, std::function<std::string(double)> format,
      std::function<void(double)> onChange, std::function<void()> onCommit
  ) {
    auto header = makeControlRow(title, scale, nullptr, false);
    header->addChild(
        ui::label({
            .out = valueOut,
            .text = format(value),
            .fontSize = Style::fontSizeCaption * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    parent.addChild(std::move(header));
    parent.addChild(
        ui::slider({
            .out = sliderOut,
            .minValue = minValue,
            .maxValue = maxValue,
            .step = step,
            .value = value,
            .enabled = enabled,
            .trackHeight = Style::sliderTrackHeight * scale,
            .thumbSize = Style::sliderThumbSize * scale,
            .controlHeight = Style::controlHeight * scale,
            .flexGrow = 1.0F,
            .onValueChanged =
                [valueOut, format, callback = std::move(onChange)](double next) {
                  if (valueOut != nullptr && *valueOut != nullptr) {
                    (*valueOut)->setText(format(next));
                  }
                  if (callback) {
                    callback(next);
                  }
                },
            .onDragEnd = std::move(onCommit),
        })
    );
  }

  [[nodiscard]] std::string kelvinLabel(std::int32_t kelvin) { return std::format("{} K", kelvin); }

  [[nodiscard]] std::string nightLightStatusText(const GammaService* nightLight) {
    if (nightLight == nullptr) {
      return i18n::tr("osd.nightlight.off");
    }
    if (nightLight->forceEnabled()) {
      return i18n::tr("osd.nightlight.forced");
    }
    if (!nightLight->enabled()) {
      return i18n::tr("osd.nightlight.off");
    }
    return i18n::tr(nightLight->active() ? "osd.nightlight.running" : "osd.nightlight.scheduled");
  }

} // namespace

MonitorTab::MonitorTab(
    BrightnessService* brightness, ConfigService* config, DisplayConfigService* displayConfig,
    const ControlCenterServices& services
)
    : m_brightness(brightness), m_configService(config), m_displayConfig(displayConfig),
      m_platform(services.platform), m_nightLight(services.nightLight),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "services", .includedGroups = {"brightness"}}
      )) {}

std::unique_ptr<Flex> MonitorTab::create() {
  const float scale = contentScale();

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
  });

  auto cardsScroll = ui::scrollView({
      .out = &m_cardsScroll,
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  m_cardsLayout = cardsScroll->content();
  m_cardsLayout->setDirection(FlexDirection::Vertical);
  m_cardsLayout->setAlign(FlexAlign::Stretch);
  m_cardsLayout->setGap(Style::spaceMd * scale);

  m_cardsLayout->addChild(
      ui::label({
          .out = &m_noteLabel,
          .text = i18n::tr("control-center.display.temporary"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
      })
  );
  m_cardsLayout->addChild(
      ui::label({
          .out = &m_errorLabel,
          .text = "",
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Error),
          .visible = false,
      })
  );

  buildArrangementCard(*m_cardsLayout, scale);
  buildNightLightCard(*m_cardsLayout, scale);

  m_cardsLayout->addChild(ui::column(
      {
          .out = &m_emptyState,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceSm * scale,
          .visible = false,
          .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
      },
      ui::label({
          .text = i18n::tr("control-center.display.no-displays"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 2,
      }),
      ui::label({
          .text = i18n::tr("control-center.display.no-displays-hint"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
      })
  ));

  tab->addChild(std::move(cardsScroll));

  syncStatusLabels();
  return m_settings->wrapLive(std::move(tab), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> MonitorTab::createHeaderActions() {
  const float scale = contentScale();
  auto live = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .out = &m_saveButton,
          .glyph = "device-floppy",
          .tooltip = i18n::tr("control-center.display.save-to-niri"),
          .onClick =
              [this]() {
                if (m_displayConfig == nullptr) {
                  return;
                }
                const auto result = compositors::niri::saveOutputs(m_displayConfig->outputs());
                if (!result.ok) {
                  notify::error(
                      "Noctalia", i18n::tr("control-center.display.save-title"),
                      result.error.empty() ? i18n::tr("control-center.display.save-failed") : result.error
                  );
                  return;
                }
                notify::info(
                    "Noctalia", i18n::tr("control-center.display.save-title"),
                    i18n::tr("control-center.display.save-ok")
                );
                syncStatusLabels();
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      }),
      ui::button({
          .out = &m_rescanButton,
          .glyph = "refresh",
          .tooltip = i18n::tr("control-center.display.rescan"),
          .onClick =
              [this]() {
                if (m_brightness != nullptr) {
                  m_brightness->requestDdcRescan();
                }
                if (m_displayConfig != nullptr) {
                  m_displayConfig->refresh();
                }
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  syncHeaderActions();
  return m_settings->embedHeader(std::move(live), scale);
}

void MonitorTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool MonitorTab::configureMode() const { return m_settings->configureMode(); }

void MonitorTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

void MonitorTab::syncHeaderActions() {
  const bool niri = m_displayConfig != nullptr && m_displayConfig->available();
  const bool ddc = m_configService != nullptr && m_configService->config().brightness.enableDdcutil;
  const bool layoutDirty = setShown(m_rescanButton, ddc || niri) || setShown(m_saveButton, niri);
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
}

void MonitorTab::syncStatusLabels() {
  const bool niri = m_displayConfig != nullptr && m_displayConfig->available();
  if (m_noteLabel != nullptr) {
    if (niri && m_displayConfig != nullptr) {
      const bool saved = compositors::niri::savedLayoutMatches(m_displayConfig->outputs());
      m_noteLabel->setText(
          saved ? i18n::tr("control-center.display.saved") : i18n::tr("control-center.display.temporary")
      );
    }
  }
  if (m_errorLabel != nullptr) {
    const std::string error = m_displayConfig != nullptr ? m_displayConfig->lastError() : std::string{};
    m_errorLabel->setText(error);
  }
  const bool layoutDirty = setShown(m_noteLabel, niri) ||
                            setShown(m_errorLabel, m_errorLabel != nullptr && !m_errorLabel->text().empty());
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
}

void MonitorTab::setActive(bool active) {
  const bool becameActive = active && !m_active;
  m_active = active;
  if (becameActive && m_brightness != nullptr) {
    m_brightness->requestDdcRefresh();
  }
  if (becameActive && m_displayConfig != nullptr) {
    m_displayConfig->refresh();
  }
}

void MonitorTab::onClose() {
  m_settings->onClose();
  flushPendingBrightness(true);
  flushPendingScale(true);
  m_debounceTimer.stop();
  m_scaleDebounceTimer.stop();
  m_rootLayout = nullptr;
  m_noteLabel = nullptr;
  m_errorLabel = nullptr;
  m_emptyState = nullptr;
  m_cardsScroll = nullptr;
  m_cardsLayout = nullptr;
  m_layoutSection = nullptr;
  m_layoutMap = nullptr;
  m_layoutNudgeRow = nullptr;
  m_layoutReferenceRow = nullptr;
  m_layoutStatusLabel = nullptr;
  m_layoutReferenceSelect = nullptr;
  m_layoutPlaceTop = nullptr;
  m_layoutPlaceLeft = nullptr;
  m_layoutPlaceRight = nullptr;
  m_layoutPlaceBottom = nullptr;
  m_rescanButton = nullptr;
  m_saveButton = nullptr;
  m_nightLightCard = nullptr;
  m_nightLightDetails = nullptr;
  m_nightLightStatus = nullptr;
  m_nightLightEnabled = nullptr;
  m_nightLightForce = nullptr;
  m_nightLightDaySlider = nullptr;
  m_nightLightDayValue = nullptr;
  m_nightLightNightSlider = nullptr;
  m_nightLightNightValue = nullptr;
  m_cards.clear();
  m_layoutTiles.clear();
  m_layoutReferenceNames.clear();
  m_layoutPressName.clear();
  m_layoutDragging = false;
  m_lastDisplayListKey.clear();
  m_lastLayoutTileKey.clear();
}

bool MonitorTab::dragging() const noexcept {
  if (m_layoutDragging || m_layoutMenuBarPress) {
    return true;
  }
  for (const auto& card : m_cards) {
    if (card.slider != nullptr && card.slider->dragging()) {
      return true;
    }
    if (card.scaleSlider != nullptr && card.scaleSlider->dragging()) {
      return true;
    }
  }
  if (m_nightLightDaySlider != nullptr && m_nightLightDaySlider->dragging()) {
    return true;
  }
  if (m_nightLightNightSlider != nullptr && m_nightLightNightSlider->dragging()) {
    return true;
  }
  return false;
}

void MonitorTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }

  rebuildCards(renderer);
  rebuildLayoutSection(contentScale());

  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);

  const float scale = contentScale();
  const float cardWidth =
      std::max(1.0F, m_cardsScroll != nullptr ? m_cardsScroll->contentViewportWidth() : contentWidth);
  const float cardInnerWidth = std::max(1.0F, cardWidth - Style::spaceMd * scale * 2.0F);
  const float headerTextMaxWidth =
      std::max(1.0F, cardInnerWidth - Style::controlHeight * scale - Style::spaceSm * scale);
  for (auto& card : m_cards) {
    if (card.card != nullptr) {
      card.card->setMinWidth(cardWidth);
    }
    if (card.nameLabel != nullptr) {
      card.nameLabel->setMaxLines(1);
      card.nameLabel->setMaxWidth(headerTextMaxWidth);
    }
    if (card.detailsLabel != nullptr) {
      card.detailsLabel->setMaxLines(1);
      card.detailsLabel->setMaxWidth(cardInnerWidth);
    }
  }

  layoutArrangementMap(renderer);
  m_rootLayout->layout(renderer);
}

void MonitorTab::doUpdate(Renderer& renderer) {
  rebuildCards(renderer);
  syncHeaderActions();
  syncStatusLabels();
  syncNightLight();

  if (m_brightness == nullptr) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();

  for (auto& card : m_cards) {
    const auto* display = m_brightness->findDisplay(card.displayId);
    if (display == nullptr || card.slider == nullptr) {
      continue;
    }

    card.slider->setEnabled(display->controllable);

    if (card.scaleSlider == nullptr) {
      const std::string infoText = formatBrightnessDisplayInfo(*display);
      if (card.detailsLabel != nullptr && card.lastDisplayInfo != infoText) {
        card.detailsLabel->setText(infoText);
        card.lastDisplayInfo = infoText;
      }
    }

    if (!display->controllable) {
      if (card.valueLabel != nullptr) {
        card.valueLabel->setText(formatBrightnessValue(*display, display->brightness));
      }
      if (!card.slider->dragging() && std::abs(display->brightness - card.lastBrightness) >= kBrightnessSyncEpsilon) {
        m_syncingSlider = true;
        card.slider->setValue(display->brightness);
        m_syncingSlider = false;
        card.lastBrightness = display->brightness;
      }
      card.lastControllable = false;
      continue;
    }

    const bool isDragging = card.slider->dragging();
    const bool isPending = m_pendingDisplayId == card.displayId && m_pendingBrightness >= 0.0F;
    const bool holdState = isDragging
        && m_lastSentBrightness >= 0.0F
        && now < m_ignoreStateUntil
        && std::abs(display->brightness - m_lastSentBrightness) > 0.02F;

    const float minBrightness = m_configService->config().brightness.minimumBrightness;
    const float displayedBrightness = std::clamp(
        isPending ? m_pendingBrightness : (holdState ? m_lastSentBrightness : display->brightness), minBrightness, 1.0F
    );

    if (!isDragging
        && (!card.lastControllable || std::abs(displayedBrightness - card.lastBrightness) >= kBrightnessSyncEpsilon)) {
      m_syncingSlider = true;
      card.slider->setValue(displayedBrightness);
      m_syncingSlider = false;
      if (card.valueLabel != nullptr) {
        card.valueLabel->setText(formatBrightnessValue(*display, displayedBrightness));
      }
      card.lastBrightness = displayedBrightness;
    }
    card.lastControllable = true;
  }
}

void MonitorTab::buildArrangementCard(Flex& parent, float scale) {
  auto section = ui::column({
      .out = &m_layoutSection,
      .align = FlexAlign::Stretch,
      .fillWidth = true,
      .visible = false,
      .participatesInLayout = false,
      .configure =
          [scale](Flex& card) {
            applyContentCardStyle(card, scale);
            card.setGap(Style::spaceMd * scale);
          },
  });
  section->addChild(ui::column({
      .out = &m_layoutMap,
      .minHeight = kLayoutMapMinHeight * scale,
      .fillWidth = true,
      .clipChildren = true,
      .configure =
          [](Flex& host) {
            host.clearFill();
            host.clearBorder();
          },
  }));

  const float buttonSize = Style::controlHeightSm * scale;
  auto makeMoveButton = [this, scale, buttonSize](
                            const char* glyph, display_config::LayoutDirection direction, Button** out
                        ) {
    return ui::button({
        .out = out,
        .glyph = glyph,
        .width = buttonSize,
        .height = buttonSize,
        .onClick = [this, direction]() { applyLayout(direction); },
        .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
    });
  };
  auto nudge = ui::row({
      .out = &m_layoutNudgeRow,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .gap = Style::spaceSm * scale,
      .visible = false,
      .participatesInLayout = false,
  });
  nudge->addChild(makeMoveButton("chevron-up", display_config::LayoutDirection::Top, &m_layoutPlaceTop));
  nudge->addChild(makeMoveButton("chevron-left", display_config::LayoutDirection::Left, &m_layoutPlaceLeft));
  nudge->addChild(makeMoveButton("chevron-down", display_config::LayoutDirection::Bottom, &m_layoutPlaceBottom));
  nudge->addChild(makeMoveButton("chevron-right", display_config::LayoutDirection::Right, &m_layoutPlaceRight));
  section->addChild(std::move(nudge));

  auto referenceRow = ui::column({
      .out = &m_layoutReferenceRow,
      .align = FlexAlign::Stretch,
      .visible = false,
      .participatesInLayout = false,
  });
  m_layoutReferenceSelect = addSelectRow(
      *referenceRow, scale, i18n::tr("control-center.display.relative-to"),
      std::vector<std::string>{i18n::tr("control-center.display.relative-to")}, panelCardOpacity(),
      [this](std::size_t index) {
        if (index < m_layoutReferenceNames.size()) {
          m_layoutReferenceName = m_layoutReferenceNames[index];
          syncLayoutSelection();
        }
      }
  );
  section->addChild(std::move(referenceRow));
  parent.addChild(std::move(section));
}

void MonitorTab::rebuildLayoutSection(float scale) {
  (void)scale;
  if (m_layoutSection == nullptr || m_layoutMap == nullptr) {
    return;
  }
  if (m_displayConfig == nullptr || !m_displayConfig->available()) {
    setShown(m_layoutSection, false);
    return;
  }

  const auto& outputs = m_displayConfig->outputs();
  std::vector<std::string> names;
  std::string tileKey;
  for (const auto& output : outputs) {
    if (!output.enabled) {
      continue;
    }
    names.push_back(output.name);
    tileKey += output.name;
    tileKey += ';';
  }
  if (names.empty()) {
    setShown(m_layoutSection, false);
    m_lastLayoutTileKey.clear();
    return;
  }

  if (m_layoutMovingName.empty() || std::ranges::find(names, m_layoutMovingName) == names.end()) {
    m_layoutMovingName = defaultMovingName(outputs);
  }
  m_layoutReferenceName = layoutReferenceName();
  setShown(m_layoutNudgeRow, names.size() >= 2);
  setShown(m_layoutReferenceRow, names.size() > 2);
  setShown(m_layoutSection, true);

  if (tileKey != m_lastLayoutTileKey) {
    m_lastLayoutTileKey = std::move(tileKey);
    rebuildArrangementTiles();
  }
  syncLayoutSelection();
}

void MonitorTab::rebuildArrangementTiles() {
  m_layoutTiles.clear();
  m_layoutPressName.clear();
  m_layoutDragging = false;
  m_layoutMenuBarPress = false;
  if (m_layoutMap == nullptr || m_displayConfig == nullptr) {
    return;
  }
  while (!m_layoutMap->children().empty()) {
    m_layoutMap->removeChild(m_layoutMap->children().front().get());
  }

  const float scale = contentScale();
  const float menuBarHeight = kLayoutMenuBarHeight * scale;
  const auto& outputs = m_displayConfig->outputs();
  const auto* origin = originOutput(outputs);
  std::size_t index = 0;
  for (const auto& output : outputs) {
    if (!output.enabled) {
      continue;
    }
    ++index;
    LayoutTile tile;
    tile.name = output.name;
    const bool selected = output.name == m_layoutMovingName;
    const bool main = origin != nullptr && origin->name == output.name;
    auto host = ui::column({
        .out = &tile.host,
        .align = FlexAlign::Stretch,
        .clipChildren = true,
        .participatesInLayout = false,
    });
    host->addChild(
        ui::box({
            .out = &tile.menuBar,
            .fill = colorSpecFromRole(ColorRole::OnSurface, 0.88F),
            .height = menuBarHeight,
            .visible = main,
            .participatesInLayout = main,
        })
    );
    auto body = ui::column({
        .align = FlexAlign::Stretch,
        .paddingV = Style::spaceXs * scale,
        .paddingH = Style::spaceXs * scale,
        .flexGrow = 1.0F,
    });
    body->addChild(
        ui::label({
            .out = &tile.indexLabel,
            .text = std::to_string(index),
            .fontSize = Style::fontSizeTitle * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .textAlign = TextAlign::Center,
            .flexGrow = 1.0F,
        })
    );
    body->addChild(
        ui::label({
            .out = &tile.nameLabel,
            .text = display_config::displayLabel(output),
            .fontSize = Style::fontSizeMini * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
            .textAlign = TextAlign::Center,
            .ellipsize = TextEllipsize::End,
        })
    );
    host->addChild(std::move(body));
    host->addChild(
        ui::inputArea({
            .out = &tile.grab,
            .cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB,
            .tooltip = display_config::displayLabel(output) + " · " + i18n::tr("control-center.display.layout-hint-short"),
            .participatesInLayout = false,
            .onMotion =
                [this, name = output.name](const InputArea::PointerData& data) {
                  updateLayoutDrag(name, data.localX, data.localY);
                },
            .onPress =
                [this, name = output.name](const InputArea::PointerData& data) {
                  if (data.button != BTN_LEFT) {
                    return;
                  }
                  if (data.pressed) {
                    beginLayoutPress(name, data.localX, data.localY);
                  } else {
                    finishLayoutPress(name);
                  }
                },
            .onClick = [this, name = output.name](const InputArea::PointerData&) { selectLayoutOutput(name); },
        })
    );
    m_layoutMap->addChild(std::move(host));
    styleArrangementTile(tile, selected, main);
    m_layoutTiles.push_back(std::move(tile));
  }
  PanelManager::instance().requestLayout();
}

void MonitorTab::styleArrangementTile(LayoutTile& tile, bool selected, bool main) {
  if (tile.host == nullptr) {
    return;
  }
  const float scale = contentScale();
  tile.host->setFill(colorSpecFromRole(ColorRole::Surface, sectionCardFillOpacity(panelCardOpacity())));
  tile.host->setRadius(Style::scaledRadiusMd(scale));
  if (selected) {
    tile.host->setBorder(colorSpecFromRole(ColorRole::Primary), Style::focusRingWidth);
  } else {
    tile.host->setBorder(colorSpecFromRole(ColorRole::Outline, 0.55F), Style::borderWidth);
  }
  setShown(tile.menuBar, main);
}

void MonitorTab::showSelectedDisplayCard() {
  const bool niri = m_displayConfig != nullptr && m_displayConfig->available();
  if (!niri) {
    for (auto& card : m_cards) {
      setShown(card.card, true);
    }
    return;
  }
  bool anySelected = false;
  bool dirty = false;
  for (auto& card : m_cards) {
    const auto* output = m_displayConfig->findOutput(card.displayId);
    const bool selected = card.displayId == m_layoutMovingName;
    const bool keep = output == nullptr || !output->enabled || selected;
    anySelected = anySelected || selected;
    dirty = setShown(card.card, keep) || dirty;
  }
  if (!anySelected && !m_cards.empty()) {
    dirty = setShown(m_cards.front().card, true) || dirty;
  }
  if (dirty) {
    PanelManager::instance().requestLayout();
  }
}

void MonitorTab::layoutArrangementMap(Renderer& renderer) {
  if (m_layoutDragging || m_layoutMap == nullptr || m_displayConfig == nullptr) {
    return;
  }

  const float scale = contentScale();
  const float pad = Style::spaceSm * scale;
  const float mapWidth = m_layoutMap->width();
  if (mapWidth < 8.0F) {
    PanelManager::instance().requestLayout();
    return;
  }

  int minX = std::numeric_limits<int>::max();
  int minY = std::numeric_limits<int>::max();
  int maxX = std::numeric_limits<int>::min();
  int maxY = std::numeric_limits<int>::min();
  bool any = false;
  float smallestSide = std::numeric_limits<float>::max();
  for (const auto& output : m_displayConfig->outputs()) {
    if (!output.enabled) {
      continue;
    }
    const auto size = display_config::logicalSize(output);
    minX = std::min(minX, output.x);
    minY = std::min(minY, output.y);
    maxX = std::max(maxX, output.x + std::max(1, size.width));
    maxY = std::max(maxY, output.y + std::max(1, size.height));
    smallestSide = std::min(smallestSide, static_cast<float>(std::max(1, std::min(size.width, size.height))));
    any = true;
  }
  if (!any) {
    return;
  }

  const float boundsW = static_cast<float>(std::max(1, maxX - minX));
  const float boundsH = static_cast<float>(std::max(1, maxY - minY));
  float desiredHeight =
      std::clamp(mapWidth * (boundsH / boundsW), kLayoutMapMinHeight * scale, kLayoutMapMaxHeight * scale);
  const float innerW = std::max(1.0F, mapWidth - pad * 2.0F);
  float innerH = std::max(1.0F, std::max(desiredHeight, m_layoutMap->height()) - pad * 2.0F);
  float fitScale = std::min(innerW / boundsW, innerH / boundsH);
  const float minSide = kLayoutMinTileSide * scale;
  if (smallestSide > 0.0F && smallestSide * fitScale < minSide) {
    fitScale = minSide / smallestSide;
    desiredHeight = std::clamp(boundsH * fitScale + pad * 2.0F, kLayoutMapMinHeight * scale, kLayoutMapMaxHeight * scale);
    innerH = std::max(1.0F, desiredHeight - pad * 2.0F);
    fitScale = std::min(innerW / boundsW, innerH / boundsH);
  }
  if (std::abs(m_layoutMap->minHeight() - desiredHeight) > 0.5F) {
    m_layoutMap->setMinHeight(desiredHeight);
  }
  if (fitScale <= 0.0F) {
    return;
  }

  const float offsetX = pad + (innerW - boundsW * fitScale) * 0.5F;
  const float offsetY = pad + (innerH - boundsH * fitScale) * 0.5F;
  m_layoutMapScale = fitScale;
  m_layoutMapOffsetX = offsetX;
  m_layoutMapOffsetY = offsetY;
  m_layoutOriginX = minX;
  m_layoutOriginY = minY;

  const auto* origin = originOutput(m_displayConfig->outputs());
  for (auto& tile : m_layoutTiles) {
    const auto* output = m_displayConfig->findOutput(tile.name);
    if (output == nullptr || tile.host == nullptr) {
      continue;
    }
    const auto size = display_config::logicalSize(*output);
    const float tileW = std::max(1.0F, static_cast<float>(std::max(1, size.width)) * fitScale);
    const float tileH = std::max(1.0F, static_cast<float>(std::max(1, size.height)) * fitScale);
    const float tileX = offsetX + static_cast<float>(output->x - minX) * fitScale;
    const float tileY = offsetY + static_cast<float>(output->y - minY) * fitScale;
    tile.x = tileX;
    tile.y = tileY;
    const bool selected = tile.name == m_layoutMovingName;
    const bool main = origin != nullptr && origin->name == tile.name;
    styleArrangementTile(tile, selected, main);
    tile.host->setSize(tileW, tileH);
    tile.host->setPosition(tileX, tileY);
    tile.host->setZIndex(selected ? 1 : 0);
    if (tile.nameLabel != nullptr) {
      tile.nameLabel->setMaxWidth(std::max(1.0F, tileW - Style::spaceSm * scale * 2.0F));
    }
    tile.host->layout(renderer);
    if (tile.grab != nullptr) {
      tile.grab->setPosition(0.0F, 0.0F);
      tile.grab->setFrameSize(tileW, tileH);
      tile.grab->setZIndex(2);
    }
  }
}

std::string MonitorTab::layoutReferenceName() const {
  if (m_displayConfig == nullptr) {
    return {};
  }
  if (!m_layoutReferenceName.empty() && m_layoutReferenceName != m_layoutMovingName) {
    const auto* output = m_displayConfig->findOutput(m_layoutReferenceName);
    if (output != nullptr && output->enabled) {
      return m_layoutReferenceName;
    }
  }
  const auto* reference = referenceOutput(m_displayConfig->outputs(), m_layoutMovingName);
  return reference != nullptr ? reference->name : std::string{};
}

void MonitorTab::selectLayoutOutput(std::string name) {
  if (name.empty()) {
    return;
  }
  if (name != m_layoutMovingName) {
    m_layoutMovingName = std::move(name);
    if (m_layoutReferenceName == m_layoutMovingName) {
      m_layoutReferenceName.clear();
    }
    m_layoutReferenceName = layoutReferenceName();
  }
  syncLayoutSelection();
  showSelectedDisplayCard();
}

void MonitorTab::syncLayoutSelection() {
  const std::string reference = layoutReferenceName();
  m_layoutReferenceName = reference;
  const auto* origin = m_displayConfig != nullptr ? originOutput(m_displayConfig->outputs()) : nullptr;
  for (auto& tile : m_layoutTiles) {
    if (tile.host == nullptr) {
      continue;
    }
    const bool selected = tile.name == m_layoutMovingName;
    const bool main = origin != nullptr && origin->name == tile.name;
    styleArrangementTile(tile, selected, main);
    tile.host->setZIndex(selected ? 1 : 0);
  }

  if (m_displayConfig != nullptr) {
    const auto* moving = m_displayConfig->findOutput(m_layoutMovingName);
    const auto* referenceOutput = m_displayConfig->findOutput(reference);
    auto setArrow = [&](Button* button, display_config::LayoutDirection direction) {
      if (button != nullptr) {
        button->setTooltip(layoutArrowTooltip(direction, moving, referenceOutput));
      }
    };
    setArrow(m_layoutPlaceTop, display_config::LayoutDirection::Top);
    setArrow(m_layoutPlaceBottom, display_config::LayoutDirection::Bottom);
    setArrow(m_layoutPlaceLeft, display_config::LayoutDirection::Left);
    setArrow(m_layoutPlaceRight, display_config::LayoutDirection::Right);
  }

  if (m_layoutReferenceSelect == nullptr || m_displayConfig == nullptr) {
    return;
  }

  m_layoutReferenceNames.clear();
  std::vector<std::string> labels;
  std::size_t selected = 0;
  for (const auto& output : m_displayConfig->outputs()) {
    if (!output.enabled || output.name == m_layoutMovingName) {
      continue;
    }
    if (output.name == reference) {
      selected = labels.size();
    }
    m_layoutReferenceNames.push_back(output.name);
    labels.push_back(display_config::displayLabel(output) + " · " + output.name);
  }
  m_layoutReferenceSelect->setOptions(std::move(labels));
  if (!m_layoutReferenceNames.empty()) {
    m_layoutReferenceSelect->setSelectedIndexSilently(selected);
  }
}

void MonitorTab::beginLayoutPress(const std::string& name, float localX, float localY) {
  selectLayoutOutput(name);
  m_layoutPressName = name;
  m_layoutGrabX = localX;
  m_layoutGrabY = localY;
  m_layoutDragging = false;
  m_layoutMenuBarPress = false;
  for (const auto& tile : m_layoutTiles) {
    if (tile.name != name || tile.menuBar == nullptr || !tile.menuBar->visible()) {
      continue;
    }
    if (localY <= tile.menuBar->height()) {
      m_layoutMenuBarPress = true;
    }
    break;
  }
}

void MonitorTab::updateLayoutDrag(const std::string& name, float localX, float localY) {
  if (m_layoutPressName != name) {
    return;
  }
  if (m_layoutMenuBarPress) {
    m_layoutGrabX = localX;
    m_layoutGrabY = localY;
    return;
  }
  LayoutTile* tile = nullptr;
  for (auto& entry : m_layoutTiles) {
    if (entry.name == name) {
      tile = &entry;
      break;
    }
  }
  if (tile == nullptr || tile->host == nullptr) {
    return;
  }

  const float nextX = tile->host->x() + localX - m_layoutGrabX;
  const float nextY = tile->host->y() + localY - m_layoutGrabY;
  const float dx = nextX - tile->x;
  const float dy = nextY - tile->y;
  if (!m_layoutDragging) {
    if ((dx * dx) + (dy * dy) < Style::dragStartThreshold * Style::dragStartThreshold) {
      return;
    }
    m_layoutDragging = true;
    tile->host->setZIndex(2);
    if (tile->grab != nullptr) {
      tile->grab->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING);
    }
  }
  tile->host->setPosition(nextX, nextY);
}

void MonitorTab::finishLayoutPress(const std::string& name) {
  if (m_layoutPressName != name) {
    return;
  }
  if (m_layoutMenuBarPress) {
    applyMenuBarDrop();
  } else if (m_layoutDragging) {
    applyLayoutDrag();
  }
  m_layoutDragging = false;
  m_layoutMenuBarPress = false;
  m_layoutPressName.clear();
  for (auto& tile : m_layoutTiles) {
    if (tile.host == nullptr) {
      continue;
    }
    tile.host->setZIndex(tile.name == m_layoutMovingName ? 1 : 0);
    if (tile.grab != nullptr) {
      tile.grab->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB);
    }
  }
}

void MonitorTab::applyLayoutDrag() {
  if (m_displayConfig == nullptr || m_layoutMapScale <= 0.0F) {
    return;
  }
  LayoutTile* tile = nullptr;
  for (auto& entry : m_layoutTiles) {
    if (entry.name == m_layoutMovingName) {
      tile = &entry;
      break;
    }
  }
  if (tile == nullptr || tile->host == nullptr) {
    return;
  }
  const auto* output = m_displayConfig->findOutput(tile->name);
  if (output == nullptr) {
    return;
  }

  const int proposedX =
      m_layoutOriginX + static_cast<int>(std::lround((tile->host->x() - m_layoutMapOffsetX) / m_layoutMapScale));
  const int proposedY =
      m_layoutOriginY + static_cast<int>(std::lround((tile->host->y() - m_layoutMapOffsetY) / m_layoutMapScale));
  const auto snap = display_config::snapDragPosition(*output, m_displayConfig->outputs(), proposedX, proposedY);
  if (!snap.has_value()) {
    return;
  }
  m_layoutReferenceName = snap->referenceName;
  if (snap->position.x == output->x && snap->position.y == output->y) {
    return;
  }
  if (!m_displayConfig->setPosition(tile->name, snap->position.x, snap->position.y)) {
    m_lastDisplayListKey.clear();
    reportLayoutError();
    return;
  }
  syncLayoutSelection();
}

void MonitorTab::applyMenuBarDrop() {
  if (m_displayConfig == nullptr) {
    return;
  }
  LayoutTile* from = nullptr;
  for (auto& entry : m_layoutTiles) {
    if (entry.name == m_layoutPressName) {
      from = &entry;
      break;
    }
  }
  if (from == nullptr || from->host == nullptr) {
    return;
  }
  const float mapX = from->host->x() + m_layoutGrabX;
  const float mapY = from->host->y() + m_layoutGrabY;
  for (const auto& tile : m_layoutTiles) {
    if (tile.host == nullptr) {
      continue;
    }
    if (mapX < tile.host->x() || mapX >= tile.host->x() + tile.host->width() || mapY < tile.host->y()
        || mapY >= tile.host->y() + tile.host->height()) {
      continue;
    }
    if (tile.name == from->name) {
      return;
    }
    if (!m_displayConfig->setOrigin(tile.name)) {
      m_lastDisplayListKey.clear();
      reportLayoutError();
    }
    return;
  }
}

void MonitorTab::buildNightLightCard(Flex& parent, float scale) {
  auto card = ui::column({
      .out = &m_nightLightCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .visible = false,
      .configure =
          [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  auto enableRow = makeControlRow(i18n::tr("settings.schema.services.night-light.label"), scale, nullptr, false);
  enableRow->addChild(
      ui::toggle({
          .out = &m_nightLightEnabled,
          .checkedImmediate = false,
          .toggleSize = ToggleSize::Medium,
          .scale = scale,
          .onChange =
              [this](bool checked) {
                if (!m_syncingNightLight) {
                  persistNightLightEnabled(checked);
                }
              },
      })
  );
  card->addChild(std::move(enableRow));
  m_nightLightStatus = addCaption(*card, nightLightStatusText(m_nightLight), scale);

  const bool detailsOn = m_nightLight != nullptr && (m_nightLight->enabled() || m_nightLight->forceEnabled());
  auto details = ui::column({
      .out = &m_nightLightDetails,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .visible = detailsOn,
      .participatesInLayout = detailsOn,
  });
  addWellDivider(*details, scale);
  m_nightLightForce = addToggleRow(
      *details, scale, i18n::tr("settings.schema.services.force-night-light.label"),
      [this](bool checked) {
        if (!m_syncingNightLight) {
          persistNightLightForce(checked);
        }
      }
  );
  const auto kelvinFormat = [](double value) {
    return kelvinLabel(static_cast<std::int32_t>(std::lround(value)));
  };
  addSliderSection(
      *details, scale, i18n::tr("settings.schema.services.day-temperature.label"), &m_nightLightDaySlider,
      &m_nightLightDayValue, static_cast<double>(NightLightConfig::kTemperatureMin),
      static_cast<double>(NightLightConfig::kTemperatureMax), static_cast<double>(NightLightConfig::kTemperatureGap),
      static_cast<double>(NightLightConfig::kTemperatureMax), kelvinFormat, [this]() { commitNightLightTemperatures(true); }
  );
  addSliderSection(
      *details, scale, i18n::tr("settings.schema.services.night-temperature.label"), &m_nightLightNightSlider,
      &m_nightLightNightValue, static_cast<double>(NightLightConfig::kTemperatureMin),
      static_cast<double>(NightLightConfig::kTemperatureMax), static_cast<double>(NightLightConfig::kTemperatureGap),
      static_cast<double>(NightLightConfig::kTemperatureMin), kelvinFormat,
      [this]() { commitNightLightTemperatures(false); }
  );
  card->addChild(std::move(details));

  parent.addChild(std::move(card));
  syncNightLight();
}

void MonitorTab::syncNightLight() {
  const bool available = m_platform != nullptr && m_platform->hasGammaControl();
  bool layoutDirty = setShown(m_nightLightCard, available);
  if (!available) {
    if (layoutDirty) {
      PanelManager::instance().requestLayout();
    }
    return;
  }

  const bool enabled = m_nightLight != nullptr && m_nightLight->enabled();
  const bool forced = m_nightLight != nullptr && m_nightLight->forceEnabled();
  const bool controlsOn = enabled || forced;
  const NightLightConfig nightlight =
      m_configService != nullptr ? m_configService->config().nightlight : NightLightConfig{};

  m_syncingNightLight = true;
  if (m_nightLightStatus != nullptr) {
    m_nightLightStatus->setText(nightLightStatusText(m_nightLight));
  }
  if (m_nightLightEnabled != nullptr) {
    m_nightLightEnabled->setCheckedImmediate(enabled);
  }
  if (m_nightLightForce != nullptr) {
    m_nightLightForce->setCheckedImmediate(forced);
    m_nightLightForce->setEnabled(controlsOn || enabled);
  }

  const auto day = static_cast<double>(nightlight.dayTemperature);
  const auto night = static_cast<double>(nightlight.nightTemperature);
  if (m_nightLightDaySlider != nullptr && !m_nightLightDaySlider->dragging()) {
    m_nightLightDaySlider->setEnabled(controlsOn);
    m_nightLightDaySlider->setValue(day);
  }
  if (m_nightLightNightSlider != nullptr && !m_nightLightNightSlider->dragging()) {
    m_nightLightNightSlider->setEnabled(controlsOn);
    m_nightLightNightSlider->setValue(night);
  }
  if (m_nightLightDayValue != nullptr) {
    m_nightLightDayValue->setText(kelvinLabel(nightlight.dayTemperature));
  }
  if (m_nightLightNightValue != nullptr) {
    m_nightLightNightValue->setText(kelvinLabel(nightlight.nightTemperature));
  }
  m_syncingNightLight = false;
  layoutDirty = setShown(m_nightLightDetails, controlsOn) || layoutDirty;
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
}

void MonitorTab::persistNightLightEnabled(bool enabled) {
  if (m_nightLight != nullptr) {
    m_nightLight->setEnabled(enabled);
  }
  if (m_configService != nullptr) {
    (void)m_configService->setOverride({"nightlight", "enabled"}, enabled);
  }
  syncNightLight();
}

void MonitorTab::persistNightLightForce(bool enabled) {
  if (m_nightLight != nullptr) {
    m_nightLight->setForceEnabled(enabled);
  }
  if (m_configService != nullptr) {
    (void)m_configService->setOverride({"nightlight", "force"}, enabled);
  }
  syncNightLight();
}

void MonitorTab::commitNightLightTemperatures(bool fromDaySlider) {
  if (m_syncingNightLight || m_configService == nullptr || m_nightLightDaySlider == nullptr
      || m_nightLightNightSlider == nullptr) {
    return;
  }

  std::int32_t day = std::clamp(
      static_cast<std::int32_t>(std::lround(m_nightLightDaySlider->value())), NightLightConfig::kTemperatureMin,
      NightLightConfig::kTemperatureMax
  );
  std::int32_t night = std::clamp(
      static_cast<std::int32_t>(std::lround(m_nightLightNightSlider->value())), NightLightConfig::kTemperatureMin,
      NightLightConfig::kTemperatureMax
  );
  if (day - NightLightConfig::kTemperatureGap < night) {
    if (fromDaySlider) {
      night = std::max(NightLightConfig::kTemperatureMin, day - NightLightConfig::kTemperatureGap);
      if (night + NightLightConfig::kTemperatureGap > day) {
        day = std::min(NightLightConfig::kTemperatureMax, night + NightLightConfig::kTemperatureGap);
      }
    } else {
      day = std::min(NightLightConfig::kTemperatureMax, night + NightLightConfig::kTemperatureGap);
      if (day - NightLightConfig::kTemperatureGap < night) {
        night = std::max(NightLightConfig::kTemperatureMin, day - NightLightConfig::kTemperatureGap);
      }
    }
  }

  (void)m_configService->setOverrides({
      {{"nightlight", "temperature_day"}, static_cast<std::int64_t>(day)},
      {{"nightlight", "temperature_night"}, static_cast<std::int64_t>(night)},
  });
}

void MonitorTab::rebuildCards(Renderer& /*renderer*/) {
  if (m_cardsLayout == nullptr) {
    return;
  }

  const std::vector<BrightnessDisplay> noDisplays;
  const auto& displays = m_brightness != nullptr ? m_brightness->displays() : noDisplays;
  std::string key = buildDisplayListKey(displays, m_displayConfig);
  if (m_displayConfig != nullptr) {
    if (const auto* origin = originOutput(m_displayConfig->outputs()); origin != nullptr) {
      key += origin->name;
    }
  }
  if (key == m_lastDisplayListKey) {
    return;
  }
  m_lastDisplayListKey = key;

  for (auto& card : m_cards) {
    if (card.card != nullptr) {
      m_cardsLayout->removeChild(card.card);
    }
  }
  m_cards.clear();

  const auto sources = collectCardSources(m_brightness, m_displayConfig);
  const bool empty = sources.empty();
  if (setShown(m_emptyState, empty)) {
    PanelManager::instance().requestLayout();
  }

  const float scale = contentScale();
  rebuildLayoutSection(scale);
  if (empty) {
    return;
  }

  const bool niriAvailable = m_displayConfig != nullptr && m_displayConfig->available();
  const std::size_t enabledCount = m_displayConfig != nullptr ? m_displayConfig->enabledCount() : 0;

  for (const auto& source : sources) {
    auto card = ui::column({
        .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
    });

    Glyph* iconPtr = nullptr;
    Label* nameLabelPtr = nullptr;
    Toggle* enabledToggle = nullptr;
    auto headerRow = makeControlRow(source.label, scale, &nameLabelPtr, false);
    if (nameLabelPtr != nullptr) {
      nameLabelPtr->setFontWeight(FontWeight::Bold);
    }
    if (source.output != nullptr) {
      const std::string displayId = source.id;
      const bool canDisable = source.output->enabled && enabledCount > 1;
      headerRow->addChild(
          ui::toggle({
              .out = &enabledToggle,
              .checkedImmediate = source.output->enabled,
              .enabled = source.output->enabled ? canDisable : true,
              .toggleSize = ToggleSize::Medium,
              .scale = scale,
              .onChange =
                  [this, displayId](bool checked) {
                    if (m_syncingConfig || m_displayConfig == nullptr) {
                      return;
                    }
                    if (!m_displayConfig->setEnabled(displayId, checked)) {
                      m_lastDisplayListKey.clear();
                    }
                  },
          })
      );
    }
    card->addChild(std::move(headerRow));

    const std::string infoText =
        source.output != nullptr ? formatOutputInfo(*source.output)
                                 : (source.brightness != nullptr ? formatBrightnessDisplayInfo(*source.brightness)
                                                              : i18n::tr("control-center.display.unknown-resolution"));
    Label* detailsLabelPtr = nullptr;
    detailsLabelPtr = addCaption(*card, infoText, scale);

    Slider* sliderPtr = nullptr;
    Label* valueLabelPtr = nullptr;
    float lastBrightness = 0.0F;
    bool lastControllable = false;
    if (source.brightness != nullptr) {
      const auto& display = *source.brightness;
      lastBrightness = display.brightness;
      lastControllable = display.controllable;
      const std::string displayId = display.id;
      const float minBrightness =
          m_configService != nullptr ? m_configService->config().brightness.minimumBrightness : 0.0F;
      addLiveSlider(
          *card, scale, i18n::tr("control-center.display.brightness"), &sliderPtr, &valueLabelPtr, minBrightness, 1.0,
          0.01, std::max(static_cast<double>(display.brightness), static_cast<double>(minBrightness)),
          display.controllable,
          [this, displayId](double value) {
            const auto* currentDisplay = m_brightness != nullptr ? m_brightness->findDisplay(displayId) : nullptr;
            if (currentDisplay == nullptr) {
              return std::to_string(static_cast<int>(std::lround(value * 100.0))) + "%";
            }
            return formatBrightnessValue(*currentDisplay, static_cast<float>(value));
          },
          [this, displayId](double value) {
            if (m_syncingSlider) {
              return;
            }
            const auto* currentDisplay = m_brightness != nullptr ? m_brightness->findDisplay(displayId) : nullptr;
            if (currentDisplay == nullptr || !currentDisplay->controllable) {
              return;
            }
            const auto brightness = static_cast<float>(value);
            queueBrightness(displayId, brightness);
            for (auto& c : m_cards) {
              if (c.displayId == displayId) {
                c.lastBrightness = brightness;
                break;
              }
            }
          },
          [this]() { flushPendingBrightness(true); }
      );
    }

    Toggle* mainToggle = nullptr;
    if (source.output != nullptr && niriAvailable && source.output->enabled && enabledCount > 1) {
      const auto* origin = originOutput(m_displayConfig->outputs());
      const bool isMain = origin != nullptr && origin->name == source.id;
      const std::string displayId = source.id;
      mainToggle = addToggleRow(
          *card, scale, i18n::tr("control-center.display.main-display"),
          [this, displayId](bool checked) {
            if (m_syncingConfig || m_displayConfig == nullptr || !checked) {
              return;
            }
            if (!m_displayConfig->setOrigin(displayId)) {
              m_lastDisplayListKey.clear();
              reportLayoutError();
            }
          }
      );
      if (mainToggle != nullptr) {
        mainToggle->setCheckedImmediate(isMain);
        mainToggle->setEnabled(!isMain);
      }
    }

    Select* resolutionSelect = nullptr;
    Select* refreshSelect = nullptr;
    Select* transformSelect = nullptr;
    Slider* scaleSlider = nullptr;
    Label* scaleLabel = nullptr;
    Toggle* vrrToggle = nullptr;
    std::vector<display_config::ResolutionChoice> resolutions;
    std::vector<display_config::Mode> refreshes;
    double currentScale = 1.0;

    if (source.output != nullptr && niriAvailable) {
      const auto& output = *source.output;
      currentScale = output.scale;
      resolutions = display_config::resolutionChoices(output);
      refreshes = display_config::refreshChoices(output, output.current.width, output.current.height);
      const bool controlsEnabled = output.enabled && !resolutions.empty();
      const std::string displayId = source.id;

      std::vector<std::string> resolutionLabels;
      resolutionLabels.reserve(resolutions.size());
      for (const auto& choice : resolutions) {
        auto label = display_config::formatResolution(choice.width, choice.height);
        if (choice.preferred) {
          label += " *";
        }
        resolutionLabels.push_back(std::move(label));
      }
      std::vector<std::string> refreshLabels;
      refreshLabels.reserve(refreshes.size());
      for (const auto& choice : refreshes) {
        refreshLabels.push_back(display_config::formatRefresh(choice.refreshMilliHz));
      }

      addWellDivider(*card, scale);
      if (!resolutionLabels.empty()) {
        const std::size_t selectedResolution = display_config::selectedResolutionIndex(output, resolutions);
        resolutionSelect = addSelectRow(
            *card, scale, i18n::tr("control-center.display.resolution"), std::move(resolutionLabels),
            panelCardOpacity(),
            [this, displayId](std::size_t index) {
              if (!m_syncingConfig) {
                applyResolution(displayId, index);
              }
            }
        );
        if (resolutionSelect != nullptr) {
          resolutionSelect->setSelectedIndexSilently(selectedResolution);
          resolutionSelect->setEnabled(controlsEnabled);
        }
      }
      if (!refreshLabels.empty()) {
        const std::size_t selectedRefresh = display_config::selectedRefreshIndex(output, refreshes);
        refreshSelect = addSelectRow(
            *card, scale, i18n::tr("control-center.display.refresh-rate"), std::move(refreshLabels), panelCardOpacity(),
            [this, displayId](std::size_t index) {
              if (!m_syncingConfig) {
                applyRefresh(displayId, index);
              }
            }
        );
        if (refreshSelect != nullptr) {
          refreshSelect->setSelectedIndexSilently(selectedRefresh);
          refreshSelect->setEnabled(controlsEnabled);
        }
      }

      transformSelect = addSelectRow(
          *card, scale, i18n::tr("control-center.display.rotation"), transformLabels(), panelCardOpacity(),
          [this, displayId](std::size_t index) {
            if (!m_syncingConfig) {
              applyTransform(displayId, index);
            }
          }
      );
      if (transformSelect != nullptr) {
        transformSelect->setSelectedIndexSilently(transformIndex(output.transform));
        transformSelect->setEnabled(output.enabled);
      }

      addLiveSlider(
          *card, scale, i18n::tr("control-center.display.scale"), &scaleSlider, &scaleLabel, kMinScale, kMaxScale,
          kScaleStep, std::clamp(currentScale, kMinScale, kMaxScale), output.enabled, display_config::formatScale,
          [this, displayId](double value) {
            if (m_syncingSlider) {
              return;
            }
            queueScale(displayId, value);
            for (auto& c : m_cards) {
              if (c.displayId == displayId) {
                c.lastScale = value;
                break;
              }
            }
          },
          [this]() { flushPendingScale(true); }
      );

      if (output.vrrSupported) {
        vrrToggle = addToggleRow(
            *card, scale, i18n::tr("control-center.display.vrr"),
            [this, displayId](bool checked) {
              if (m_syncingConfig || m_displayConfig == nullptr) {
                return;
              }
              if (!m_displayConfig->setVrr(displayId, checked)) {
                m_lastDisplayListKey.clear();
              }
            }
        );
        if (vrrToggle != nullptr) {
          vrrToggle->setCheckedImmediate(output.vrrEnabled);
          vrrToggle->setEnabled(output.enabled);
        }
      }
    }

    auto* cardPtr = card.get();
    m_cardsLayout->addChild(std::move(card));
    m_cards.push_back(
        DisplayCard{
            .displayId = source.id,
            .card = cardPtr,
            .nameLabel = nameLabelPtr,
            .detailsLabel = detailsLabelPtr,
            .icon = iconPtr,
            .slider = sliderPtr,
            .valueLabel = valueLabelPtr,
            .resolutionSelect = resolutionSelect,
            .refreshSelect = refreshSelect,
            .transformSelect = transformSelect,
            .scaleSlider = scaleSlider,
            .scaleLabel = scaleLabel,
            .vrrToggle = vrrToggle,
            .enabledToggle = enabledToggle,
            .mainToggle = mainToggle,
            .resolutions = std::move(resolutions),
            .refreshes = std::move(refreshes),
            .lastBrightness = lastBrightness,
            .lastScale = currentScale,
            .lastControllable = lastControllable,
            .lastDisplayInfo = infoText,
        }
    );
  }
  showSelectedDisplayCard();
}

void MonitorTab::queueBrightness(const std::string& displayId, float value) {
  m_pendingDisplayId = displayId;
  m_pendingBrightness = value;

  const auto now = std::chrono::steady_clock::now();
  if (now - m_lastCommitAt >= kBrightnessCommitInterval) {
    flushPendingBrightness();
    return;
  }

  if (!m_debounceTimer.active()) {
    m_debounceTimer.start(kBrightnessCommitInterval, [this]() { flushPendingBrightness(); });
  }
}

void MonitorTab::flushPendingBrightness(bool /*force*/) {
  m_debounceTimer.stop();

  if (m_pendingBrightness < 0.0F || m_brightness == nullptr) {
    return;
  }

  const auto* display = m_brightness->findDisplay(m_pendingDisplayId);
  if (display == nullptr || !display->controllable) {
    m_pendingBrightness = -1.0F;
    return;
  }

  m_brightness->setBrightness(m_pendingDisplayId, m_pendingBrightness);
  m_lastSentBrightness = m_pendingBrightness;
  m_lastCommitAt = std::chrono::steady_clock::now();
  m_ignoreStateUntil = m_lastCommitAt + kBrightnessStateHoldoff;
  m_pendingBrightness = -1.0F;
}

void MonitorTab::queueScale(const std::string& displayId, double value) {
  m_pendingScaleId = displayId;
  m_pendingScale = value;
  if (!m_scaleDebounceTimer.active()) {
    m_scaleDebounceTimer.start(kBrightnessCommitInterval, [this]() { flushPendingScale(); });
  }
}

void MonitorTab::flushPendingScale(bool /*force*/) {
  m_scaleDebounceTimer.stop();
  if (m_pendingScale < 0.0 || m_displayConfig == nullptr) {
    m_pendingScale = -1.0;
    return;
  }
  const auto* output = m_displayConfig->findOutput(m_pendingScaleId);
  if (output == nullptr || !output->enabled) {
    m_pendingScale = -1.0;
    return;
  }
  if (std::abs(output->scale - m_pendingScale) < kScaleSyncEpsilon) {
    m_pendingScale = -1.0;
    return;
  }
  if (!m_displayConfig->setScale(m_pendingScaleId, m_pendingScale)) {
    m_lastDisplayListKey.clear();
  }
  m_pendingScale = -1.0;
}

void MonitorTab::applyResolution(const std::string& displayId, std::size_t index) {
  if (m_displayConfig == nullptr) {
    return;
  }
  const auto* output = m_displayConfig->findOutput(displayId);
  if (output == nullptr) {
    return;
  }
  const auto card = std::ranges::find_if(m_cards, [&](const DisplayCard& entry) { return entry.displayId == displayId; });
  if (card == m_cards.end() || index >= card->resolutions.size()) {
    return;
  }
  const auto& choice = card->resolutions[index];
  const auto refresh = display_config::bestRefreshMilliHz(*output, choice.width, choice.height);
  if (!refresh.has_value()) {
    m_lastDisplayListKey.clear();
    return;
  }
  if (!m_displayConfig->setMode(displayId, choice.width, choice.height, *refresh)) {
    m_lastDisplayListKey.clear();
  }
}

void MonitorTab::applyRefresh(const std::string& displayId, std::size_t index) {
  if (m_displayConfig == nullptr) {
    return;
  }
  const auto* output = m_displayConfig->findOutput(displayId);
  const auto card = std::ranges::find_if(m_cards, [&](const DisplayCard& entry) { return entry.displayId == displayId; });
  if (output == nullptr || card == m_cards.end() || index >= card->refreshes.size()) {
    return;
  }
  const auto& choice = card->refreshes[index];
  if (!m_displayConfig->setMode(displayId, output->current.width, output->current.height, choice.refreshMilliHz)) {
    m_lastDisplayListKey.clear();
  }
}

void MonitorTab::applyTransform(const std::string& displayId, std::size_t index) {
  if (m_displayConfig == nullptr || index >= std::size(kTransforms)) {
    return;
  }
  if (!m_displayConfig->setTransform(displayId, kTransforms[index])) {
    m_lastDisplayListKey.clear();
  }
}

void MonitorTab::applyLayout(display_config::LayoutDirection direction) {
  if (m_displayConfig == nullptr || m_layoutMovingName.empty()) {
    return;
  }
  const std::string referenceName = layoutReferenceName();
  if (referenceName.empty()) {
    return;
  }
  if (!m_displayConfig->placeRelative(m_layoutMovingName, referenceName, direction)) {
    m_lastDisplayListKey.clear();
    reportLayoutError();
    return;
  }
  syncLayoutSelection();
}

void MonitorTab::reportLayoutError() {
  if (m_displayConfig == nullptr) {
    return;
  }
  const std::string error = m_displayConfig->lastError();
  if (error.empty()) {
    return;
  }
  notify::error("Noctalia", i18n::tr("control-center.display.layout-error"), error);
  syncStatusLabels();
}
