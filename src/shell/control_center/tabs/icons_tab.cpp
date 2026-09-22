#include "shell/control_center/tabs/icons_tab.h"

#include "config/color_spec.h"
#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "ui/app_icon_colorization.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/dialogs/color_picker_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <functional>
#include <string>
#include <utility>

using namespace control_center;

namespace {

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

  struct TintSwatch {
    ColorRole role;
    const char* labelKey;
  };

  constexpr std::array<TintSwatch, 5> kTintSwatches{{
      {ColorRole::Primary, "control-center.appearance.primary"},
      {ColorRole::Secondary, "control-center.appearance.secondary"},
      {ColorRole::Tertiary, "control-center.appearance.tertiary"},
      {ColorRole::Error, "control-center.appearance.error"},
      {ColorRole::OnSurface, "control-center.appearance.ink"},
  }};

  constexpr Color kDefaultIconColors[] = {rgbHex(0xFF3B30), rgbHex(0x34C759), rgbHex(0x007AFF)};

  Button::ButtonPalette roleSwatchPalette(ColorRole role) {
    Button::ButtonStateColors rest{
        .bg = colorSpecFromRole(role),
        .border = colorSpecFromRole(ColorRole::Outline, 0.45F),
        .label = colorSpecFromRole(ColorRole::OnSurface),
    };
    Button::ButtonStateColors hover = rest;
    hover.border = colorSpecFromRole(ColorRole::Primary);
    Button::ButtonStateColors selected = hover;
    selected.border = colorSpecFromRole(ColorRole::Primary);
    return {
        .borderWidth = 2.0F,
        .normal = rest,
        .hover = hover,
        .pressed = hover,
        .disabled = rest,
        .selected = selected,
    };
  }

  std::unique_ptr<Flex> makeIconPreview(const ColorSpec fills[3], Box** out[3], float scale, float minHeight) {
    const float size = Style::controlHeight * 0.72F * scale;
    const float radius = Style::scaledRadiusMd(scale);
    auto row = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .gap = Style::spaceXs * scale,
        .padding = Style::spaceXs * scale,
        .minHeight = minHeight,
        .configure = [](Flex& host) { host.setHitTestVisible(false); },
    });
    for (std::size_t i = 0; i < 3; ++i) {
      row->addChild(
          ui::box({
              .out = out != nullptr ? out[i] : nullptr,
              .fill = fills[i],
              .radius = radius,
              .width = size,
              .height = size,
          })
      );
    }
    return row;
  }

} // namespace

