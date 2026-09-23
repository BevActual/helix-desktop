#include "shell/control_center/tabs/osd_tab.h"

#include "config/config_service.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  [[nodiscard]] bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return changed;
  }

  [[nodiscard]] std::string multiplierLabel(double value) { return std::format("{:.2f}×", value); }
  [[nodiscard]] std::string percentLabel(double value) { return std::format("{}%", static_cast<int>(value * 100.0)); }

  struct PositionOption {
    const char* value;
    const char* labelKey;
  };

  constexpr std::array<PositionOption, 8> kPositions{{
      {"top_right", "settings.options.screen-position.top-right"},
      {"top_left", "settings.options.screen-position.top-left"},
      {"top_center", "settings.options.screen-position.top-center"},
      {"bottom_right", "settings.options.screen-position.bottom-right"},
      {"bottom_left", "settings.options.screen-position.bottom-left"},
      {"bottom_center", "settings.options.screen-position.bottom-center"},
      {"center_right", "settings.options.screen-position.center-right"},
      {"center_left", "settings.options.screen-position.center-left"},
  }};

  [[nodiscard]] std::size_t positionIndex(const std::string& value) {
    for (std::size_t i = 0; i < kPositions.size(); ++i) {
      if (value == kPositions[i].value) {
        return i;
      }
    }
    return 0;
  }

} // namespace

std::size_t OsdTab::indexFromSection(Section section) noexcept {
  return section == Section::Style ? 1 : 0;
}

OsdTab::Section OsdTab::sectionFromIndex(std::size_t index) noexcept {
  return index == 1 ? Section::Style : Section::Placement;
}

OsdTab::Section OsdTab::sectionFromId(std::string_view id) noexcept {
  return id == "style" ? Section::Style : Section::Placement;
}

std::string_view OsdTab::idFromSection(Section section) noexcept {
  return section == Section::Style ? "style" : "placement";
}

