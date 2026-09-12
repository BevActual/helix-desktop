#include "shell/control_center/tabs/monitor_tab.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_button_style.h"
#include "system/brightness_service.h"
#include "system/display_config_service.h"
#include "ui/builders.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>

using namespace control_center;

namespace {

  constexpr float kBrightnessSyncEpsilon = 0.005F;
  constexpr float kScaleSyncEpsilon = 0.02F;
  constexpr auto kBrightnessCommitInterval = std::chrono::milliseconds(16);
  constexpr auto kBrightnessStateHoldoff = std::chrono::milliseconds(180);
  constexpr double kMinScale = 0.5;
  constexpr double kMaxScale = 3.0;
  constexpr double kScaleStep = 0.05;

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
      key += displayConfig->fingerprint();
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

  std::unique_ptr<Flex> labeledField(float scale, const std::string& label, std::unique_ptr<Select> control) {
    return ui::column(
        {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0F},
        ui::label({
            .text = label,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        }),
        std::move(control)
    );
  }

} // namespace

MonitorTab::MonitorTab(
    BrightnessService* brightness, ConfigService* config, DisplayConfigService* displayConfig,
    const ControlCenterServices& services
)
    : m_brightness(brightness), m_configService(config), m_displayConfig(displayConfig),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "services", .includedGroups = {"brightness"}}
      )) {}

