#include "shell/control_center/tabs/glass_tab.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "compositors/niri/niri_config_blur.h"
#include "compositors/niri/niri_runtime.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  [[nodiscard]] const char* captionKey(float glass) {
    if (glass < 1.0F / 3.0F) {
      return "control-center.appearance.glass-solid-caption";
    }
    if (glass < 2.0F / 3.0F) {
      return "control-center.appearance.glass-soft-caption";
    }
    return "control-center.appearance.glass-glass-caption";
  }

  [[nodiscard]] const char* refractionCaptionKey(bool helixNiri) {
    if (!compositors::isNiri()) {
      return "control-center.appearance.glass-refraction-other-caption";
    }
    if (helixNiri) {
      return "control-center.appearance.glass-refraction-helix-caption";
    }
    return "control-center.appearance.glass-refraction-stock-caption";
  }

  [[nodiscard]] std::string glassPercentLabel(double value) {
    return std::format("{:.0f}%", std::clamp(value, 0.0, 1.0) * 100.0);
  }

} // namespace

GlassTab::GlassTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_platform(services.platform),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "glass", .showConfigureButton = false}
      )) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> GlassTab::create() {
  return m_settings->wrapLive(createLiveBody(), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> GlassTab::createLiveBody(bool hosted) {
  m_hosted = hosted;
  const float scale = contentScale();
  const float glass =
      m_config != nullptr ? resolvedPanelGlass(m_config->config().shell.panel) : 0.0F;
  const bool refraction = m_config != nullptr && m_config->config().shell.panel.refraction;

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = hosted ? 0.0F : 1.0F,
  });

  auto compactCard = ui::column({
      .configure =
          [scale, opacity = panelCardOpacity(), hosted](Flex& card) {
            applyCompactCardStyle(card, scale, opacity, hosted);
          },
  });
  addSliderSection(
      *compactCard, scale, i18n::tr("control-center.appearance.glass-slider"), &m_glass, &m_glassValue, 0.0, 1.0, 0.01,
      glass, glassPercentLabel, [this]() { commitGlass(); }
  );
  compactCard->addChild(
      ui::label({
          .out = &m_caption,
          .text = i18n::tr(captionKey(glass)),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
      })
  );
  m_refraction = addToggleRow(
      *compactCard, scale, i18n::tr("control-center.appearance.glass-refraction"),
      [this](bool value) { applyRefraction(value); }
  );
  if (m_refraction != nullptr) {
    m_refraction->setCheckedImmediate(refraction);
  }
  compactCard->addChild(
      ui::label({
          .out = &m_refractionCaption,
          .text = i18n::tr(refractionCaptionKey(
              m_platform != nullptr && compositors::niri::detectLiquidGlass(&m_platform->niriRuntime())
          )),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
      })
  );
  tab->addChild(std::move(compactCard));

  syncLiveControls();
  return tab;
}

std::unique_ptr<Flex> GlassTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void GlassTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_glass = nullptr;
  m_glassValue = nullptr;
  m_caption = nullptr;
  m_refraction = nullptr;
  m_refractionCaption = nullptr;
}

void GlassTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool GlassTab::configureMode() const { return m_settings->configureMode(); }

void GlassTab::setSettingsSearchQuery(std::string_view query) {
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* GlassTab::settingsPane() { return m_settings.get(); }

void GlassTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
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

void GlassTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
}

void GlassTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  const float glass = resolvedPanelGlass(m_config->config().shell.panel);
  const bool refraction = m_config->config().shell.panel.refraction;
  m_syncing = true;
  if (m_glass != nullptr) {
    m_glass->setValue(glass);
  }
  if (m_glassValue != nullptr) {
    m_glassValue->setText(glassPercentLabel(glass));
  }
  if (m_caption != nullptr) {
    m_caption->setText(i18n::tr(captionKey(glass)));
  }
  if (m_refraction != nullptr) {
    m_refraction->setCheckedImmediate(refraction);
  }
  if (m_refractionCaption != nullptr) {
    m_refractionCaption->setText(i18n::tr(refractionCaptionKey(
        m_platform != nullptr && compositors::niri::detectLiquidGlass(&m_platform->niriRuntime())
    )));
  }
  m_syncing = false;
}

void GlassTab::commitGlass() {
  if (m_syncing || m_config == nullptr || m_glass == nullptr) {
    return;
  }
  const float glass = static_cast<float>(std::clamp(m_glass->value(), 0.0, 1.0));
  const auto mode = transparencyModeForGlass(glass);
  const auto index = static_cast<std::size_t>(mode);
  std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
  overrides.emplace_back(std::vector<std::string>{"shell", "panel", "glass"}, static_cast<double>(glass));
  if (index < std::size(kPanelTransparencyModes)) {
    overrides.emplace_back(
        std::vector<std::string>{"shell", "panel", "transparency_mode"}, std::string(kPanelTransparencyModes[index].key)
    );
  }
  (void)m_config->setOverrides(std::move(overrides));
  persistCompositorGlass();
  syncLiveControls();
  PanelManager::instance().refresh();
}

void GlassTab::applyRefraction(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "panel", "refraction"}, enabled);
  persistCompositorGlass();
  syncLiveControls();
  PanelManager::instance().refresh();
}

void GlassTab::persistCompositorGlass() {
  if (m_config == nullptr || !compositors::isNiri()) {
    return;
  }
  const compositors::niri::NiriRuntime* runtime = m_platform != nullptr ? &m_platform->niriRuntime() : nullptr;
  (void)compositors::niri::saveNiriBlur(compositors::niri::blurSettingsFromConfig(
      m_config->config(), compositors::niri::detectLiquidGlass(runtime), compositors::niri::detectLiquidGlassParams(runtime)
  ));
}