OsdTab::OsdTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "osd", .showConfigureButton = false}
      )) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> section = m_config->stateString("osd_tab", "section")) {
      m_section = sectionFromId(*section);
    }
  }
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> OsdTab::create() {
  return m_settings->wrapLive(createLiveBody(), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> OsdTab::createLiveBody(bool hosted) {
  m_hosted = hosted;
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;
  const float opacity = panelCardOpacity();
  const OsdConfig osd = m_config != nullptr ? m_config->config().osd : OsdConfig{};

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = hosted ? 0.0F : 1.0F,
  });

  if (!hosted) {
    auto compactCard = ui::column({
        .configure =
            [scale, opacity, hosted](Flex& card) {
              applyCompactCardStyle(card, scale, opacity, hosted);
            },
    });
    m_enabled = addToggleRow(*compactCard, scale, i18n::tr("control-center.osd.enable"), [this](bool value) {
      applyEnabled(value);
    });
    if (m_enabled != nullptr) {
      m_enabled->setCheckedImmediate(osd.enabled);
    }

    auto sectionToolbar = ui::column({
        .out = &m_sectionToolbar,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .visible = osd.enabled,
        .participatesInLayout = osd.enabled,
    });
    sectionToolbar->addChild(
        ui::segmented({
            .out = &m_sectionSwitch,
            .options =
                std::vector<ui::SegmentedOption>{
                    {
                        .label = "",
                        .glyph = "layout-navbar",
                        .tooltip = i18n::tr("control-center.osd.placement-tooltip"),
                    },
                    {
                        .label = "",
                        .glyph = "contrast",
                        .tooltip = i18n::tr("control-center.osd.style-tooltip"),
                    },
                },
            .selectedIndex = indexFromSection(m_section),
            .scale = scale,
            .compact = true,
            .surfaceOpacity = opacity,
            .onChange = [this](std::size_t index) { applySection(index); },
        })
    );
    sectionToolbar->addChild(
        ui::label({
            .out = &m_caption,
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 2,
        })
    );
    compactCard->addChild(std::move(sectionToolbar));
    live->addChild(std::move(compactCard));
  }

  const bool placementOpen = osd.enabled && m_section == Section::Placement;
  auto placementHost = ui::column({
      .out = &m_placementHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = hosted ? 0.0F : 1.0F,
      .visible = placementOpen,
      .participatesInLayout = placementOpen,
      .configure =
          [scale, hosted](Flex& section) {
            if (!hosted) {
              applyContentCardStyle(section, scale);
            }
          },
  });
  addCaption(*placementHost, i18n::tr("control-center.osd.orientation"), scale);
  placementHost->addChild(
      ui::segmented({
          .out = &m_orientation,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = i18n::tr("settings.options.orientation.horizontal"),
                      .tooltip = i18n::tr("control-center.osd.orientation-horizontal-tooltip"),
                  },
                  {
                      .label = i18n::tr("settings.options.orientation.vertical"),
                      .tooltip = i18n::tr("control-center.osd.orientation-vertical-tooltip"),
                  },
              },
          .selectedIndex = osd.orientation == "vertical" ? std::size_t{1} : std::size_t{0},
          .fontSize = captionSize,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = opacity,
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyOrientation(index); },
      })
  );
  std::vector<std::string> labels;
  labels.reserve(kPositions.size());
  for (const auto& option : kPositions) {
    labels.push_back(i18n::tr(option.labelKey));
  }
  const bool vertical = osd.orientation == "vertical";
  m_position = addSelectRow(
      *placementHost, scale, i18n::tr("control-center.osd.position"), std::move(labels), opacity,
      [this](std::size_t index) { applyPosition(index); }
  );
  if (m_position != nullptr) {
    m_position->setSelectedIndexSilently(positionIndex(vertical ? osd.positionVertical : osd.position));
  }
  live->addChild(std::move(placementHost));

  const bool styleOpen = osd.enabled && m_section == Section::Style;
  auto styleHost = ui::column({
      .out = &m_styleHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = hosted ? 0.0F : 1.0F,
      .visible = styleOpen,
      .participatesInLayout = styleOpen,
      .configure =
          [scale, hosted](Flex& section) {
            if (!hosted) {
              applyContentCardStyle(section, scale);
            }
          },
  });
  addSliderSection(
      *styleHost, scale, i18n::tr("control-center.osd.scale"), &m_scale, &m_scaleValue,
      static_cast<double>(noctalia::config::schema::kScaleRange.min.value_or(0.5F)),
      static_cast<double>(noctalia::config::schema::kScaleRange.max.value_or(2.5F)),
      static_cast<double>(noctalia::config::schema::kScaleRange.step.value_or(0.05F)), static_cast<double>(osd.scale),
      multiplierLabel, [this]() { commitScale(); }
  );
  addSliderSection(
      *styleHost, scale, i18n::tr("control-center.osd.opacity"), &m_opacity, &m_opacityValue,
      static_cast<double>(noctalia::config::schema::kUnitRange.min.value_or(0.0F)),
      static_cast<double>(noctalia::config::schema::kUnitRange.max.value_or(1.0F)),
      static_cast<double>(noctalia::config::schema::kUnitRange.step.value_or(0.01F)),
      static_cast<double>(osd.backgroundOpacity), percentLabel, [this]() { commitOpacity(); }
  );
  m_border = addToggleRow(*styleHost, scale, i18n::tr("control-center.osd.border"), [this](bool value) {
    applyBorder(value);
  });
  if (m_border != nullptr) {
    m_border->setCheckedImmediate(osd.border);
  }
  live->addChild(std::move(styleHost));

  syncLiveControls();
  return live;
}

std::unique_ptr<Flex> OsdTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void OsdTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_enabled = nullptr;
  m_sectionToolbar = nullptr;
  m_sectionSwitch = nullptr;
  m_caption = nullptr;
  m_placementHost = nullptr;
  m_styleHost = nullptr;
  m_orientation = nullptr;
  m_position = nullptr;
  m_scale = nullptr;
  m_scaleValue = nullptr;
  m_opacity = nullptr;
  m_opacityValue = nullptr;
  m_border = nullptr;
}

void OsdTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool OsdTab::configureMode() const { return m_settings->configureMode(); }

void OsdTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

SettingsPane* OsdTab::settingsPane() { return m_settings.get(); }

void OsdTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  if (m_hosted) {
    layoutHostedLiveBody(m_rootLayout, renderer, contentWidth);
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void OsdTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
}

