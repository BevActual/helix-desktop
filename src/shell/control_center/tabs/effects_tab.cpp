#include "shell/control_center/tabs/effects_tab.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <chrono>
#include <format>
#include <functional>
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

  [[nodiscard]] std::string percentLabel(double value) { return std::format("{:.0f}%", value * 100.0); }

  Button::ButtonPalette treatmentTogglePalette() {
    const ColorSpec fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.55F);
    const ColorSpec label = colorSpecFromRole(ColorRole::OnSurface);
    const ColorSpec restBorder = colorSpecFromRole(ColorRole::Outline, 0.28F);
    const ColorSpec selectedBorder = colorSpecFromRole(ColorRole::Primary);
    Button::ButtonStateColors rest{.bg = fill, .border = restBorder, .label = label};
    Button::ButtonStateColors selected{.bg = fill, .border = selectedBorder, .label = label};
    return {
        .borderWidth = 2.0F,
        .normal = rest,
        .hover = selected,
        .pressed = selected,
        .disabled = rest,
        .selected = selected,
    };
  }

  std::unique_ptr<Button> makeTreatmentToggle(
      Button** out, std::string_view glyph, const std::string& label, bool selected, float scale,
      std::function<void()> onClick
  ) {
    const float size = Style::controlHeightSm * scale;
    return ui::button({
        .out = out,
        .glyph = std::string(glyph),
        .glyphSize = Style::baseGlyphSize * scale,
        .selected = selected,
        .customPalette = treatmentTogglePalette(),
        .tooltip = label,
        .minWidth = size,
        .minHeight = size,
        .maxWidth = size,
        .maxHeight = size,
        .padding = Style::spaceXs * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = std::move(onClick),
    });
  }

} // namespace

