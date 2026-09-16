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
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  void setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return;
    }
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
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

  struct OsdKindItem {
    bool OsdKindsConfig::* member;
    const char* tomlKey;
    const char* labelKey;
  };

  constexpr OsdKindItem kOsdKinds[] = {
      {&OsdKindsConfig::volume, "volume", "settings.schema.shell.osd-kinds-volume.label"},
      {&OsdKindsConfig::volumeOutput, "volume_output", "settings.schema.shell.osd-kinds-volume-output.label"},
      {&OsdKindsConfig::volumeInput, "volume_input", "settings.schema.shell.osd-kinds-volume-input.label"},
      {&OsdKindsConfig::brightness, "brightness", "settings.schema.shell.osd-kinds-brightness.label"},
      {&OsdKindsConfig::wifi, "wifi", "settings.schema.shell.osd-kinds-wifi.label"},
      {&OsdKindsConfig::bluetooth, "bluetooth", "settings.schema.shell.osd-kinds-bluetooth.label"},
      {&OsdKindsConfig::powerProfile, "power_profile", "settings.schema.shell.osd-kinds-power-profile.label"},
      {&OsdKindsConfig::caffeine, "caffeine", "settings.schema.shell.osd-kinds-caffeine.label"},
      {&OsdKindsConfig::nightlight, "nightlight", "settings.schema.shell.osd-kinds-nightlight.label"},
      {&OsdKindsConfig::dnd, "dnd", "settings.schema.shell.osd-kinds-dnd.label"},
      {&OsdKindsConfig::lockKeys, "lock_keys", "settings.schema.shell.osd-kinds-lock-keys.label"},
      {&OsdKindsConfig::keyboardLayout, "keyboard_layout", "settings.schema.shell.osd-kinds-keyboard-layout.label"},
      {&OsdKindsConfig::media, "media", "settings.schema.shell.osd-kinds-media.label"},
      {&OsdKindsConfig::privacy, "privacy", "settings.schema.shell.osd-kinds-privacy.label"},
      {&OsdKindsConfig::keyboardBacklight, "keyboard_backlight",
       "settings.schema.shell.osd-kinds-keyboard-backlight.label"},
  };

  constexpr std::size_t kVolumeIndex = 0;
  constexpr std::size_t kVolumeOutputIndex = 1;
  constexpr std::size_t kVolumeInputIndex = 2;

} // namespace

