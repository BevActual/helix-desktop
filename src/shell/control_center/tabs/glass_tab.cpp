#include "shell/control_center/tabs/glass_tab.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  [[nodiscard]] const char* captionKey(PanelTransparencyMode mode) {
    switch (mode) {
    case PanelTransparencyMode::Soft:
      return "control-center.appearance.glass-soft-caption";
    case PanelTransparencyMode::Glass:
      return "control-center.appearance.glass-glass-caption";
    case PanelTransparencyMode::Solid:
    default:
      return "control-center.appearance.glass-solid-caption";
    }
  }

} // namespace

GlassTab::GlassTab(const ControlCenterServices& services)
    : m_config(services.config),
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

std::unique_ptr<Flex> GlassTab::createLiveBody() {
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;
  const PanelTransparencyMode current =
      m_config != nullptr ? m_config->config().shell.panel.transparencyMode : PanelTransparencyMode::Solid;

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto compactCard = ui::column({
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& card) {
            applySectionCardStyle(card, scale, opacity);
            card.setGap(Style::spaceXs * scale);
            card.setPadding(Style::spaceSm * scale);
          },
  });
  compactCard->addChild(
      ui::segmented({
          .out = &m_modeSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = "",
                      .glyph = "square",
                      .tooltip = i18n::tr("settings.options.shell.panel-transparency.solid"),
                  },
                  {
                      .label = "",
                      .glyph = "blur",
                      .tooltip = i18n::tr("settings.options.shell.panel-transparency.soft"),
                  },
                  {
                      .label = "",
                      .glyph = "glass",
                      .tooltip = i18n::tr("settings.options.shell.panel-transparency.glass"),
                  },
              },
          .selectedIndex = static_cast<std::size_t>(current),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .onChange =
              [this](std::size_t index) {
                if (index >= std::size(kPanelTransparencyModes)) {
                  return;
                }
                applyTransparencyMode(kPanelTransparencyModes[index].value);
              },
      })
  );
  compactCard->addChild(
      ui::label({
          .out = &m_caption,
          .text = i18n::tr(captionKey(current)),
          .fontSize = captionSize,
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
  m_modeSwitch = nullptr;
  m_caption = nullptr;
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

void GlassTab::onPanelCardOpacityChanged(float opacity) {
  if (m_modeSwitch != nullptr) {
    m_modeSwitch->setSurfaceOpacity(opacity);
  }
}

void GlassTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  const PanelTransparencyMode mode = m_config->config().shell.panel.transparencyMode;
  m_syncing = true;
  if (m_modeSwitch != nullptr) {
    m_modeSwitch->setSelectedIndex(static_cast<std::size_t>(mode));
  }
  if (m_caption != nullptr) {
    m_caption->setText(i18n::tr(captionKey(mode)));
  }
  m_syncing = false;
}

void GlassTab::applyTransparencyMode(PanelTransparencyMode mode) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  const auto index = static_cast<std::size_t>(mode);
  if (index >= std::size(kPanelTransparencyModes)) {
    return;
  }
  (void)m_config->setOverride({"shell", "panel", "transparency_mode"}, std::string(kPanelTransparencyModes[index].key));
  syncLiveControls();
  PanelManager::instance().refresh();
}