EffectsTab::EffectsTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "effects", .showConfigureButton = false}
      )) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> catalog = m_config->stateString("effects_tab", "catalog")) {
      m_catalog = catalogFromState(*catalog);
      m_pinnedCatalog = m_catalog;
    }
  }
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> EffectsTab::create() {
  return m_settings->wrapLive(createLiveBody(), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> EffectsTab::createLiveBody(bool hosted) {
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;
  m_syncing = true;

  const bool highContrast = m_config != nullptr && m_config->config().accessibility.highContrast;
  const bool animations = m_config != nullptr && m_config->config().shell.animation.enabled;

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto compactCard = ui::column({
      .configure =
          [scale, opacity = panelCardOpacity(), hosted](Flex& card) {
            applyCompactCardStyle(card, scale, opacity, hosted);
          },
  });
  auto toolbar = ui::row({
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
  });
  toolbar->addChild(makeTreatmentToggle(
      &m_contrastTile, "contrast", i18n::tr("settings.schema.accessibility.high-contrast.label"), highContrast, scale,
      [this]() {
        if (m_config == nullptr) {
          return;
        }
        applyHighContrast(!m_config->config().accessibility.highContrast);
      }
  ));
  toolbar->addChild(makeTreatmentToggle(
      &m_motionTile, "player-play", i18n::tr("settings.schema.appearance.animations.label"), animations, scale,
      [this]() {
        if (m_config == nullptr) {
          return;
        }
        applyAnimations(!m_config->config().shell.animation.enabled);
      }
  ));
  toolbar->addChild(
      ui::segmented({
          .out = &m_catalogSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = "",
                      .glyph = "border-all",
                      .tooltip = i18n::tr("control-center.appearance.effects-borders"),
                  },
                  {
                      .label = "",
                      .glyph = "shadow",
                      .tooltip = i18n::tr("control-center.appearance.effects-shadows"),
                  },
                  {
                      .label = "",
                      .glyph = "player-play",
                      .tooltip = i18n::tr("control-center.appearance.effects-speed"),
                  },
              },
          .selectedIndex = indexFromCatalog(m_catalog),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = false,
          .onChange = [this](std::size_t index) { applyCatalog(index, true); },
      })
  );
  compactCard->addChild(std::move(toolbar));
  compactCard->addChild(
      ui::label({
          .out = &m_caption,
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
      })
  );
  tab->addChild(std::move(compactCard));

  auto leftover = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });
  const float opacity = panelCardOpacity();
  auto makeHost = [&](Flex** out, bool visible) {
    return ui::column({
        .out = out,
        .align = FlexAlign::Stretch,
        .flexGrow = 1.0F,
        .visible = visible,
        .participatesInLayout = visible,
        .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
    });
  };

  auto borders = makeHost(&m_bordersHost, true);
  m_buttonBorders = addToggleRow(
      *borders, scale, i18n::tr("settings.schema.appearance.button-borders.label"),
      [this](bool enabled) {
        if (!m_syncing && m_config != nullptr) {
          (void)m_config->setOverride({"shell", "button_borders"}, enabled);
        }
      }
  );
  addWellDivider(*borders, scale);
  m_inputBorders = addToggleRow(
      *borders, scale, i18n::tr("settings.schema.appearance.input-borders.label"),
      [this](bool enabled) {
        if (!m_syncing && m_config != nullptr) {
          (void)m_config->setOverride({"shell", "input_borders"}, enabled);
        }
      }
  );
  addWellDivider(*borders, scale);
  m_popupBorders = addToggleRow(
      *borders, scale, i18n::tr("settings.schema.appearance.popup-borders.label"),
      [this](bool enabled) {
        if (!m_syncing && m_config != nullptr) {
          (void)m_config->setOverride({"shell", "popup_borders"}, enabled);
        }
      }
  );
  addWellDivider(*borders, scale);
  m_cardBorders = addToggleRow(
      *borders, scale, i18n::tr("settings.schema.appearance.card-borders.label"),
      [this](bool enabled) {
        if (!m_syncing && m_config != nullptr) {
          (void)m_config->setOverride({"shell", "card_borders"}, enabled);
          syncLiveControls();
        }
      }
  );
  leftover->addChild(std::move(borders));

  auto shadows = makeHost(&m_shadowsHost, false);
  m_popupShadows = addToggleRow(
      *shadows, scale, i18n::tr("settings.schema.appearance.popup-shadows.label"),
      [this](bool enabled) {
        if (!m_syncing && m_config != nullptr) {
          (void)m_config->setOverride({"shell", "popup_shadows"}, enabled);
          syncLiveControls();
          PanelManager::instance().requestLayout();
        }
      }
  );
  auto shadowDetails = ui::column({
      .out = &m_shadowDetails,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });
  addWellDivider(*shadowDetails, scale);
  std::vector<std::string> directionLabels;
  m_shadowDirectionKeys.clear();
  for (const auto& option : kShadowDirections) {
    m_shadowDirectionKeys.emplace_back(option.key);
    directionLabels.emplace_back(i18n::tr(option.labelKey));
  }
  m_shadowDirection = addSelectRow(
      *shadowDetails, scale, i18n::tr("settings.schema.shared.shadow-direction.label"), std::move(directionLabels),
      opacity, [this](std::size_t index) { applyShadowDirection(index); }
  );
  addSliderSection(
      *shadowDetails, scale, i18n::tr("settings.schema.shared.shadow-alpha.label"), &m_shadowAlpha, &m_shadowAlphaValue,
      noctalia::config::schema::kUnitRange.min.value_or(0.0F), noctalia::config::schema::kUnitRange.max.value_or(1.0F),
      noctalia::config::schema::kUnitRange.step.value_or(0.01F), 0.55, percentLabel, [this]() { commitShadowAlpha(); }
  );
  shadows->addChild(std::move(shadowDetails));
  leftover->addChild(std::move(shadows));

  auto speed = makeHost(&m_speedHost, false);
  auto speedSection = ui::column({
      .out = &m_animationSpeedSection,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });
  addSliderSection(
      *speedSection, scale, i18n::tr("settings.schema.appearance.animation-speed.label"), &m_animationSpeed,
      &m_animationSpeedValue, noctalia::config::schema::kAnimationSpeedRange.min.value_or(0.1F),
      noctalia::config::schema::kAnimationSpeedRange.max.value_or(4.0F),
      noctalia::config::schema::kAnimationSpeedRange.step.value_or(0.05F), 1.0, multiplierLabel,
      [this]() { commitAnimationSpeed(); }
  );
  speed->addChild(std::move(speedSection));
  speed->addChild(
      ui::label({
          .out = &m_speedOffCaption,
          .text = i18n::tr("control-center.appearance.effects-speed-off"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  leftover->addChild(std::move(speed));
  tab->addChild(std::move(leftover));

  syncLiveControls();
  syncCatalog();
  return tab;
}

std::unique_ptr<Flex> EffectsTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void EffectsTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_contrastTile = nullptr;
  m_motionTile = nullptr;
  m_catalogSwitch = nullptr;
  m_caption = nullptr;
  m_bordersHost = nullptr;
  m_shadowsHost = nullptr;
  m_speedHost = nullptr;
  m_animationSpeedSection = nullptr;
  m_animationSpeed = nullptr;
  m_animationSpeedValue = nullptr;
  m_speedOffCaption = nullptr;
  m_buttonBorders = nullptr;
  m_inputBorders = nullptr;
  m_popupBorders = nullptr;
  m_cardBorders = nullptr;
  m_popupShadows = nullptr;
  m_shadowDetails = nullptr;
  m_shadowDirection = nullptr;
  m_shadowAlpha = nullptr;
  m_shadowAlphaValue = nullptr;
}

void EffectsTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool EffectsTab::configureMode() const { return m_settings->configureMode(); }

void EffectsTab::setSettingsSearchQuery(std::string_view query) {
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* EffectsTab::settingsPane() { return m_settings.get(); }

void EffectsTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  syncCatalog();
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void EffectsTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
}

void EffectsTab::refreshCaption() {
  if (m_caption == nullptr) {
    return;
  }
  const char* key = "control-center.appearance.borders-hint";
  switch (m_catalog) {
  case Catalog::Shadows:
    key = "settings.schema.appearance.popup-shadows.description";
    break;
  case Catalog::Speed:
    key = "settings.schema.appearance.animations.description";
    break;
  case Catalog::Borders:
    break;
  }
  m_caption->setText(i18n::tr(key));
}

void EffectsTab::applyHighContrast(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"accessibility", "high_contrast"}, enabled);
  syncLiveControls();
}

