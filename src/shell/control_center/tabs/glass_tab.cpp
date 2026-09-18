#include "shell/control_center/tabs/glass_tab.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <string>
#include <utility>

using namespace control_center;

namespace {

  Button::ButtonPalette materialTilePalette() {
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

  void insertBeforeLabel(Button& button, std::unique_ptr<Node> child) {
    std::size_t labelIndex = button.children().size();
    for (std::size_t i = 0; i < button.children().size(); ++i) {
      if (button.children()[i].get() == button.label()) {
        labelIndex = i;
        break;
      }
    }
    button.insertChildAt(labelIndex, std::move(child));
  }

  [[nodiscard]] float previewCardOpacity(PanelTransparencyMode mode) {
    return panelCardOpacityForTransparencyMode(mode, detachedPanelBackgroundOpacityForTransparencyMode(mode));
  }

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

  std::unique_ptr<Flex> makeMaterialPreview(PanelTransparencyMode mode, float scale, float height, Flex** panelOut) {
    const float pad = Style::spaceXs * scale;
    const float radius = Style::scaledRadiusLg(scale);
    // Solid = opaque chrome. Soft = Regular glass. Glass = Clear glass, which
    // needs a dimming layer so leftover type stays readable over wallpaper.
    const ColorSpec desktopFill = mode == PanelTransparencyMode::Glass
        ? colorSpecFromRole(ColorRole::OnSurface, 0.22F)
        : colorSpecFromRole(ColorRole::SurfaceVariant);
    auto desktop = ui::column({
        .align = FlexAlign::Stretch,
        .justify = FlexJustify::Center,
        .padding = pad,
        .fill = desktopFill,
        .minHeight = height,
        .fillWidth = true,
        .fillHeight = true,
        .flexGrow = 1.0F,
        .configure = [](Flex& host) { host.setHitTestVisible(false); },
    });
    auto panel = ui::column({
        .out = panelOut,
        .align = FlexAlign::Stretch,
        .gap = std::max(2.0F, 3.0F * scale),
        .padding = std::max(4.0F, 5.0F * scale),
        .fill = colorSpecFromRole(ColorRole::Surface, previewCardOpacity(mode)),
        .radius = radius,
        .border = colorSpecFromRole(ColorRole::Outline, 0.32F),
        .borderWidth = 1.0F,
        .minHeight = (panelOut != nullptr ? 72.0F : 34.0F) * scale,
        .fillWidth = true,
        .fillHeight = panelOut != nullptr,
        .clipChildren = true,
        .flexGrow = 1.0F,
    });
    panel->addChild(
        ui::row({
            .fill = colorSpecFromRole(ColorRole::Primary),
            .radius = 2.0F * scale,
            .minHeight = (panelOut != nullptr ? 8.0F : 5.0F) * scale,
            .fillWidth = true,
        })
    );
    panel->addChild(
        ui::row({
            .fill = colorSpecFromRole(ColorRole::Outline, 0.45F),
            .radius = 2.0F * scale,
            .minHeight = (panelOut != nullptr ? 6.0F : 4.0F) * scale,
            .maxWidth = (panelOut != nullptr ? 96.0F : 48.0F) * scale,
            .fillWidth = true,
        })
    );
    if (panelOut != nullptr) {
      panel->addChild(
          ui::row({
              .fill = colorSpecFromRole(ColorRole::Outline, 0.28F),
              .radius = 2.0F * scale,
              .minHeight = 6.0F * scale,
              .maxWidth = 64.0F * scale,
              .fillWidth = true,
          })
      );
    }
    desktop->addChild(std::move(panel));
    return desktop;
  }

  std::unique_ptr<Button> makeMaterialTile(
      Button** out, const std::string& label, bool selected, PanelTransparencyMode mode, float scale,
      std::function<void()> onClick
  ) {
    return ui::button({
        .out = out,
        .text = std::string(label),
        .fontSize = Style::fontSizeCaption * scale,
        .selected = selected,
        .contentAlign = ButtonContentAlign::Center,
        .customPalette = materialTilePalette(),
        .tooltip = label,
        .minHeight = Style::controlHeightLg * 3.05F * scale,
        .padding = Style::spaceXs * scale,
        .gap = Style::spaceXs * scale,
        .radius = Style::scaledRadiusXl(scale),
        .flexGrow = 1.0F,
        .onClick = std::move(onClick),
        .configure =
            [mode, scale](Button& button) {
              button.setDirection(FlexDirection::Vertical);
              button.setAlign(FlexAlign::Stretch);
              button.setJustify(FlexJustify::Start);
              button.setClipChildren(true);
              if (button.label() != nullptr) {
                button.label()->setMaxLines(1);
                button.label()->setTextAlign(TextAlign::Center);
              }
              insertBeforeLabel(
                  button, makeMaterialPreview(mode, scale, Style::controlHeightLg * 2.05F * scale, nullptr)
              );
            },
    });
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
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) { applySectionCardStyle(card, scale, opacity); },
  });
  auto modeRow = ui::row({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });
  modeRow->addChild(makeMaterialTile(
      &m_modeSolid, i18n::tr("settings.options.shell.panel-transparency.solid"),
      current == PanelTransparencyMode::Solid, PanelTransparencyMode::Solid, scale,
      [this]() { applyTransparencyMode(PanelTransparencyMode::Solid); }
  ));
  modeRow->addChild(makeMaterialTile(
      &m_modeSoft, i18n::tr("settings.options.shell.panel-transparency.soft"), current == PanelTransparencyMode::Soft,
      PanelTransparencyMode::Soft, scale, [this]() { applyTransparencyMode(PanelTransparencyMode::Soft); }
  ));
  modeRow->addChild(makeMaterialTile(
      &m_modeGlass, i18n::tr("settings.options.shell.panel-transparency.glass"),
      current == PanelTransparencyMode::Glass, PanelTransparencyMode::Glass, scale,
      [this]() { applyTransparencyMode(PanelTransparencyMode::Glass); }
  ));
  compactCard->addChild(std::move(modeRow));
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

  auto leftover = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillHeight = true,
      .flexGrow = 1.0F,
  });
  leftover->addChild(makeMaterialPreview(current, scale, Style::controlHeightLg * 4.2F * scale, &m_specimen));
  tab->addChild(std::move(leftover));

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
  m_modeSolid = nullptr;
  m_modeSoft = nullptr;
  m_modeGlass = nullptr;
  m_caption = nullptr;
  m_specimen = nullptr;
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

void GlassTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  const PanelTransparencyMode mode = m_config->config().shell.panel.transparencyMode;
  m_syncing = true;
  if (m_modeSolid != nullptr) {
    m_modeSolid->setSelected(mode == PanelTransparencyMode::Solid);
  }
  if (m_modeSoft != nullptr) {
    m_modeSoft->setSelected(mode == PanelTransparencyMode::Soft);
  }
  if (m_modeGlass != nullptr) {
    m_modeGlass->setSelected(mode == PanelTransparencyMode::Glass);
  }
  if (m_caption != nullptr) {
    m_caption->setText(i18n::tr(captionKey(mode)));
  }
  if (m_specimen != nullptr) {
    m_specimen->setFill(colorSpecFromRole(ColorRole::Surface, previewCardOpacity(mode)));
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