void OsdTab::onPanelCardOpacityChanged(float opacity) {
  if (m_sectionSwitch != nullptr) {
    m_sectionSwitch->setSurfaceOpacity(opacity);
  }
  if (m_orientation != nullptr) {
    m_orientation->setSurfaceOpacity(opacity);
  }
  if (m_position != nullptr) {
    m_position->setSurfaceOpacity(opacity);
  }
}

void OsdTab::syncHosts() {
  const bool enabled = m_config != nullptr && m_config->config().osd.enabled;
  const bool placementOpen = enabled && m_section == Section::Placement;
  const bool styleOpen = enabled && m_section == Section::Style;
  bool layoutDirty = false;
  layoutDirty |= setShown(m_sectionToolbar, enabled);
  layoutDirty |= setShown(m_placementHost, placementOpen);
  layoutDirty |= setShown(m_styleHost, styleOpen);
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
}

void OsdTab::persistSection() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("osd_tab", "section", std::string(idFromSection(m_section)));
}

void OsdTab::applySection(std::size_t index) {
  if (m_sectionSyncing) {
    return;
  }
  const Section section = sectionFromIndex(index);
  if (m_section == section) {
    syncHosts();
    return;
  }
  m_section = section;
  persistSection();
  if (m_sectionSwitch != nullptr) {
    m_sectionSyncing = true;
    m_sectionSwitch->setSelectedIndex(indexFromSection(m_section));
    m_sectionSyncing = false;
  }
  syncHosts();
  syncLiveControls();
}

void OsdTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  const OsdConfig& osd = m_config->config().osd;
  m_syncing = true;
  if (m_enabled != nullptr) {
    m_enabled->setCheckedImmediate(osd.enabled);
  }
  if (m_caption != nullptr) {
    m_caption->setText(
        m_section == Section::Style ? i18n::tr("control-center.osd.style-caption")
                                    : i18n::tr("control-center.osd.placement-caption")
    );
  }
  if (m_orientation != nullptr) {
    m_orientation->setSelectedIndex(osd.orientation == "vertical" ? 1 : 0);
  }
  if (m_position != nullptr) {
    const std::string& pos = osd.orientation == "vertical" ? osd.positionVertical : osd.position;
    m_position->setSelectedIndexSilently(positionIndex(pos));
  }
  if (m_scale != nullptr) {
    m_scale->setValue(osd.scale);
  }
  if (m_scaleValue != nullptr) {
    m_scaleValue->setText(multiplierLabel(osd.scale));
  }
  if (m_opacity != nullptr) {
    m_opacity->setValue(osd.backgroundOpacity);
  }
  if (m_opacityValue != nullptr) {
    m_opacityValue->setText(percentLabel(osd.backgroundOpacity));
  }
  if (m_border != nullptr) {
    m_border->setCheckedImmediate(osd.border);
  }
  syncHosts();
  m_syncing = false;
}

void OsdTab::applyEnabled(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"osd", "enabled"}, enabled);
  syncLiveControls();
  PanelManager::instance().refresh();
}

void OsdTab::applyOrientation(std::size_t index) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  const bool vertical = index == 1;
  (void)m_config->setOverride({"osd", "orientation"}, std::string(vertical ? "vertical" : "horizontal"));
  if (m_position != nullptr) {
    const auto& osd = m_config->config().osd;
    const std::string& pos = vertical ? osd.positionVertical : osd.position;
    m_position->setSelectedIndexSilently(positionIndex(pos));
  }
  PanelManager::instance().refresh();
}

void OsdTab::applyPosition(std::size_t index) {
  if (m_syncing || m_config == nullptr || index >= kPositions.size()) {
    return;
  }
  const bool vertical = m_config->config().osd.orientation == "vertical";
  (void)m_config->setOverride(
      {"osd", vertical ? "position_vertical" : "position"}, std::string(kPositions[index].value)
  );
  PanelManager::instance().refresh();
}

void OsdTab::applyBorder(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"osd", "border"}, enabled);
}

void OsdTab::commitScale() {
  if (m_syncing || m_config == nullptr || m_scale == nullptr) {
    return;
  }
  (void)m_config->setOverride({"osd", "scale"}, m_scale->value());
}

void OsdTab::commitOpacity() {
  if (m_syncing || m_config == nullptr || m_opacity == nullptr) {
    return;
  }
  (void)m_config->setOverride({"osd", "background_opacity"}, m_opacity->value());
}