void EffectsTab::applyAnimations(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "animation", "enabled"}, enabled);
  syncLiveControls();
  PanelManager::instance().requestLayout();
}

void EffectsTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  const Config& cfg = m_config->config();
  m_syncing = true;
  if (m_contrastTile != nullptr) {
    m_contrastTile->setSelected(cfg.accessibility.highContrast);
  }
  if (m_motionTile != nullptr) {
    m_motionTile->setSelected(cfg.shell.animation.enabled);
  }
  refreshCaption();
  if (m_animationSpeed != nullptr) {
    m_animationSpeed->setValue(cfg.shell.animation.speed);
  }
  if (m_animationSpeedValue != nullptr) {
    m_animationSpeedValue->setText(multiplierLabel(cfg.shell.animation.speed));
  }
  if (m_buttonBorders != nullptr) {
    m_buttonBorders->setCheckedImmediate(cfg.shell.buttonBorders);
  }
  if (m_inputBorders != nullptr) {
    m_inputBorders->setCheckedImmediate(cfg.shell.inputBorders);
  }
  if (m_popupBorders != nullptr) {
    m_popupBorders->setCheckedImmediate(cfg.shell.popupBorders);
  }
  if (m_cardBorders != nullptr) {
    m_cardBorders->setCheckedImmediate(cfg.shell.cardBorders);
  }
  if (m_popupShadows != nullptr) {
    m_popupShadows->setCheckedImmediate(cfg.shell.popupShadows);
  }
  if (setShown(m_shadowDetails, cfg.shell.popupShadows)) {
    PanelManager::instance().requestLayout();
  }
  if (m_shadowDirection != nullptr) {
    const std::string_view key = enumToKey(kShadowDirections, cfg.shell.shadow.direction);
    std::size_t index = 0;
    for (std::size_t i = 0; i < m_shadowDirectionKeys.size(); ++i) {
      if (m_shadowDirectionKeys[i] == key) {
        index = i;
        break;
      }
    }
    m_shadowDirection->setSelectedIndexSilently(index);
  }
  if (m_shadowAlpha != nullptr) {
    m_shadowAlpha->setValue(cfg.shell.shadow.alpha);
  }
  if (m_shadowAlphaValue != nullptr) {
    m_shadowAlphaValue->setText(percentLabel(cfg.shell.shadow.alpha));
  }
  m_syncing = false;
  syncCatalog();
}

