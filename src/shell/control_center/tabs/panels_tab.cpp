#include "shell/control_center/tabs/panels_tab.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <cstddef>
#include <cstdint>
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

  constexpr std::array<const char*, 2> kLayerValues = {"top", "overlay"};

  template <std::size_t N>
  [[nodiscard]] std::size_t indexOf(const std::array<const char*, N>& values, const std::string& value) {
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (value == values[i]) {
        return i;
      }
    }
    return 0;
  }

  [[nodiscard]] std::string pixelLabel(double value) { return std::format("{}px", static_cast<int>(value)); }

} // namespace

std::size_t PanelsTab::indexFromSection(Section section) noexcept {
  return section == Section::Chrome ? 1 : 0;
}

PanelsTab::Section PanelsTab::sectionFromIndex(std::size_t index) noexcept {
  return index == 1 ? Section::Chrome : Section::Placement;
}

PanelsTab::Section PanelsTab::sectionFromId(std::string_view id) noexcept {
  return id == "chrome" ? Section::Chrome : Section::Placement;
}

std::string_view PanelsTab::idFromSection(Section section) noexcept {
  return section == Section::Chrome ? "chrome" : "placement";
}

PanelsTab::PanelsTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "panels", .showConfigureButton = false}
      )) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> section = m_config->stateString("panels_tab", "section")) {
      m_section = sectionFromId(*section);
    }
  }
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> PanelsTab::create() {
  return m_settings->wrapLive(createLiveBody(), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> PanelsTab::createLiveBody(bool hosted) {
  m_hosted = hosted;
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;
  const float opacity = panelCardOpacity();
  const ShellConfig::PanelConfig panel =
      m_config != nullptr ? m_config->config().shell.panel : ShellConfig::PanelConfig{};

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
    compactCard->addChild(
        ui::segmented({
            .out = &m_sectionSwitch,
            .options =
                std::vector<ui::SegmentedOption>{
                    {
                        .label = "",
                        .glyph = "layers-intersect",
                        .tooltip = i18n::tr("control-center.panels.placement-tooltip"),
                    },
                    {
                        .label = "",
                        .glyph = "border-all",
                        .tooltip = i18n::tr("control-center.panels.chrome-tooltip"),
                    },
                },
            .selectedIndex = indexFromSection(m_section),
            .scale = scale,
            .compact = true,
            .surfaceOpacity = opacity,
            .onChange = [this](std::size_t index) { applySection(index); },
        })
    );
    compactCard->addChild(
        ui::label({
            .out = &m_caption,
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 2,
        })
    );
    live->addChild(std::move(compactCard));
  }

  const bool placementOpen = m_section == Section::Placement;
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
  addCaption(*placementHost, i18n::tr("settings.schema.panels.floating-layer.label"), scale);
  placementHost->addChild(
      ui::segmented({
          .out = &m_floatingLayer,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = i18n::tr("settings.options.layer.top"),
                      .tooltip = i18n::tr("control-center.panels.layer-top-tooltip"),
                  },
                  {
                      .label = i18n::tr("settings.options.layer.overlay"),
                      .tooltip = i18n::tr("control-center.panels.layer-overlay-tooltip"),
                  },
              },
          .selectedIndex = indexOf(kLayerValues, panel.floatingLayer),
          .fontSize = captionSize,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = opacity,
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyFloatingLayer(index); },
      })
  );
  addSliderSection(
      *placementHost, scale, i18n::tr("settings.schema.panels.floating-offset.label"), &m_floatingOffset,
      &m_floatingOffsetValue, 0.0, 100.0, 1.0, static_cast<double>(panel.floatingOffset), pixelLabel,
      [this]() { commitFloatingOffset(); }
  );
  live->addChild(std::move(placementHost));

  const bool chromeOpen = m_section == Section::Chrome;
  auto chromeHost = ui::column({
      .out = &m_chromeHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = hosted ? 0.0F : 1.0F,
      .visible = chromeOpen,
      .participatesInLayout = chromeOpen,
      .configure =
          [scale, hosted](Flex& section) {
            if (!hosted) {
              applyContentCardStyle(section, scale);
            }
          },
  });
  m_borders = addToggleRow(
      *chromeHost, scale, i18n::tr("settings.schema.panels.borders.label"), [this](bool value) { applyBorders(value); }
  );
  if (m_borders != nullptr) {
    m_borders->setCheckedImmediate(panel.borders);
  }
  addWellDivider(*chromeHost, scale);
  m_shadow = addToggleRow(
      *chromeHost, scale, i18n::tr("settings.schema.shared.shadow.label"), [this](bool value) { applyShadow(value); }
  );
  if (m_shadow != nullptr) {
    m_shadow->setCheckedImmediate(panel.shadow);
  }
  addWellDivider(*chromeHost, scale);
  m_listItemBackground = addToggleRow(
      *chromeHost, scale, i18n::tr("control-center.panels.list-rows"),
      [this](bool value) { applyListItemBackground(value); }
  );
  if (m_listItemBackground != nullptr) {
    m_listItemBackground->setCheckedImmediate(panel.listItemBackground);
  }
  live->addChild(std::move(chromeHost));

  syncLiveControls();
  return live;
}