std::unique_ptr<Flex> MonitorTab::create() {
  const float scale = contentScale();

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  tab->addChild(
      ui::label({
          .out = &m_noteLabel,
          .text = i18n::tr("control-center.display.temporary"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
      })
  );
  tab->addChild(
      ui::label({
          .out = &m_errorLabel,
          .text = "",
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Error),
          .visible = false,
      })
  );

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
  tab->addChild(std::move(cardsScroll));

  auto emptyState = ui::column(
      {.out = &m_emptyState,
       .align = FlexAlign::Center,
       .justify = FlexJustify::Center,
       .flexGrow = 1.0F,
       .visible = false},
      ui::label({
          .text = i18n::tr("control-center.display.no-displays"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  tab->addChild(std::move(emptyState));

  syncStatusLabels();
  return m_settings->takeHost(std::move(tab), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> MonitorTab::createHeaderActions() {
  const float scale = contentScale();
  auto live = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
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
  return m_settings->createEmbedHeaderActions(std::move(live), scale);
}

void MonitorTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool MonitorTab::configureMode() const { return m_settings->configureMode(); }

void MonitorTab::syncHeaderActions() {
  if (m_rescanButton == nullptr) {
    return;
  }
  const bool ddc = m_configService != nullptr && m_configService->config().brightness.enableDdcutil;
  const bool niri = m_displayConfig != nullptr && m_displayConfig->available();
  m_rescanButton->setVisible(ddc || niri);
}

void MonitorTab::syncStatusLabels() {
  const bool niri = m_displayConfig != nullptr && m_displayConfig->available();
  if (m_noteLabel != nullptr) {
    m_noteLabel->setVisible(niri);
  }
  if (m_errorLabel != nullptr) {
    const std::string error = m_displayConfig != nullptr ? m_displayConfig->lastError() : std::string{};
    m_errorLabel->setText(error);
    m_errorLabel->setVisible(!error.empty());
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
  m_layoutSelect = nullptr;
  m_rescanButton = nullptr;
  m_cards.clear();
  m_layoutOutputNames.clear();
  m_lastDisplayListKey.clear();
}

bool MonitorTab::dragging() const noexcept {
  for (const auto& card : m_cards) {
    if (card.slider != nullptr && card.slider->dragging()) {
      return true;
    }
    if (card.scaleSlider != nullptr && card.scaleSlider->dragging()) {
      return true;
    }
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

  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);

  const float scale = contentScale();
  const float cardWidth =
      std::max(1.0F, m_cardsScroll != nullptr ? m_cardsScroll->contentViewportWidth() : contentWidth);
  const float cardInnerWidth = std::max(1.0F, cardWidth - Style::spaceMd * scale * 2.0F);
  const float headerTextMaxWidth =
      std::max(1.0F, cardInnerWidth - Style::fontSizeTitle * scale - Style::spaceSm * scale);
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

  m_rootLayout->layout(renderer);
}

void MonitorTab::doUpdate(Renderer& renderer) {
  rebuildCards(renderer);
  syncHeaderActions();
  syncStatusLabels();

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

void MonitorTab::rebuildLayoutSection(float scale) {
  m_layoutSection = nullptr;
  m_layoutSelect = nullptr;
  m_layoutOutputNames.clear();

  if (m_cardsLayout == nullptr || m_displayConfig == nullptr || !m_displayConfig->available()) {
    return;
  }

  const auto& outputs = m_displayConfig->outputs();
  std::vector<std::string> names;
  std::vector<std::string> labels;
  for (const auto& output : outputs) {
    if (!output.enabled) {
      continue;
    }
    names.push_back(output.name);
    labels.push_back(display_config::displayLabel(output) + " · " + output.name);
  }
  if (names.size() < 2) {
    return;
  }

  if (m_layoutMovingName.empty()
      || std::ranges::find(names, m_layoutMovingName) == names.end()) {
    m_layoutMovingName = defaultMovingName(outputs);
  }
  std::size_t selectedIndex = 0;
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (names[index] == m_layoutMovingName) {
      selectedIndex = index;
      break;
    }
  }

  auto section = ui::column({
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) {
        applySectionCardStyle(card, scale, opacity);
        card.setGap(Style::spaceMd * scale);
      },
  });

  section->addChild(
      ui::label({
          .text = i18n::tr("control-center.display.layout"),
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  section->addChild(
      ui::label({
          .text = i18n::tr("control-center.display.layout-hint"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );

  auto movingSelect = ui::select({
      .out = &m_layoutSelect,
      .options = labels,
      .selectedIndex = selectedIndex,
      .surfaceOpacity = panelCardOpacity(),
      .flexGrow = 1.0F,
      .onSelectionChanged =
          [this](std::size_t index, std::string_view /*text*/) {
            if (index < m_layoutOutputNames.size()) {
              m_layoutMovingName = m_layoutOutputNames[index];
            }
          },
  });
  section->addChild(labeledField(scale, i18n::tr("control-center.display.move"), std::move(movingSelect)));

  const float buttonSize = Style::controlHeight * scale;
  auto makeMoveButton = [this, scale, buttonSize](
                            const char* glyph, display_config::LayoutDirection direction, const std::string& tooltip
                        ) {
    return ui::button({
        .glyph = glyph,
        .tooltip = tooltip,
        .width = buttonSize,
        .height = buttonSize,
        .onClick = [this, direction]() { applyLayout(direction); },
        .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
    });
  };
  auto makePad = [buttonSize]() {
    return ui::row({.width = buttonSize, .height = buttonSize});
  };

  auto dpad = ui::column({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
  dpad->addChild(
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceSm * scale},
          makePad(),
          makeMoveButton(
              "chevron-up", display_config::LayoutDirection::Top, i18n::tr("control-center.display.place-top")
          ),
          makePad()
      )
  );
  dpad->addChild(
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceSm * scale},
          makeMoveButton(
              "chevron-left", display_config::LayoutDirection::Left, i18n::tr("control-center.display.place-left")
          ),
          makePad(),
          makeMoveButton(
              "chevron-right", display_config::LayoutDirection::Right, i18n::tr("control-center.display.place-right")
          )
      )
  );
  dpad->addChild(
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceSm * scale},
          makePad(),
          makeMoveButton(
              "chevron-down", display_config::LayoutDirection::Bottom, i18n::tr("control-center.display.place-bottom")
          ),
          makePad()
      )
  );
  section->addChild(
      ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center}, std::move(dpad))
  );

  m_layoutOutputNames = std::move(names);
  m_layoutSection = section.get();
  m_cardsLayout->addChild(std::move(section));
}

void MonitorTab::rebuildCards(Renderer& /*renderer*/) {
  if (m_cardsLayout == nullptr) {
    return;
  }

  const std::vector<BrightnessDisplay> noDisplays;
  const auto& displays = m_brightness != nullptr ? m_brightness->displays() : noDisplays;
  const std::string key = buildDisplayListKey(displays, m_displayConfig);
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
  if (m_layoutSection != nullptr) {
    m_cardsLayout->removeChild(m_layoutSection);
    m_layoutSection = nullptr;
    m_layoutSelect = nullptr;
  }

  const auto sources = collectCardSources(m_brightness, m_displayConfig);
  const bool empty = sources.empty();
  if (m_emptyState != nullptr) {
    m_emptyState->setVisible(empty);
  }
  if (m_cardsScroll != nullptr) {
    m_cardsScroll->setVisible(!empty);
  }
  if (empty) {
    return;
  }

  const float scale = contentScale();
  rebuildLayoutSection(scale);

  const bool niriAvailable = m_displayConfig != nullptr && m_displayConfig->available();
  const std::size_t enabledCount = m_displayConfig != nullptr ? m_displayConfig->enabledCount() : 0;

  for (const auto& source : sources) {
    auto card = ui::column({
        .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
          applySectionCardStyle(section, scale, opacity);
          section.setGap(Style::spaceMd * scale);
        },
    });

    Glyph* iconPtr = nullptr;
    Label* nameLabelPtr = nullptr;
    Toggle* enabledToggle = nullptr;
    auto headerRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    headerRow->addChild(
        ui::glyph({
            .out = &iconPtr,
            .glyph = "device-desktop",
            .glyphSize = Style::fontSizeTitle * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    headerRow->addChild(
        ui::label({
            .out = &nameLabelPtr,
            .text = source.label,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
    if (source.output != nullptr) {
      const std::string displayId = source.id;
      const bool canDisable = source.output->enabled && enabledCount > 1;
      headerRow->addChild(
          ui::toggle({
              .out = &enabledToggle,
              .checkedImmediate = source.output->enabled,
              .enabled = source.output->enabled ? canDisable : true,
              .toggleSize = ToggleSize::Small,
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
    card->addChild(
        ui::label({
            .out = &detailsLabelPtr,
            .text = infoText,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );

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

      const std::string displayId = source.id;
      auto resolution = ui::select({
          .out = &resolutionSelect,
          .options = std::move(resolutionLabels),
          .selectedIndex = display_config::selectedResolutionIndex(output, resolutions),
          .placeholder = i18n::tr("control-center.display.resolution"),
          .enabled = controlsEnabled,
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0F,
          .onSelectionChanged =
              [this, displayId](std::size_t index, std::string_view /*text*/) {
                if (!m_syncingConfig) {
                  applyResolution(displayId, index);
                }
              },
      });
      auto refresh = ui::select({
          .out = &refreshSelect,
          .options = std::move(refreshLabels),
          .selectedIndex = display_config::selectedRefreshIndex(output, refreshes),
          .placeholder = i18n::tr("control-center.display.refresh-rate"),
          .enabled = controlsEnabled && !refreshes.empty(),
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0F,
          .onSelectionChanged =
              [this, displayId](std::size_t index, std::string_view /*text*/) {
                if (!m_syncingConfig) {
                  applyRefresh(displayId, index);
                }
              },
      });
      card->addChild(
          ui::row(
              {.align = FlexAlign::Start, .gap = Style::spaceSm * scale},
              labeledField(scale, i18n::tr("control-center.display.resolution"), std::move(resolution)),
              labeledField(scale, i18n::tr("control-center.display.refresh-rate"), std::move(refresh))
          )
      );

      auto rotation = ui::select({
          .out = &transformSelect,
          .options = transformLabels(),
          .selectedIndex = transformIndex(output.transform),
          .placeholder = i18n::tr("control-center.display.rotation"),
          .enabled = output.enabled,
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0F,
          .onSelectionChanged =
              [this, displayId](std::size_t index, std::string_view /*text*/) {
                if (!m_syncingConfig) {
                  applyTransform(displayId, index);
                }
              },
      });
      card->addChild(labeledField(scale, i18n::tr("control-center.display.rotation"), std::move(rotation)));

      auto scaleHeader = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
      scaleHeader->addChild(
          ui::label({
              .text = i18n::tr("control-center.display.scale"),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .flexGrow = 1.0F,
          })
      );
      scaleHeader->addChild(
          ui::label({
              .out = &scaleLabel,
              .text = display_config::formatScale(currentScale),
              .fontSize = Style::fontSizeCaption * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
          })
      );
      card->addChild(std::move(scaleHeader));
      card->addChild(
          ui::slider({
              .out = &scaleSlider,
              .minValue = kMinScale,
              .maxValue = kMaxScale,
              .step = kScaleStep,
              .value = std::clamp(currentScale, kMinScale, kMaxScale),
              .enabled = output.enabled,
              .trackHeight = Style::sliderTrackHeight * scale,
              .thumbSize = Style::sliderThumbSize * scale,
              .controlHeight = Style::controlHeight * scale,
              .flexGrow = 1.0F,
              .onValueChanged =
                  [this, displayId](double value) {
                    if (m_syncingSlider) {
                      return;
                    }
                    queueScale(displayId, value);
                    for (auto& c : m_cards) {
                      if (c.displayId == displayId && c.scaleLabel != nullptr) {
                        c.scaleLabel->setText(display_config::formatScale(value));
                        c.lastScale = value;
                        break;
                      }
                    }
                  },
              .onDragEnd = [this]() { flushPendingScale(true); },
          })
      );

      if (output.vrrSupported) {
        auto vrrRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
        vrrRow->addChild(
            ui::label({
                .text = i18n::tr("control-center.display.vrr"),
                .fontSize = Style::fontSizeCaption * scale,
                .color = colorSpecFromRole(ColorRole::OnSurface),
                .flexGrow = 1.0F,
            })
        );
        vrrRow->addChild(
            ui::toggle({
                .out = &vrrToggle,
                .checkedImmediate = output.vrrEnabled,
                .enabled = output.enabled,
                .toggleSize = ToggleSize::Small,
                .scale = scale,
                .onChange =
                    [this, displayId](bool checked) {
                      if (m_syncingConfig || m_displayConfig == nullptr) {
                        return;
                      }
                      if (!m_displayConfig->setVrr(displayId, checked)) {
                        m_lastDisplayListKey.clear();
                      }
                    },
            })
        );
        card->addChild(std::move(vrrRow));
      }
    }

    Slider* sliderPtr = nullptr;
    Label* valueLabelPtr = nullptr;
    float lastBrightness = 0.0F;
    bool lastControllable = false;
    if (source.brightness != nullptr) {
      const auto& display = *source.brightness;
      lastBrightness = display.brightness;
      lastControllable = display.controllable;
      auto sliderRow = ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
          ui::glyph({
              .glyph = "brightness-low",
              .glyphSize = Style::fontSizeTitle * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );

      const std::string displayId = display.id;
      const float minBrightness = m_configService->config().brightness.minimumBrightness;
      auto slider = ui::slider({
          .out = &sliderPtr,
          .minValue = minBrightness,
          .maxValue = 1.0F,
          .step = 0.01F,
          .value = std::max(display.brightness, minBrightness),
          .enabled = display.controllable,
          .trackHeight = Style::sliderTrackHeight * scale,
          .thumbSize = Style::sliderThumbSize * scale,
          .controlHeight = Style::controlHeight * scale,
          .flexGrow = 1.0F,
          .onValueChanged =
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
                  if (c.displayId == displayId && c.valueLabel != nullptr) {
                    c.valueLabel->setText(formatBrightnessValue(*currentDisplay, brightness));
                    c.lastBrightness = brightness;
                    break;
                  }
                }
              },
          .onDragEnd = [this]() { flushPendingBrightness(true); },
      });
      sliderRow->addChild(std::move(slider));
      sliderRow->addChild(
          ui::glyph({
              .glyph = "brightness-high",
              .glyphSize = Style::fontSizeTitle * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
      sliderRow->addChild(
          ui::label({
              .out = &valueLabelPtr,
              .text = formatBrightnessValue(display, display.brightness),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .minWidth = Style::controlHeightLg * scale,
          })
      );
      card->addChild(std::move(sliderRow));
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
            .resolutions = std::move(resolutions),
            .refreshes = std::move(refreshes),
            .lastBrightness = lastBrightness,
            .lastScale = currentScale,
            .lastControllable = lastControllable,
            .lastDisplayInfo = infoText,
        }
    );
  }
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
  const auto* reference = referenceOutput(m_displayConfig->outputs(), m_layoutMovingName);
  if (reference == nullptr) {
    return;
  }
  if (!m_displayConfig->placeRelative(m_layoutMovingName, reference->name, direction)) {
    m_lastDisplayListKey.clear();
  }
}
