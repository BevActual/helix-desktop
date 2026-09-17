#include "shell/control_center/tabs/effects_tab.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "shell/settings/color_spec_picker.h"
#include "ui/app_icon_colorization.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <format>
#include <iterator>
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

} // namespace

EffectsTab::EffectsTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(
          services,
          SettingsPaneOptions{.section = "effects", .includedGroups = {"icons"}, .showConfigureButton = false}
      )) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> EffectsTab::create() {
  const float scale = contentScale();
  m_syncing = true;

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
  auto makeWell = [scale, opacity]() {
    return ui::column({
        .align = FlexAlign::Stretch,
        .configure = [scale, opacity](Flex& section) { applySectionCardStyle(section, scale, opacity); },
    });
  };

  auto chromeWell = makeWell();
  std::vector<std::string> transparencyLabels;
  transparencyLabels.reserve(std::size(kPanelTransparencyModes));
  for (const auto& option : kPanelTransparencyModes) {
    transparencyLabels.emplace_back(i18n::tr(option.labelKey));
  }
  m_transparencyMode = addSelectRow(
      *chromeWell, scale, i18n::tr("settings.schema.panels.transparency-mode.label"), std::move(transparencyLabels),
      opacity, [this](std::size_t index) { applyTransparencyMode(index); }
  );
  addCaption(*chromeWell, i18n::tr("settings.schema.panels.transparency-mode.description"), scale);
  content->addChild(std::move(chromeWell));

  auto treatmentWell = makeWell();
  m_highContrast = addToggleRow(
      *treatmentWell, scale, i18n::tr("settings.schema.accessibility.high-contrast.label"),
      [this](bool enabled) {
        if (!m_syncing && m_config != nullptr) {
          (void)m_config->setOverride({"accessibility", "high_contrast"}, enabled);
        }
      }
  );

  addWellDivider(*treatmentWell, scale);
  m_iconColorize = addToggleRow(
      *treatmentWell, scale, i18n::tr("settings.schema.appearance.app-icon-colorize.label"),
      [this](bool enabled) {
        if (!m_syncing && m_config != nullptr) {
          (void)m_config->setOverride({"shell", "app_icon_colorize"}, enabled);
          syncLiveControls();
          PanelManager::instance().requestLayout();
        }
      }
  );
  auto iconColorSection = ui::column({
      .out = &m_iconColorSection,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });
  addWellDivider(*iconColorSection, scale);
  auto iconColorRow = makeControlRow(i18n::tr("settings.schema.appearance.app-icon-color.label"), scale, nullptr, false);
  std::string iconColorValue;
  if (m_config != nullptr) {
    ShellConfig colorizeShell = m_config->config().shell;
    colorizeShell.appIconColorize = true;
    if (const auto tint = effectiveShellAppIconColorizationTint(colorizeShell)) {
      iconColorValue = settings::colorSpecConfigValue(*tint);
    }
  }
  iconColorRow->addChild(settings::makeColorSpecSelect(
      settings::ColorSpecSelectOptions{
          .selectedValue = std::move(iconColorValue),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .out = &m_iconColor,
      },
      [this](std::string value) {
        if (!m_syncing && m_config != nullptr) {
          (void)m_config->setOverride({"shell", "app_icon_color"}, std::move(value));
        }
      },
      [this]() {
        if (!m_syncing && m_config != nullptr) {
          (void)m_config->clearOverride({"shell", "app_icon_color"});
        }
      }
  ));
  iconColorSection->addChild(std::move(iconColorRow));
  treatmentWell->addChild(std::move(iconColorSection));
  addCaption(*treatmentWell, i18n::tr("settings.schema.accessibility.high-contrast.description"), scale);
  addCaption(*treatmentWell, i18n::tr("settings.schema.appearance.app-icon-colorize.description"), scale);

  addWellDivider(*treatmentWell, scale);
  m_animations = addToggleRow(
      *treatmentWell, scale, i18n::tr("settings.schema.appearance.animations.label"),
      [this](bool enabled) {
        if (!m_syncing && m_config != nullptr) {
          (void)m_config->setOverride({"shell", "animation", "enabled"}, enabled);
          syncLiveControls();
          PanelManager::instance().requestLayout();
        }
      }
  );
  auto speedSection = ui::column({
      .out = &m_animationSpeedSection,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });
  addWellDivider(*speedSection, scale);
  addSliderSection(
      *speedSection, scale, i18n::tr("settings.schema.appearance.animation-speed.label"), &m_animationSpeed,
      &m_animationSpeedValue, noctalia::config::schema::kAnimationSpeedRange.min.value_or(0.1F),
      noctalia::config::schema::kAnimationSpeedRange.max.value_or(4.0F),
      noctalia::config::schema::kAnimationSpeedRange.step.value_or(0.05F), 1.0, multiplierLabel,
      [this]() { commitAnimationSpeed(); }
  );
  treatmentWell->addChild(std::move(speedSection));
  addCaption(*treatmentWell, i18n::tr("settings.schema.appearance.animations.description"), scale);

  addWellDivider(*treatmentWell, scale);
  m_buttonBorders = addToggleRow(
      *treatmentWell, scale, i18n::tr("settings.schema.appearance.button-borders.label"),
      [this](bool enabled) {
        if (!m_syncing && m_config != nullptr) {
          (void)m_config->setOverride({"shell", "button_borders"}, enabled);
        }
      }
  );
  addWellDivider(*treatmentWell, scale);
  m_inputBorders = addToggleRow(
      *treatmentWell, scale, i18n::tr("settings.schema.appearance.input-borders.label"),
      [this](bool enabled) {
        if (!m_syncing && m_config != nullptr) {
          (void)m_config->setOverride({"shell", "input_borders"}, enabled);
        }
      }
  );
  addWellDivider(*treatmentWell, scale);
  m_popupBorders = addToggleRow(
      *treatmentWell, scale, i18n::tr("settings.schema.appearance.popup-borders.label"),
      [this](bool enabled) {
        if (!m_syncing && m_config != nullptr) {
          (void)m_config->setOverride({"shell", "popup_borders"}, enabled);
        }
      }
  );
  addWellDivider(*treatmentWell, scale);
  m_cardBorders = addToggleRow(
      *treatmentWell, scale, i18n::tr("settings.schema.appearance.card-borders.label"),
      [this](bool enabled) {
        if (!m_syncing && m_config != nullptr) {
          (void)m_config->setOverride({"shell", "card_borders"}, enabled);
        }
      }
  );
  addCaption(*treatmentWell, i18n::tr("control-center.appearance.borders-hint"), scale);

  addWellDivider(*treatmentWell, scale);
  m_popupShadows = addToggleRow(
      *treatmentWell, scale, i18n::tr("settings.schema.appearance.popup-shadows.label"),
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
  treatmentWell->addChild(std::move(shadowDetails));
  addCaption(*treatmentWell, i18n::tr("settings.schema.appearance.popup-shadows.description"), scale);
  content->addChild(std::move(treatmentWell));

  live->addChild(std::move(scroll));
  auto wrapped = m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
}

std::unique_ptr<Flex> EffectsTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void EffectsTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_transparencyMode = nullptr;
  m_highContrast = nullptr;
  m_iconColorize = nullptr;
  m_iconColorSection = nullptr;
  m_iconColor = nullptr;
  m_animations = nullptr;
  m_animationSpeedSection = nullptr;
  m_animationSpeed = nullptr;
  m_animationSpeedValue = nullptr;
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
  if (!query.empty()) {
    m_settings->setConfigureMode(true);
  }
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

void EffectsTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  const Config& cfg = m_config->config();
  m_syncing = true;
  if (m_transparencyMode != nullptr) {
    m_transparencyMode->setSelectedIndexSilently(static_cast<std::size_t>(cfg.shell.panel.transparencyMode));
  }
  if (m_highContrast != nullptr) {
    m_highContrast->setCheckedImmediate(cfg.accessibility.highContrast);
  }
  if (m_iconColorize != nullptr) {
    m_iconColorize->setCheckedImmediate(cfg.shell.appIconColorize);
  }
  if (setShown(m_iconColorSection, cfg.shell.appIconColorize)) {
    PanelManager::instance().requestLayout();
  }
  if (m_animations != nullptr) {
    m_animations->setCheckedImmediate(cfg.shell.animation.enabled);
  }
  if (m_animationSpeed != nullptr) {
    m_animationSpeed->setValue(cfg.shell.animation.speed);
  }
  if (m_animationSpeedValue != nullptr) {
    m_animationSpeedValue->setText(multiplierLabel(cfg.shell.animation.speed));
  }
  if (setShown(m_animationSpeedSection, cfg.shell.animation.enabled)) {
    PanelManager::instance().requestLayout();
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

void EffectsTab::applyTransparencyMode(std::size_t index) {
  if (m_syncing || m_config == nullptr || index >= std::size(kPanelTransparencyModes)) {
    return;
  }
  (void)m_config->setOverride(
      {"shell", "panel", "transparency_mode"}, std::string(kPanelTransparencyModes[index].key)
  );
  PanelManager::instance().refresh();
}