OsdTab::OsdTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "osd"})) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> OsdTab::create() {
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
                                    void (OsdTab::*apply)(bool)) {
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

  const OsdConfig osd = m_config != nullptr ? m_config->config().osd : OsdConfig{};

  auto generalCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addTitle(*generalCard, i18n::tr("settings.navigation.sections.osd"), scale);
  addToggleRow(*generalCard, i18n::tr("control-center.osd.enable"), &m_enabled, osd.enabled, &OsdTab::applyEnabled);

  addCaption(*generalCard, i18n::tr("control-center.osd.orientation"));
  generalCard->addChild(
      ui::segmented({
          .out = &m_orientation,
          .options =
              std::vector<ui::SegmentedOption>{
                  {.label = i18n::tr("settings.options.orientation.horizontal")},
                  {.label = i18n::tr("settings.options.orientation.vertical")},
              },
          .selectedIndex = osd.orientation == "vertical" ? std::size_t{1} : std::size_t{0},
          .fontSize = captionSize,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyOrientation(index); },
      })
  );

  addCaption(*generalCard, i18n::tr("control-center.osd.position"));
  {
    std::vector<std::string> labels;
    labels.reserve(kPositions.size());
    for (const auto& option : kPositions) {
      labels.push_back(i18n::tr(option.labelKey));
    }
    generalCard->addChild(
        ui::select({
            .out = &m_position,
            .options = labels,
            .selectedIndex = positionIndex(osd.position),
            .fontSize = Style::fontSizeBody * scale,
            .controlHeight = controlHeight,
            .surfaceOpacity = panelCardOpacity(),
            .flexGrow = 1.0F,
            .onSelectionChanged = [this](std::size_t index, std::string_view) { applyPosition(index); },
        })
    );
  }

  auto scaleHeader = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  scaleHeader->addChild(
      ui::label({
          .text = i18n::tr("control-center.osd.scale"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .flexGrow = 1.0F,
      })
  );
  scaleHeader->addChild(
      ui::label({
          .out = &m_scaleValue,
          .text = multiplierLabel(osd.scale),
          .fontSize = captionSize,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  generalCard->addChild(std::move(scaleHeader));
  generalCard->addChild(
      ui::slider({
          .out = &m_scale,
          .minValue = static_cast<double>(noctalia::config::schema::kScaleRange.min.value_or(0.5F)),
          .maxValue = static_cast<double>(noctalia::config::schema::kScaleRange.max.value_or(2.5F)),
          .step = static_cast<double>(noctalia::config::schema::kScaleRange.step.value_or(0.05F)),
          .value = static_cast<double>(osd.scale),
          .trackHeight = Style::sliderTrackHeight * scale,
          .thumbSize = Style::sliderThumbSize * scale,
          .controlHeight = controlHeight,
          .flexGrow = 1.0F,
          .onValueChanged =
              [this](double value) {
                if (m_scaleValue != nullptr) {
                  m_scaleValue->setText(multiplierLabel(value));
                }
              },
          .onDragEnd = [this]() { commitScale(); },
      })
  );

  auto opacityHeader = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  opacityHeader->addChild(
      ui::label({
          .text = i18n::tr("control-center.osd.opacity"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .flexGrow = 1.0F,
      })
  );
  opacityHeader->addChild(
      ui::label({
          .out = &m_opacityValue,
          .text = percentLabel(osd.backgroundOpacity),
          .fontSize = captionSize,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  generalCard->addChild(std::move(opacityHeader));
  generalCard->addChild(
      ui::slider({
          .out = &m_opacity,
          .minValue = static_cast<double>(noctalia::config::schema::kUnitRange.min.value_or(0.0F)),
          .maxValue = static_cast<double>(noctalia::config::schema::kUnitRange.max.value_or(1.0F)),
          .step = static_cast<double>(noctalia::config::schema::kUnitRange.step.value_or(0.01F)),
          .value = static_cast<double>(osd.backgroundOpacity),
          .trackHeight = Style::sliderTrackHeight * scale,
          .thumbSize = Style::sliderThumbSize * scale,
          .controlHeight = controlHeight,
          .flexGrow = 1.0F,
          .onValueChanged =
              [this](double value) {
                if (m_opacityValue != nullptr) {
                  m_opacityValue->setText(percentLabel(value));
                }
              },
          .onDragEnd = [this]() { commitOpacity(); },
      })
  );
  addToggleRow(*generalCard, i18n::tr("control-center.osd.border"), &m_border, osd.border, &OsdTab::applyBorder);
  content->addChild(std::move(generalCard));

  auto kindsCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addTitle(*kindsCard, i18n::tr("control-center.osd.kinds-title"), scale);
  kindsCard->addChild(
      ui::label({
          .text = i18n::tr("control-center.osd.kinds-description"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
      })
  );

  m_kindToggles.assign(std::size(kOsdKinds), nullptr);
  m_volumeOutputRow = nullptr;
  m_volumeInputRow = nullptr;
  for (std::size_t i = 0; i < std::size(kOsdKinds); ++i) {
    const auto& item = kOsdKinds[i];
    const bool checked = osd.kinds.*(item.member);
    const bool indented = i == kVolumeOutputIndex || i == kVolumeInputIndex;
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .paddingH = indented ? Style::spaceLg * scale : 0.0F,
        .minHeight = Style::controlHeightSm * scale,
    });
    Flex* rowPtr = row.get();
    if (i == kVolumeOutputIndex) {
      m_volumeOutputRow = rowPtr;
    } else if (i == kVolumeInputIndex) {
      m_volumeInputRow = rowPtr;
    }
    row->addChild(
        ui::label({
            .text = i18n::tr(item.labelKey),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
    row->addChild(
        ui::toggle({
            .out = &m_kindToggles[i],
            .checkedImmediate = checked,
            .toggleSize = ToggleSize::Medium,
            .scale = scale,
            .onChange = [this, i](bool value) { applyKind(i, value); },
        })
    );
    kindsCard->addChild(std::move(row));
  }
  content->addChild(std::move(kindsCard));

  live->addChild(std::move(scroll));
  auto wrapped = m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
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
  m_orientation = nullptr;
  m_position = nullptr;
  m_scale = nullptr;
  m_scaleValue = nullptr;
  m_opacity = nullptr;
  m_opacityValue = nullptr;
  m_border = nullptr;
  m_kindToggles.clear();
  m_volumeOutputRow = nullptr;
  m_volumeInputRow = nullptr;
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

void OsdTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  const OsdConfig& osd = m_config->config().osd;
  m_syncing = true;
  if (m_enabled != nullptr) {
    m_enabled->setCheckedImmediate(osd.enabled);
  }
  if (m_orientation != nullptr) {
    m_orientation->setSelectedIndex(osd.orientation == "vertical" ? 1 : 0);
  }
  if (m_position != nullptr) {
    m_position->setSelectedIndexSilently(positionIndex(osd.position));
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
  for (std::size_t i = 0; i < std::size(kOsdKinds) && i < m_kindToggles.size(); ++i) {
    if (m_kindToggles[i] != nullptr) {
      m_kindToggles[i]->setCheckedImmediate(osd.kinds.*(kOsdKinds[i].member));
    }
  }
  refreshKindVisibility();
  m_syncing = false;
}

void OsdTab::refreshKindVisibility() {
  if (m_config == nullptr) {
    return;
  }
  const bool volumeOn = m_config->config().osd.kinds.volume;
  setShown(m_volumeOutputRow, volumeOn);
  setShown(m_volumeInputRow, volumeOn);
}

void OsdTab::applyEnabled(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"osd", "enabled"}, enabled);
  PanelManager::instance().refresh();
}

void OsdTab::applyOrientation(std::size_t index) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"osd", "orientation"}, std::string(index == 1 ? "vertical" : "horizontal"));
  PanelManager::instance().refresh();
}

void OsdTab::applyPosition(std::size_t index) {
  if (m_syncing || m_config == nullptr || index >= kPositions.size()) {
    return;
  }
  (void)m_config->setOverride({"osd", "position"}, std::string(kPositions[index].value));
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

void OsdTab::applyKind(std::size_t index, bool enabled) {
  if (m_syncing || m_config == nullptr || index >= std::size(kOsdKinds)) {
    return;
  }
  (void)m_config->setOverride({"osd", "kinds", kOsdKinds[index].tomlKey}, enabled);
  if (index == kVolumeIndex) {
    refreshKindVisibility();
  }
}