std::unique_ptr<Flex> PanelsTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void PanelsTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_sectionSwitch = nullptr;
  m_caption = nullptr;
  m_placementHost = nullptr;
  m_chromeHost = nullptr;
  m_floatingLayer = nullptr;
  m_borders = nullptr;
  m_shadow = nullptr;
  m_listItemBackground = nullptr;
  m_floatingOffset = nullptr;
  m_floatingOffsetValue = nullptr;
}

void PanelsTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool PanelsTab::configureMode() const { return m_settings->configureMode(); }

void PanelsTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

SettingsPane* PanelsTab::settingsPane() { return m_settings.get(); }

void PanelsTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
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

void PanelsTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
}

void PanelsTab::onPanelCardOpacityChanged(float opacity) {
  if (m_sectionSwitch != nullptr) {
    m_sectionSwitch->setSurfaceOpacity(opacity);
  }
  if (m_floatingLayer != nullptr) {
    m_floatingLayer->setSurfaceOpacity(opacity);
  }
}

void PanelsTab::syncHosts() {
  const bool placementOpen = m_section == Section::Placement;
  bool layoutDirty = false;
  layoutDirty |= setShown(m_placementHost, placementOpen);
  layoutDirty |= setShown(m_chromeHost, !placementOpen);
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
}

void PanelsTab::persistSection() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("panels_tab", "section", std::string(idFromSection(m_section)));
}

void PanelsTab::applySection(std::size_t index) {
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

void PanelsTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  const ShellConfig::PanelConfig& panel = m_config->config().shell.panel;
  m_syncing = true;
  if (m_caption != nullptr) {
    m_caption->setText(
        m_section == Section::Chrome ? i18n::tr("control-center.panels.chrome-caption")
                                     : i18n::tr("control-center.panels.placement-caption")
    );
  }
  if (m_floatingLayer != nullptr) {
    m_floatingLayer->setSelectedIndex(indexOf(kLayerValues, panel.floatingLayer));
  }
  if (m_borders != nullptr) {
    m_borders->setCheckedImmediate(panel.borders);
  }
  if (m_shadow != nullptr) {
    m_shadow->setCheckedImmediate(panel.shadow);
  }
  if (m_listItemBackground != nullptr) {
    m_listItemBackground->setCheckedImmediate(panel.listItemBackground);
  }
  if (m_floatingOffset != nullptr) {
    m_floatingOffset->setValue(panel.floatingOffset);
  }
  if (m_floatingOffsetValue != nullptr) {
    m_floatingOffsetValue->setText(pixelLabel(static_cast<double>(panel.floatingOffset)));
  }
  syncHosts();
  m_syncing = false;
}

void PanelsTab::applyFloatingLayer(std::size_t index) {
  if (m_syncing || m_config == nullptr || index >= kLayerValues.size()) {
    return;
  }
  (void)m_config->setOverride({"shell", "panel", "floating_layer"}, std::string(kLayerValues[index]));
}

void PanelsTab::applyBorders(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "panel", "borders"}, enabled);
}

void PanelsTab::applyShadow(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "panel", "shadow"}, enabled);
}

void PanelsTab::applyListItemBackground(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "panel", "list_item_background"}, enabled);
}

void PanelsTab::commitFloatingOffset() {
  if (m_syncing || m_config == nullptr || m_floatingOffset == nullptr) {
    return;
  }
  (void)m_config->setOverride(
      {"shell", "panel", "floating_offset"}, static_cast<std::int64_t>(m_floatingOffset->value())
  );
}