IconsTab::IconsTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "icons", .showConfigureButton = false}
      )) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> IconsTab::create() {
  return m_settings->wrapLive(createLiveBody(), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> IconsTab::createLiveBody(bool hosted) {
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;
  const bool tinted = m_config != nullptr && m_config->config().shell.appIconColorize;

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
  toolbar->addChild(
      ui::segmented({
          .out = &m_styleSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = "",
                      .glyph = "apps",
                      .tooltip = i18n::tr("control-center.appearance.icons-default"),
                  },
                  {
                      .label = "",
                      .glyph = "palette",
                      .tooltip = i18n::tr("control-center.appearance.icons-tinted"),
                  },
              },
          .selectedIndex = tinted ? std::size_t{1} : std::size_t{0},
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = false,
          .onChange = [this](std::size_t index) { applyIconStyle(index == 1); },
      })
  );
  const ColorSpec defaultFills[3] = {
      fixedColorSpec(kDefaultIconColors[0]),
      fixedColorSpec(kDefaultIconColors[1]),
      fixedColorSpec(kDefaultIconColors[2]),
  };
  ColorSpec tintFill = colorSpecFromRole(ColorRole::Primary);
  if (m_config != nullptr) {
    ShellConfig colorizeShell = m_config->config().shell;
    colorizeShell.appIconColorize = true;
    if (const auto tint = effectiveShellAppIconColorizationTint(colorizeShell)) {
      tintFill = *tint;
    }
  }
  const ColorSpec previewFills[3] = {
      tinted ? tintFill : defaultFills[0],
      tinted ? tintFill : defaultFills[1],
      tinted ? tintFill : defaultFills[2],
  };
  Box** tintedOut[3] = {&m_tintedPreview[0], &m_tintedPreview[1], &m_tintedPreview[2]};
  auto current = makeIconPreview(previewFills, tintedOut, scale, Style::controlHeightSm * 1.35F * scale);
  toolbar->addChild(std::move(current));
  compactCard->addChild(std::move(toolbar));
  compactCard->addChild(
      ui::label({
          .out = &m_caption,
          .text = i18n::tr(
              tinted ? "control-center.appearance.icons-tinted-caption"
                     : "control-center.appearance.icons-default-caption"
          ),
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
  auto tintRow = ui::row({
      .out = &m_tintRow,
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceSm * scale,
      .visible = tinted,
      .participatesInLayout = tinted,
  });
  m_tintButtons.clear();
  m_tintButtons.reserve(kTintSwatches.size());
  const float swatchSize = Style::controlHeight * 1.15F * scale;
  for (const auto& swatch : kTintSwatches) {
    const std::string roleLabel = i18n::tr(swatch.labelKey);
    Button* button = nullptr;
    tintRow->addChild(
        ui::button({
            .out = &button,
            .customPalette = roleSwatchPalette(swatch.role),
            .tooltip = roleLabel,
            .minWidth = swatchSize,
            .minHeight = swatchSize,
            .maxWidth = swatchSize,
            .maxHeight = swatchSize,
            .padding = 0.0F,
            .radius = swatchSize * 0.5F,
            .width = swatchSize,
            .height = swatchSize,
            .onClick = [this, role = swatch.role]() { applyTintRole(role); },
        })
    );
    m_tintButtons.push_back(button);
  }
  tintRow->addChild(
      ui::button({
          .out = &m_customTint,
          .glyph = "palette",
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.appearance.custom-tint-tooltip"),
          .minWidth = swatchSize,
          .minHeight = swatchSize,
          .maxWidth = swatchSize,
          .maxHeight = swatchSize,
          .padding = Style::spaceXs * scale,
          .radius = swatchSize * 0.5F,
          .onClick = [this]() { pickCustomTint(); },
      })
  );
  leftover->addChild(std::move(tintRow));
  tab->addChild(std::move(leftover));

  syncLiveControls();
  return tab;
}

std::unique_ptr<Flex> IconsTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void IconsTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_styleSwitch = nullptr;
  m_caption = nullptr;
  m_tintRow = nullptr;
  m_customTint = nullptr;
  m_tintButtons.clear();
  m_tintedPreview = {};
}

void IconsTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool IconsTab::configureMode() const { return m_settings->configureMode(); }

void IconsTab::setSettingsSearchQuery(std::string_view query) {
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* IconsTab::settingsPane() { return m_settings.get(); }

void IconsTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void IconsTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
}

void IconsTab::onPanelCardOpacityChanged(float opacity) {
  if (m_styleSwitch != nullptr) {
    m_styleSwitch->setSurfaceOpacity(opacity);
  }
}

void IconsTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  const ShellConfig& shell = m_config->config().shell;
  const bool tinted = shell.appIconColorize;
  m_syncing = true;
  if (m_styleSwitch != nullptr) {
    m_styleSwitch->setSelectedIndex(tinted ? 1 : 0);
  }
  if (m_caption != nullptr) {
    m_caption->setText(i18n::tr(
        tinted ? "control-center.appearance.icons-tinted-caption" : "control-center.appearance.icons-default-caption"
    ));
  }

  ShellConfig colorizeShell = shell;
  colorizeShell.appIconColorize = true;
  const std::optional<ColorSpec> tint = effectiveShellAppIconColorizationTint(colorizeShell);
  const ColorSpec previewFill = tint.value_or(colorSpecFromRole(ColorRole::Primary));
  const auto applyPreviewFill = [&](const std::array<Box*, 3>& boxes) {
    if (tinted) {
      for (Box* box : boxes) {
        if (box != nullptr) {
          box->setFill(previewFill);
        }
      }
      return;
    }
    for (std::size_t i = 0; i < boxes.size(); ++i) {
      if (boxes[i] != nullptr) {
        boxes[i]->setFill(fixedColorSpec(kDefaultIconColors[i]));
      }
    }
  };
  applyPreviewFill(m_tintedPreview);
  (void)setShown(m_tintRow, tinted);

  const std::string selectedValue = tint.has_value() ? colorSpecToConfigString(*tint) : std::string{};
  bool matchedRole = false;
  for (std::size_t i = 0; i < m_tintButtons.size() && i < kTintSwatches.size(); ++i) {
    const bool selected = selectedValue == colorRoleToken(kTintSwatches[i].role);
    matchedRole = matchedRole || selected;
    if (m_tintButtons[i] != nullptr) {
      m_tintButtons[i]->setSelected(selected);
    }
  }
  if (m_customTint != nullptr) {
    m_customTint->setSelected(tinted && !matchedRole && tint.has_value());
  }
  m_syncing = false;
}

void IconsTab::applyIconStyle(bool tinted) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "app_icon_colorize"}, tinted);
  syncLiveControls();
  PanelManager::instance().requestLayout();
}

void IconsTab::applyTintRole(ColorRole role) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "app_icon_colorize"}, true);
  (void)m_config->setOverride({"shell", "app_icon_color"}, std::string(colorRoleToken(role)));
  syncLiveControls();
  PanelManager::instance().requestLayout();
}

void IconsTab::pickCustomTint() {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  ShellConfig colorizeShell = m_config->config().shell;
  colorizeShell.appIconColorize = true;
  ColorPickerDialogOptions options;
  options.title = i18n::tr("control-center.appearance.custom-tint");
  if (const auto tint = effectiveShellAppIconColorizationTint(colorizeShell)) {
    options.initialColor = resolveColorSpec(*tint);
  }
  (void)ColorPickerDialog::open(std::move(options), [this](std::optional<Color> result) {
    if (!result.has_value() || m_config == nullptr) {
      return;
    }
    Color rgb = *result;
    rgb.a = 1.0F;
    (void)m_config->setOverride({"shell", "app_icon_colorize"}, true);
    (void)m_config->setOverride({"shell", "app_icon_color"}, colorSpecToConfigString(fixedColorSpec(rgb)));
    syncLiveControls();
    PanelManager::instance().requestLayout();
  });
}