void EffectsTab::commitAnimationSpeed() {
  if (m_syncing || m_config == nullptr || m_animationSpeed == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "animation", "speed"}, m_animationSpeed->value());
}

void EffectsTab::commitShadowAlpha() {
  if (m_syncing || m_config == nullptr || m_shadowAlpha == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "shadow", "alpha"}, m_shadowAlpha->value());
}

void EffectsTab::applyShadowDirection(std::size_t index) {
  if (m_syncing || m_config == nullptr || index >= m_shadowDirectionKeys.size()) {
    return;
  }
  (void)m_config->setOverride({"shell", "shadow", "direction"}, m_shadowDirectionKeys[index]);
}

EffectsTab::Catalog EffectsTab::catalogFromState(std::string_view value) noexcept {
  if (value == "shadows") {
    return Catalog::Shadows;
  }
  if (value == "speed") {
    return Catalog::Speed;
  }
  return Catalog::Borders;
}

std::string_view EffectsTab::catalogState(Catalog catalog) noexcept {
  switch (catalog) {
  case Catalog::Shadows:
    return "shadows";
  case Catalog::Speed:
    return "speed";
  case Catalog::Borders:
    break;
  }
  return "borders";
}

std::size_t EffectsTab::indexFromCatalog(Catalog catalog) noexcept {
  switch (catalog) {
  case Catalog::Shadows:
    return 1;
  case Catalog::Speed:
    return 2;
  case Catalog::Borders:
    break;
  }
  return 0;
}

EffectsTab::Catalog EffectsTab::catalogFromIndex(std::size_t index) noexcept {
  switch (index) {
  case 1:
    return Catalog::Shadows;
  case 2:
    return Catalog::Speed;
  default:
    return Catalog::Borders;
  }
}

void EffectsTab::persistCatalog() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("effects_tab", "catalog", std::string(catalogState(m_pinnedCatalog)));
}

void EffectsTab::syncCatalog() {
  const bool borders = m_catalog == Catalog::Borders;
  const bool shadows = m_catalog == Catalog::Shadows;
  const bool speed = m_catalog == Catalog::Speed;
  const bool motionOn = m_config != nullptr && m_config->config().shell.animation.enabled;
  bool dirty = false;
  dirty |= setShown(m_bordersHost, borders);
  dirty |= setShown(m_shadowsHost, shadows);
  dirty |= setShown(m_speedHost, speed);
  dirty |= setShown(m_animationSpeedSection, speed && motionOn);
  dirty |= setShown(m_speedOffCaption, speed && !motionOn);
  refreshCaption();
  if (m_catalogSwitch != nullptr && m_catalogSwitch->selectedIndex() != indexFromCatalog(m_pinnedCatalog)) {
    m_catalogSyncing = true;
    m_catalogSwitch->setSelectedIndex(indexFromCatalog(m_pinnedCatalog));
    m_catalogSyncing = false;
    dirty = true;
  }
  if (dirty) {
    PanelManager::instance().requestLayout();
  }
}

void EffectsTab::applyCatalog(std::size_t index, bool persist) {
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
  m_catalog = next;
  syncCatalog();
}

void EffectsTab::onPanelCardOpacityChanged(float opacity) {
  if (m_catalogSwitch != nullptr) {
    m_catalogSwitch->setSurfaceOpacity(opacity);
  }
}
