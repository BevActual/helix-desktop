#include "shell/control_center/tabs/theme_tab.h"

#include "compositors/niri/niri_layout_theme.h"
#include "config/config_service.h"
#include "core/process/process.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "theme/builtin_palettes.h"
#include "theme/community_palettes.h"
#include "theme/custom_palettes.h"
#include "theme/theme_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/color_swatch_preview.h"
#include "ui/controls/flex.h"
#include "ui/controls/grid_view.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/dialogs/color_picker_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  struct PaletteSwatch {
    ColorRole role;
    const char* labelKey;
  };

  constexpr std::array<PaletteSwatch, 6> kPaletteSwatches{{
      {ColorRole::Primary, "control-center.appearance.primary"},
      {ColorRole::Secondary, "control-center.appearance.secondary"},
      {ColorRole::Tertiary, "control-center.appearance.tertiary"},
      {ColorRole::Error, "control-center.appearance.error"},
      {ColorRole::Surface, "control-center.appearance.surface"},
      {ColorRole::Outline, "control-center.appearance.outline"},
  }};

  constexpr std::array<const char*, 10> kWallpaperSchemeLabelKeys = {
      "theme.scheme.m3-content",     "theme.scheme.m3-tonal-spot", "theme.scheme.m3-fruit-salad",
      "theme.scheme.m3-rainbow",     "theme.scheme.m3-monochrome", "theme.scheme.vibrant",
      "theme.scheme.faithful",       "theme.scheme.soft",          "theme.scheme.dysfunctional",
      "theme.scheme.muted",
  };

  constexpr std::array<const char*, 10> kWallpaperSchemeValues = {
      "m3-content", "m3-tonal-spot", "m3-fruit-salad", "m3-rainbow",    "m3-monochrome",
      "vibrant",    "faithful",      "soft",           "dysfunctional", "muted",
  };

  Button::ButtonPalette roleSwatchPalette(ColorRole role) {
    Button::ButtonStateColors rest{
        .bg = colorSpecFromRole(role),
        .border = colorSpecFromRole(ColorRole::Outline, 0.45F),
        .label = colorSpecFromRole(ColorRole::OnSurface),
    };
    Button::ButtonStateColors hover = rest;
    hover.border = colorSpecFromRole(ColorRole::Primary);
    return {
        .borderWidth = 1.0F,
        .normal = rest,
        .hover = hover,
        .pressed = hover,
        .disabled = rest,
    };
  }

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

  [[nodiscard]] std::size_t themeModeIndex(ThemeMode mode) {
    switch (mode) {
    case ThemeMode::Dark:
      return 0;
    case ThemeMode::Light:
      return 1;
    case ThemeMode::Auto:
    default:
      return 2;
    }
  }

  [[nodiscard]] ThemeMode themeModeFromIndex(std::size_t index) {
    switch (index) {
    case 0:
      return ThemeMode::Dark;
    case 1:
      return ThemeMode::Light;
    default:
      return ThemeMode::Auto;
    }
  }

  [[nodiscard]] std::size_t paletteSourceIndex(PaletteSource source) {
    switch (source) {
    case PaletteSource::Builtin:
      return 0;
    case PaletteSource::Wallpaper:
      return 1;
    case PaletteSource::Community:
      return 2;
    case PaletteSource::Custom:
    default:
      return 3;
    }
  }

  [[nodiscard]] PaletteSource paletteSourceFromIndex(std::size_t index) {
    switch (index) {
    case 1:
      return PaletteSource::Wallpaper;
    case 2:
      return PaletteSource::Community;
    case 3:
      return PaletteSource::Custom;
    default:
      return PaletteSource::Builtin;
    }
  }

  [[nodiscard]] ColorSwatchPreview previewFromPalette(const Palette& palette) {
    return ColorSwatchPreview{
        .surface = fixedColorSpec(palette.surface),
        .swatches =
            {
                fixedColorSpec(palette.primary),
                fixedColorSpec(palette.secondary),
                fixedColorSpec(palette.tertiary),
                fixedColorSpec(palette.error),
            },
    };
  }

  [[nodiscard]] ColorSwatchPreview previewFromMetadata(const noctalia::theme::AvailablePalette::PreviewMode& metadata) {
    ColorSwatchPreview preview;
    Color surface;
    if (tryParseHexColor(metadata.surface, surface)) {
      preview.surface = fixedColorSpec(surface);
    }
    preview.swatches.reserve(metadata.accents.size());
    for (const auto& hexColor : metadata.accents) {
      Color color;
      if (tryParseHexColor(hexColor, color)) {
        preview.swatches.push_back(fixedColorSpec(color));
      }
    }
    return preview;
  }

  [[nodiscard]] ColorSwatchPreview availablePalettePreview(
      const noctalia::theme::AvailablePalette& palette, ThemeMode mode
  ) {
    if (mode == ThemeMode::Light) {
      ColorSwatchPreview preview = previewFromMetadata(palette.preview.light);
      if (!preview.empty()) {
        return preview;
      }
      return previewFromMetadata(palette.preview.dark);
    }
    return previewFromMetadata(palette.preview.dark);
  }

  struct PaletteCatalog {
    std::vector<std::string> values;
    std::vector<std::string> labels;
    std::vector<ColorSwatchPreview> previews;
  };

  [[nodiscard]] PaletteCatalog buildPaletteCatalog(PaletteSource source, ThemeMode previewMode) {
    PaletteCatalog catalog;
    switch (source) {
    case PaletteSource::Builtin:
      for (const auto& builtin : noctalia::theme::builtinPalettes()) {
        catalog.values.emplace_back(builtin.name);
        catalog.labels.emplace_back(builtin.name);
        catalog.previews.push_back(
            previewFromPalette(previewMode == ThemeMode::Light ? builtin.light.palette : builtin.dark.palette)
        );
      }
      break;
    case PaletteSource::Wallpaper:
      for (std::size_t i = 0; i < kWallpaperSchemeValues.size(); ++i) {
        catalog.values.emplace_back(kWallpaperSchemeValues[i]);
        catalog.labels.emplace_back(i18n::tr(kWallpaperSchemeLabelKeys[i]));
      }
      break;
    case PaletteSource::Community:
      for (const auto& entry : noctalia::theme::availableCommunityPalettes()) {
        catalog.values.push_back(entry.name);
        catalog.labels.push_back(entry.name);
        catalog.previews.push_back(availablePalettePreview(entry, previewMode));
      }
      break;
    case PaletteSource::Custom:
      for (const auto& entry : noctalia::theme::availableCustomPalettes()) {
        catalog.values.push_back(entry.name);
        catalog.labels.push_back(entry.name);
        catalog.previews.push_back(availablePalettePreview(entry, previewMode));
      }
      break;
    }
    return catalog;
  }

  [[nodiscard]] PaletteCatalog filterPaletteCatalog(const PaletteCatalog& catalog, std::string_view query) {
    if (query.empty()) {
      return catalog;
    }
    const std::string needle = StringUtils::toLower(query);
    PaletteCatalog filtered;
    for (std::size_t i = 0; i < catalog.values.size(); ++i) {
      const std::string valueLower = StringUtils::toLower(catalog.values[i]);
      const std::string labelLower = i < catalog.labels.size() ? StringUtils::toLower(catalog.labels[i]) : valueLower;
      if (valueLower.find(needle) == std::string::npos && labelLower.find(needle) == std::string::npos) {
        continue;
      }
      filtered.values.push_back(catalog.values[i]);
      filtered.labels.push_back(i < catalog.labels.size() ? catalog.labels[i] : catalog.values[i]);
      if (i < catalog.previews.size()) {
        filtered.previews.push_back(catalog.previews[i]);
      }
    }
    return filtered;
  }

  [[nodiscard]] ColorSwatchPreview currentRolePreview() {
    return ColorSwatchPreview{
        .surface = colorSpecFromRole(ColorRole::Surface),
        .swatches =
            {
                colorSpecFromRole(ColorRole::Primary),
                colorSpecFromRole(ColorRole::Secondary),
                colorSpecFromRole(ColorRole::Tertiary),
                colorSpecFromRole(ColorRole::Error),
            },
    };
  }

  constexpr Color kLightDesktop = rgbHex(0xF5F5F7);
  constexpr Color kDarkDesktop = rgbHex(0x1C1C1E);
  constexpr Color kLightWindow = rgbHex(0xFFFFFF);
  constexpr Color kDarkWindow = rgbHex(0x2C2C2E);
  constexpr Color kLightTitlebar = rgbHex(0xE8E8ED);
  constexpr Color kDarkTitlebar = rgbHex(0x3A3A3C);
  constexpr Color kLightRule = rgbHex(0xD1D1D6);
  constexpr Color kDarkRule = rgbHex(0x636366);
  constexpr Color kTrafficClose = rgbHex(0xFF5F57);
  constexpr Color kTrafficMin = rgbHex(0xFEBC2E);
  constexpr Color kTrafficMax = rgbHex(0x28C840);

  Button::ButtonPalette appearanceTilePalette(bool light, bool autoTile) {
    const ColorSpec fill = autoTile ? colorSpecFromRole(ColorRole::SurfaceVariant)
                                    : (light ? fixedColorSpec(kLightDesktop) : fixedColorSpec(kDarkDesktop));
    const ColorSpec label = autoTile ? colorSpecFromRole(ColorRole::OnSurface)
                                     : (light ? fixedColorSpec(kDarkDesktop) : fixedColorSpec(kLightDesktop));
    const ColorSpec restBorder = autoTile ? colorSpecFromRole(ColorRole::Outline, 0.4F)
                                          : (light ? fixedColorSpec(Color{0.0F, 0.0F, 0.0F, 0.10F})
                                                   : fixedColorSpec(Color{1.0F, 1.0F, 1.0F, 0.14F}));
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

  void openControlCenterTab(std::string_view tab) {
    PanelManager::instance().togglePanel("control-center", PanelOpenRequest{.context = tab});
  }

  struct WindowPreviewStyle {
    ColorSpec desktop;
    ColorSpec window;
    ColorSpec titlebar;
    ColorSpec border;
    ColorSpec accent;
    ColorSpec rule;
  };

  WindowPreviewStyle appearanceWindowStyle(bool light) {
    return {
        .desktop = light ? fixedColorSpec(kLightDesktop) : fixedColorSpec(kDarkDesktop),
        .window = light ? fixedColorSpec(kLightWindow) : fixedColorSpec(kDarkWindow),
        .titlebar = light ? fixedColorSpec(kLightTitlebar) : fixedColorSpec(kDarkTitlebar),
        .border = light ? fixedColorSpec(Color{0.0F, 0.0F, 0.0F, 0.10F})
                        : fixedColorSpec(Color{1.0F, 1.0F, 1.0F, 0.12F}),
        .accent = colorSpecFromRole(ColorRole::Primary),
        .rule = light ? fixedColorSpec(kLightRule) : fixedColorSpec(kDarkRule),
    };
  }

  WindowPreviewStyle styleFromPreview(const ColorSwatchPreview& preview) {
    const ColorSpec surface = preview.surface.value_or(colorSpecFromRole(ColorRole::Surface));
    const ColorSpec accent =
        preview.swatches.empty() ? colorSpecFromRole(ColorRole::Primary) : preview.swatches.front();
    const ColorSpec rule =
        preview.swatches.size() > 1 ? preview.swatches[1] : colorSpecFromRole(ColorRole::Outline, 0.45F);
    const ColorSpec titlebar =
        preview.swatches.size() > 2 ? preview.swatches[2] : colorSpecFromRole(ColorRole::SurfaceVariant);
    return {
        .desktop = surface,
        .window = surface,
        .titlebar = titlebar,
        .border = colorSpecFromRole(ColorRole::Outline, 0.32F),
        .accent = accent,
        .rule = rule,
    };
  }

  std::unique_ptr<Box> makeTrafficDot(const Color& fill, float size) {
    return ui::box({
        .fill = fixedColorSpec(fill),
        .radius = size * 0.5F,
        .width = size,
        .height = size,
    });
  }

  std::unique_ptr<Flex> makeMiniWindow(const WindowPreviewStyle& style, float scale, bool compact) {
    const float titleHeight = (compact ? 11.0F : 14.0F) * scale;
    const float dot = (compact ? 4.5F : 6.0F) * scale;
    const float radius = Style::scaledRadiusMd(scale);
    const float ruleHeight = (compact ? 3.0F : 4.0F) * scale;
    auto window = ui::column({
        .align = FlexAlign::Stretch,
        .fill = style.window,
        .radius = radius,
        .border = style.border,
        .borderWidth = 1.0F,
        .minHeight = (compact ? 34.0F : 48.0F) * scale,
        .fillWidth = true,
        .clipChildren = true,
        .flexGrow = 1.0F,
    });
    auto titlebar = ui::row({
        .align = FlexAlign::Center,
        .gap = std::max(2.0F, 3.0F * scale),
        .paddingV = std::max(2.0F, 3.0F * scale),
        .paddingH = std::max(4.0F, 5.0F * scale),
        .fill = style.titlebar,
        .minHeight = titleHeight,
        .fillWidth = true,
    });
    titlebar->addChild(makeTrafficDot(kTrafficClose, dot));
    titlebar->addChild(makeTrafficDot(kTrafficMin, dot));
    titlebar->addChild(makeTrafficDot(kTrafficMax, dot));
    auto body = ui::column({
        .align = FlexAlign::Stretch,
        .gap = std::max(2.0F, 3.0F * scale),
        .padding = std::max(4.0F, 5.0F * scale),
        .fillWidth = true,
        .flexGrow = 1.0F,
    });
    body->addChild(
        ui::row({
            .fill = style.accent,
            .radius = 2.0F * scale,
            .minHeight = (compact ? 5.0F : 6.0F) * scale,
            .fillWidth = true,
        })
    );
    body->addChild(
        ui::row({
            .fill = style.rule,
            .radius = 2.0F * scale,
            .minHeight = ruleHeight,
            .maxWidth = 48.0F * scale,
            .fillWidth = true,
        })
    );
    window->addChild(std::move(titlebar));
    window->addChild(std::move(body));
    return window;
  }

  std::unique_ptr<Flex> makeDesktopPreview(const WindowPreviewStyle& style, float scale, bool compact, float height) {
    const float pad = Style::spaceXs * scale;
    auto desktop = ui::column({
        .align = FlexAlign::Stretch,
        .justify = FlexJustify::Center,
        .padding = pad,
        .fill = style.desktop,
        .minHeight = height,
        .fillWidth = true,
        .flexGrow = 1.0F,
        .configure = [](Flex& host) { host.setHitTestVisible(false); },
    });
    desktop->addChild(makeMiniWindow(style, scale, compact));
    return desktop;
  }

  std::unique_ptr<Flex> makeAppearancePreview(bool light, bool autoTile, float scale) {
    const float pad = Style::spaceXs * scale;
    const float previewHeight = Style::controlHeightLg * 2.05F * scale;
    const float radius = Style::scaledRadiusLg(scale);
    if (autoTile) {
      auto split = ui::row({
          .align = FlexAlign::Stretch,
          .gap = 0.0F,
          .radius = radius,
          .minHeight = previewHeight,
          .fillWidth = true,
          .clipChildren = true,
          .flexGrow = 1.0F,
          .configure = [](Flex& host) { host.setHitTestVisible(false); },
      });
      auto left = ui::column({
          .align = FlexAlign::Stretch,
          .padding = pad,
          .fill = fixedColorSpec(kLightDesktop),
          .minHeight = previewHeight,
          .fillHeight = true,
          .flexGrow = 1.0F,
          .configure = [radius](Flex& pane) { pane.setRadii(Radii{radius, 0.0F, 0.0F, radius}); },
      });
      left->addChild(makeMiniWindow(appearanceWindowStyle(true), scale, true));
      auto right = ui::column({
          .align = FlexAlign::Stretch,
          .padding = pad,
          .fill = fixedColorSpec(kDarkDesktop),
          .minHeight = previewHeight,
          .fillHeight = true,
          .flexGrow = 1.0F,
          .configure = [radius](Flex& pane) { pane.setRadii(Radii{0.0F, radius, radius, 0.0F}); },
      });
      right->addChild(makeMiniWindow(appearanceWindowStyle(false), scale, true));
      split->addChild(std::move(left));
      split->addChild(std::move(right));
      return split;
    }

    return makeDesktopPreview(appearanceWindowStyle(light), scale, false, previewHeight);
  }

  std::unique_ptr<Flex> makeCatalogPreview(
      const ColorSwatchPreview& preview, bool wallpaperFallback, float scale, float wellHeight
  ) {
    if (!preview.empty()) {
      return makeDesktopPreview(styleFromPreview(preview), scale, true, wellHeight);
    }
    if (!wallpaperFallback) {
      return nullptr;
    }
    auto well = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .padding = Style::spaceXs * scale,
        .fill = colorSpecFromRole(ColorRole::Surface),
        .radius = Style::scaledRadiusLg(scale),
        .minHeight = wellHeight,
        .fillWidth = true,
        .configure = [](Flex& host) { host.setHitTestVisible(false); },
    });
    well->addChild(
        ui::glyph({
            .glyph = "photo",
            .glyphSize = Style::fontSizeTitle * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    return well;
  }

  Button::ButtonPalette captionButtonPalette() {
    const ColorSpec label = colorSpecFromRole(ColorRole::OnSurfaceVariant);
    const ColorSpec hoverLabel = colorSpecFromRole(ColorRole::OnSurface);
    Button::ButtonStateColors rest{.bg = clearColorSpec(), .border = clearColorSpec(), .label = label};
    Button::ButtonStateColors hover{
        .bg = colorSpecFromRole(ColorRole::Hover, 0.35F), .border = clearColorSpec(), .label = hoverLabel
    };
    return {
        .borderWidth = 0.0F,
        .normal = rest,
        .hover = hover,
        .pressed = hover,
        .disabled = rest,
    };
  }

  Button::ButtonPalette catalogTilePalette() {
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

  [[nodiscard]] std::string colorHex(const Color& color) {
    const auto channel = [](float v) { return static_cast<int>(std::clamp(v, 0.0F, 1.0F) * 255.0F); };
    return std::format("#{:02x}{:02x}{:02x}", channel(color.r), channel(color.g), channel(color.b));
  }

  std::unique_ptr<Button> makeAppearanceTile(
      Button** out, const std::string& label, bool selected, bool light, bool autoTile, float scale,
      std::function<void()> onClick
  ) {
    return ui::button({
        .out = out,
        .text = std::string(label),
        .fontSize = Style::fontSizeCaption * scale,
        .selected = selected,
        .contentAlign = ButtonContentAlign::Center,
        .customPalette = appearanceTilePalette(light, autoTile),
        .tooltip = label,
        .minHeight = Style::controlHeightLg * 3.05F * scale,
        .padding = Style::spaceXs * scale,
        .gap = Style::spaceXs * scale,
        .radius = Style::scaledRadiusXl(scale),
        .flexGrow = 1.0F,
        .onClick = std::move(onClick),
        .configure =
            [light, autoTile, scale](Button& button) {
              button.setDirection(FlexDirection::Vertical);
              button.setAlign(FlexAlign::Stretch);
              button.setJustify(FlexJustify::Start);
              button.setClipChildren(true);
              if (button.label() != nullptr) {
                button.label()->setMaxLines(1);
                button.label()->setTextAlign(TextAlign::Center);
              }
              insertBeforeLabel(button, makeAppearancePreview(light, autoTile, scale));
            },
    });
  }

} // namespace

ThemeTab::Section ThemeTab::sectionFromIndex(std::size_t index) noexcept {
  switch (index) {
  case 1:
    return Section::Glass;
  case 2:
    return Section::Icons;
  case 3:
    return Section::Type;
  case 4:
    return Section::Scale;
  case 5:
    return Section::Roundness;
  case 6:
    return Section::Effects;
  case 7:
    return Section::Language;
  case 0:
  default:
    return Section::Palettes;
  }
}

std::size_t ThemeTab::indexFromSection(Section section) noexcept {
  switch (section) {
  case Section::Glass:
    return 1;
  case Section::Icons:
    return 2;
  case Section::Type:
    return 3;
  case Section::Scale:
    return 4;
  case Section::Roundness:
    return 5;
  case Section::Effects:
    return 6;
  case Section::Language:
    return 7;
  case Section::Palettes:
  default:
    return 0;
  }
}

std::string_view ThemeTab::idFromSection(Section section) noexcept {
  switch (section) {
  case Section::Glass:
    return "glass";
  case Section::Icons:
    return "icons";
  case Section::Type:
    return "type";
  case Section::Scale:
    return "scale";
  case Section::Roundness:
    return "roundness";
  case Section::Effects:
    return "effects";
  case Section::Language:
    return "language";
  case Section::Palettes:
  default:
    return "palettes";
  }
}

ThemeTab::Section ThemeTab::sectionFromId(std::string_view id) noexcept {
  if (id == "glass") {
    return Section::Glass;
  }
  if (id == "icons") {
    return Section::Icons;
  }
  if (id == "type") {
    return Section::Type;
  }
  if (id == "scale") {
    return Section::Scale;
  }
  if (id == "roundness") {
    return Section::Roundness;
  }
  if (id == "effects") {
    return Section::Effects;
  }
  if (id == "language") {
    return Section::Language;
  }
  return Section::Palettes;
}

ThemeTab::ThemeTab(const ControlCenterServices& services)
    : m_config(services.config), m_theme(services.theme),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "theme", .includedGroups = {"theme", "niri"}}
      )),
      m_glass(std::make_unique<GlassTab>(services)),
      m_icons(std::make_unique<IconsTab>(services)),
      m_type(std::make_unique<TypeTab>(services)),
      m_scale(std::make_unique<ScaleTab>(services)),
      m_roundness(std::make_unique<RoundnessTab>(services)),
      m_effects(std::make_unique<EffectsTab>(services)),
      m_language(std::make_unique<LanguageTab>(services)) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
  if (m_config != nullptr) {
    if (const std::optional<std::string> section = m_config->stateString("theme_tab", "section")) {
      m_section = sectionFromId(*section);
    }
  }
  syncSettingsSection();
}

std::unique_ptr<Flex> ThemeTab::create() {
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;
  const ThemeMode currentMode = m_config != nullptr ? m_config->config().theme.mode : ThemeMode::Auto;
  const std::size_t modeIndex = themeModeIndex(currentMode);
  const PaletteSource currentSource =
      m_config != nullptr ? m_config->config().theme.source : PaletteSource::Builtin;
  const float opacity = panelCardOpacity();
  syncChildChrome();

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto compactCard = ui::column({
      .configure =
          [scale, opacity](Flex& card) {
            applySectionCardStyle(card, scale, opacity);
            card.setGap(Style::spaceXs * scale);
            card.setPadding(Style::spaceSm * scale);
          },
  });
  compactCard->addChild(
      ui::segmented({
          .out = &m_sectionSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = "",
                      .glyph = "palette",
                      .tooltip = i18n::tr("settings.navigation.sections.theme"),
                  },
                  {
                      .label = "",
                      .glyph = "glass",
                      .tooltip = i18n::tr("settings.navigation.sections.glass"),
                  },
                  {
                      .label = "",
                      .glyph = "apps",
                      .tooltip = i18n::tr("settings.navigation.sections.icons"),
                  },
                  {
                      .label = "",
                      .glyph = "typography",
                      .tooltip = i18n::tr("settings.navigation.sections.type"),
                  },
                  {
                      .label = "",
                      .glyph = "resize",
                      .tooltip = i18n::tr("settings.navigation.sections.scale"),
                  },
                  {
                      .label = "",
                      .glyph = "border-radius",
                      .tooltip = i18n::tr("settings.navigation.sections.roundness"),
                  },
                  {
                      .label = "",
                      .glyph = "shadow",
                      .tooltip = i18n::tr("settings.navigation.sections.effects"),
                  },
                  {
                      .label = "",
                      .glyph = "language",
                      .tooltip = i18n::tr("settings.navigation.sections.language"),
                  },
              },
          .selectedIndex = indexFromSection(m_section),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = opacity,
          .equalSegmentWidths = false,
          .onChange = [this](std::size_t index) { applySection(index); },
          .configure = [](Segmented& sources) { sources.setWrap(true); },
      })
  );
  tab->addChild(std::move(compactCard));

  auto palettesHost = ui::column({
      .out = &m_palettesHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });
  auto palettesCard = ui::column({
      .out = &m_palettesCard,
      .configure =
          [scale, opacity](Flex& card) {
            applySectionCardStyle(card, scale, opacity);
            card.setGap(Style::spaceXs * scale);
            card.setPadding(Style::spaceSm * scale);
          },
  });

  auto browse = ui::row({
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceXs * scale,
      .flexGrow = 1.0F,
  });
  browse->addChild(
      ui::segmented({
          .out = &m_sourceSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = "",
                      .glyph = "palette",
                      .tooltip = i18n::tr("control-center.appearance.palette-builtin-tooltip"),
                  },
                  {
                      .label = "",
                      .glyph = "photo",
                      .tooltip = i18n::tr("control-center.appearance.palette-wallpaper-tooltip"),
                  },
                  {
                      .label = "",
                      .glyph = "users",
                      .tooltip = i18n::tr("control-center.appearance.palette-community-tooltip"),
                  },
                  {
                      .label = "",
                      .glyph = "folder",
                      .tooltip = i18n::tr("control-center.appearance.palette-custom-tooltip"),
                  },
              },
          .selectedIndex = paletteSourceIndex(currentSource),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = opacity,
          .onChange = [this](std::size_t index) { applyPaletteSource(index); },
      })
  );
  browse->addChild(
      ui::row(
          {.align = FlexAlign::Center, .flexGrow = 1.0F},
          ui::input({
              .out = &m_paletteFilter,
              .placeholder = i18n::tr("control-center.appearance.palette-search"),
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeightSm * scale,
              .clearButtonEnabled = true,
              .minLayoutWidth = 128.0F * scale,
              .surfaceOpacity = opacity,
              .height = 0.0F,
              .flexGrow = 1.0F,
              .onChange =
                  [this](const std::string& text) {
                    if (m_paletteQuery == text) {
                      return;
                    }
                    m_paletteQuery = text;
                    rebuildPaletteGrid();
                    PanelManager::instance().requestLayout();
                  },
          })
      )
  );
  browse->addChild(
      ui::button({
          .out = &m_saveCustomButton,
          .glyph = "device-floppy",
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.appearance.save-custom-tooltip"),
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [this]() { saveCurrentPalette(); },
      })
  );
  browse->addChild(
      ui::button({
          .out = &m_openWallpaperButton,
          .glyph = "photo",
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.appearance.open-wallpaper"),
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .visible = false,
          .participatesInLayout = false,
          .onClick = []() { openControlCenterTab("wallpaper"); },
      })
  );
  browse->addChild(
      ui::button({
          .out = &m_openPalettesButton,
          .glyph = "folder",
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.appearance.open-palettes-folder"),
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .visible = false,
          .participatesInLayout = false,
          .onClick = [this]() { openCustomPalettesFolder(); },
      })
  );

  auto toolbar = ui::row({
      .out = &m_catalogToolbar,
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
  });
  toolbar->addChild(std::move(browse));
  toolbar->addChild(
      ui::label({
          .out = &m_paletteName,
          .text = i18n::tr("control-center.appearance.palette"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  palettesCard->addChild(std::move(toolbar));

  auto modeRow = ui::row({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });
  modeRow->addChild(makeAppearanceTile(
      &m_modeDark, i18n::tr("settings.options.theme.mode.dark"), modeIndex == 0, false, false, scale,
      [this]() { applyThemeMode(0); }
  ));
  modeRow->addChild(makeAppearanceTile(
      &m_modeLight, i18n::tr("settings.options.theme.mode.light"), modeIndex == 1, true, false, scale,
      [this]() { applyThemeMode(1); }
  ));
  modeRow->addChild(makeAppearanceTile(
      &m_modeAuto, i18n::tr("common.states.auto"), modeIndex == 2, false, true, scale, [this]() { applyThemeMode(2); }
  ));
  palettesCard->addChild(std::move(modeRow));
  palettesCard->addChild(
      ui::button({
          .out = &m_autoStatus,
          .fontSize = captionSize,
          .contentAlign = ButtonContentAlign::Start,
          .customPalette = captionButtonPalette(),
          .tooltip = i18n::tr("control-center.appearance.auto-open-weather"),
          .paddingV = Style::spaceXs * scale,
          .paddingH = 0.0F,
          .visible = false,
          .participatesInLayout = false,
          .onClick = []() { openControlCenterTab("weather"); },
          .configure =
              [](Button& button) {
                if (button.label() != nullptr) {
                  button.label()->setMaxLines(2);
                  button.label()->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
                }
              },
      })
  );

  auto heroWell = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .gap = Style::spaceSm * scale,
      .padding = Style::spaceSm * scale,
      .fill = colorSpecFromRole(ColorRole::Surface),
      .radius = Style::scaledRadiusXl(scale),
      .minHeight = Style::controlHeightLg * 1.25F * scale,
      .fillWidth = true,
  });
  const float primarySize = Style::controlHeight * 1.45F * scale;
  const float swatchSize = Style::controlHeight * 1.05F * scale;
  for (std::size_t i = 0; i < kPaletteSwatches.size(); ++i) {
    const auto& swatch = kPaletteSwatches[i];
    const std::string roleLabel = i18n::tr(swatch.labelKey);
    const float size = i == 0 ? primarySize : swatchSize;
    heroWell->addChild(
        ui::button({
            .customPalette = roleSwatchPalette(swatch.role),
            .tooltip = i18n::tr("control-center.appearance.edit-role-tooltip", "role", roleLabel),
            .minWidth = size,
            .minHeight = size,
            .maxWidth = size,
            .maxHeight = size,
            .padding = 0.0F,
            .radius = size * 0.5F,
            .width = size,
            .height = size,
            .onClick = [this, role = swatch.role, roleLabel]() { pickRoleColor(role, roleLabel); },
        })
    );
  }
  palettesCard->addChild(std::move(heroWell));
  palettesCard->addChild(
      ui::label({
          .out = &m_exportStatus,
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  palettesHost->addChild(std::move(palettesCard));

  auto leftover = ui::column({
      .out = &m_palettesLeftover,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
  });
  leftover->addChild(
      ui::label({
          .out = &m_paletteEmpty,
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  auto listScroll = ui::scrollView({
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure =
          [](ScrollView& scrollView) {
            scrollView.clearFill();
            scrollView.clearBorder();
          },
  });
  auto* content = listScroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  auto grid = std::make_unique<GridView>();
  grid->setColumns(2);
  grid->setColumnGap(Style::spaceSm * scale);
  grid->setRowGap(Style::spaceSm * scale);
  grid->setUniformCellSize(true);
  grid->setStretchItems(true);
  grid->setSquareCells(false);
  grid->setMinCellHeight(Style::controlHeightLg * 2.45F * scale);
  m_paletteGrid = grid.get();
  content->addChild(std::move(grid));
  leftover->addChild(std::move(listScroll));
  palettesHost->addChild(std::move(leftover));
  tab->addChild(std::move(palettesHost));

  auto addHost = [&]<typename Hosted>(Flex** out, Hosted* child) {
    auto host = ui::column({
        .out = out,
        .align = FlexAlign::Stretch,
        .gap = 0.0F,
        .flexGrow = 1.0F,
    });
    host->addChild(child->createLiveBody());
    tab->addChild(std::move(host));
  };
  addHost(&m_glassHost, m_glass.get());
  addHost(&m_iconsHost, m_icons.get());
  addHost(&m_typeHost, m_type.get());
  addHost(&m_scaleHost, m_scale.get());
  addHost(&m_roundnessHost, m_roundness.get());
  addHost(&m_effectsHost, m_effects.get());
  addHost(&m_languageHost, m_language.get());

  syncHosts();
  auto wrapped = m_settings->wrapLive(std::move(tab), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
}

std::unique_ptr<Flex> ThemeTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void ThemeTab::onClose() {
  if (m_glass != nullptr) {
    m_glass->onClose();
  }
  if (m_icons != nullptr) {
    m_icons->onClose();
  }
  if (m_type != nullptr) {
    m_type->onClose();
  }
  if (m_scale != nullptr) {
    m_scale->onClose();
  }
  if (m_roundness != nullptr) {
    m_roundness->onClose();
  }
  if (m_effects != nullptr) {
    m_effects->onClose();
  }
  if (m_language != nullptr) {
    m_language->onClose();
  }
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_sectionSwitch = nullptr;
  m_palettesHost = nullptr;
  m_palettesCard = nullptr;
  m_palettesLeftover = nullptr;
  m_glassHost = nullptr;
  m_iconsHost = nullptr;
  m_typeHost = nullptr;
  m_scaleHost = nullptr;
  m_roundnessHost = nullptr;
  m_effectsHost = nullptr;
  m_languageHost = nullptr;
  m_modeDark = nullptr;
  m_modeLight = nullptr;
  m_modeAuto = nullptr;
  m_autoStatus = nullptr;
  m_paletteName = nullptr;
  m_sourceSwitch = nullptr;
  m_paletteFilter = nullptr;
  m_catalogToolbar = nullptr;
  m_paletteGrid = nullptr;
  m_paletteEmpty = nullptr;
  m_openWallpaperButton = nullptr;
  m_openPalettesButton = nullptr;
  m_saveCustomButton = nullptr;
  m_exportStatus = nullptr;
  m_lastGridKey.clear();
  m_sectionSyncing = false;
}

void ThemeTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool ThemeTab::configureMode() const { return m_settings->configureMode(); }

void ThemeTab::setSettingsSearchQuery(std::string_view query) {
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* ThemeTab::settingsPane() { return m_settings.get(); }

void ThemeTab::setActive(bool active) {
  if (m_glass != nullptr) {
    m_glass->setActive(active && m_section == Section::Glass);
  }
  if (m_icons != nullptr) {
    m_icons->setActive(active && m_section == Section::Icons);
  }
  if (m_type != nullptr) {
    m_type->setActive(active && m_section == Section::Type);
  }
  if (m_scale != nullptr) {
    m_scale->setActive(active && m_section == Section::Scale);
  }
  if (m_roundness != nullptr) {
    m_roundness->setActive(active && m_section == Section::Roundness);
  }
  if (m_effects != nullptr) {
    m_effects->setActive(active && m_section == Section::Effects);
  }
  if (m_language != nullptr) {
    m_language->setActive(active && m_section == Section::Language);
  }
}

void ThemeTab::applyOpenSection(Section section, bool persist) {
  if (m_section == section) {
    syncHosts();
    syncSettingsSection();
    return;
  }
  m_section = section;
  if (persist) {
    persistSection();
  }
  if (m_sectionSwitch != nullptr) {
    m_sectionSyncing = true;
    m_sectionSwitch->setSelectedIndex(indexFromSection(m_section));
    m_sectionSyncing = false;
  }
  syncHosts();
  syncSettingsSection();
  setActive(true);
  PanelManager::instance().requestLayout();
}

void ThemeTab::applySection(std::size_t index) {
  if (m_sectionSyncing) {
    return;
  }
  applyOpenSection(sectionFromIndex(index), true);
}

void ThemeTab::persistSection() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("theme_tab", "section", std::string(idFromSection(m_section)));
}

void ThemeTab::syncHosts() {
  const bool palettes = m_section == Section::Palettes;
  setShown(m_palettesHost, palettes);
  setShown(m_glassHost, m_section == Section::Glass);
  setShown(m_iconsHost, m_section == Section::Icons);
  setShown(m_typeHost, m_section == Section::Type);
  setShown(m_scaleHost, m_section == Section::Scale);
  setShown(m_roundnessHost, m_section == Section::Roundness);
  setShown(m_effectsHost, m_section == Section::Effects);
  setShown(m_languageHost, m_section == Section::Language);
}

void ThemeTab::syncSettingsSection() {
  if (m_settings == nullptr) {
    return;
  }
  if (m_section == Section::Palettes) {
    m_settings->setSection("theme");
    m_settings->setIncludedGroups({"theme", "niri"});
    return;
  }
  m_settings->setSection(std::string(idFromSection(m_section)));
  m_settings->setIncludedGroups({});
}

void ThemeTab::syncChildChrome() {
  const float scale = contentScale();
  const float opacity = panelCardOpacity();
  const auto apply = [scale, opacity](Tab* child) {
    if (child == nullptr) {
      return;
    }
    child->setContentScale(scale);
    child->setPanelCardOpacity(opacity);
  };
  apply(m_glass.get());
  apply(m_icons.get());
  apply(m_type.get());
  apply(m_scale.get());
  apply(m_roundness.get());
  apply(m_effects.get());
  apply(m_language.get());
}

Tab* ThemeTab::hostedTab() const noexcept {
  switch (m_section) {
  case Section::Glass:
    return m_glass.get();
  case Section::Icons:
    return m_icons.get();
  case Section::Type:
    return m_type.get();
  case Section::Scale:
    return m_scale.get();
  case Section::Roundness:
    return m_roundness.get();
  case Section::Effects:
    return m_effects.get();
  case Section::Language:
    return m_language.get();
  case Section::Palettes:
  default:
    return nullptr;
  }
}

Flex* ThemeTab::hostNode() const noexcept {
  switch (m_section) {
  case Section::Glass:
    return m_glassHost;
  case Section::Icons:
    return m_iconsHost;
  case Section::Type:
    return m_typeHost;
  case Section::Scale:
    return m_scaleHost;
  case Section::Roundness:
    return m_roundnessHost;
  case Section::Effects:
    return m_effectsHost;
  case Section::Language:
    return m_languageHost;
  case Section::Palettes:
  default:
    return m_palettesHost;
  }
}

void ThemeTab::updatePalettesLayout(float contentWidth) {
  if (m_palettesHost == nullptr) {
    return;
  }
  const float scale = contentScale();
  const bool split = m_section == Section::Palettes && contentWidth >= 640.0F * scale;
  m_palettesHost->setDirection(split ? FlexDirection::Horizontal : FlexDirection::Vertical);
  if (m_palettesCard != nullptr) {
    m_palettesCard->setMinWidth(split ? 280.0F * scale : 0.0F);
    m_palettesCard->setMaxWidth(split ? 380.0F * scale : 0.0F);
    m_palettesCard->setFillHeight(split);
  }
  if (m_palettesLeftover != nullptr) {
    m_palettesLeftover->setFlexGrow(1.0F);
    m_palettesLeftover->setFillHeight(true);
    m_palettesLeftover->setMinWidth(split ? 280.0F * scale : 0.0F);
  }
}

bool ThemeTab::updatePaletteColumns(float availableWidth) {
  if (m_paletteGrid == nullptr || availableWidth <= 0.0F) {
    return false;
  }
  const float scale = contentScale();
  const std::size_t columns =
      leftoverCatalogColumns(availableWidth, 168.0F * scale, Style::spaceSm * scale, 2, 5);
  if (m_paletteGrid->columns() == columns) {
    return false;
  }
  m_paletteGrid->setColumns(columns);
  return true;
}

void ThemeTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  updatePalettesLayout(contentWidth);
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  if (m_section == Section::Palettes) {
    const float catalogWidth = m_palettesLeftover != nullptr && m_palettesLeftover->width() > 1.0F
        ? m_palettesLeftover->width()
        : contentWidth;
    if (updatePaletteColumns(catalogWidth)) {
      m_rootLayout->layout(renderer);
    }
  }
  if (Tab* child = hostedTab(); child != nullptr) {
    Flex* host = hostNode();
    const float hostWidth = host != nullptr && host->width() > 1.0F ? host->width() : contentWidth;
    const float hostHeight = host != nullptr && host->height() > 1.0F ? host->height() : bodyHeight;
    child->layout(renderer, hostWidth, hostHeight);
  }
}

void ThemeTab::doUpdate(Renderer& renderer) {
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (Tab* child = hostedTab(); child != nullptr) {
    child->update(renderer);
    return;
  }
  (void)renderer;
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  } else {
    updateAutoStatus();
  }
}

void ThemeTab::onPanelCardOpacityChanged(float opacity) {
  if (m_sectionSwitch != nullptr) {
    m_sectionSwitch->setSurfaceOpacity(opacity);
  }
  if (m_sourceSwitch != nullptr) {
    m_sourceSwitch->setSurfaceOpacity(opacity);
  }
  syncChildChrome();
}

void ThemeTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }

  const ThemeConfig& theme = m_config->config().theme;
  m_syncing = true;
  const std::size_t modeIndex = themeModeIndex(theme.mode);
  if (m_modeDark != nullptr) {
    m_modeDark->setSelected(modeIndex == 0);
  }
  if (m_modeLight != nullptr) {
    m_modeLight->setSelected(modeIndex == 1);
  }
  if (m_modeAuto != nullptr) {
    m_modeAuto->setSelected(modeIndex == 2);
  }
  if (m_sourceSwitch != nullptr) {
    m_sourceSwitch->setSelectedIndex(paletteSourceIndex(theme.source));
  }
  rebuildPaletteGrid();
  updateAutoStatus();
  setShown(m_openPalettesButton, theme.source == PaletteSource::Custom);
  setShown(m_openWallpaperButton, theme.source == PaletteSource::Wallpaper);
  m_syncing = false;
  syncNiriLayoutTheme();
}

void ThemeTab::updateAutoStatus() {
  if (m_autoStatus == nullptr || m_config == nullptr) {
    return;
  }

  const ThemeConfig& theme = m_config->config().theme;
  const bool appsAuto = theme.mode == ThemeMode::Auto;
  const bool shellAuto = theme.shellMode == ShellThemeMode::Auto;
  const bool shellFollows = theme.shellMode == ShellThemeMode::Follow;
  if (!appsAuto && !shellAuto) {
    setShown(m_autoStatus, false);
    return;
  }
  if (m_theme == nullptr) {
    setShown(m_autoStatus, false);
    return;
  }

  const auto schedule = m_theme->autoSchedule();
  if (!schedule.ready) {
    m_autoStatus->setText(i18n::tr("control-center.appearance.auto-needs-location"));
    setShown(m_autoStatus, true);
    return;
  }

  const auto unixSeconds = std::chrono::system_clock::to_time_t(schedule.nextBoundary);
  const std::string& configuredFormat = m_config->config().shell.timeFormat;
  const std::string time = formatLocalUnixTime(
      static_cast<std::int64_t>(unixSeconds), configuredFormat.empty() ? "{:%H:%M}" : configuredFormat
  );
  const bool combined = (appsAuto && shellAuto) || (appsAuto && shellFollows);
  std::string text;
  if (combined) {
    text = schedule.night ? i18n::tr("control-center.appearance.auto-dark-until", "time", time)
                          : i18n::tr("control-center.appearance.auto-light-until", "time", time);
  } else if (appsAuto) {
    text = schedule.night ? i18n::tr("control-center.appearance.auto-apps-dark-until", "time", time)
                          : i18n::tr("control-center.appearance.auto-apps-light-until", "time", time);
  } else {
    text = schedule.night ? i18n::tr("control-center.appearance.auto-shell-dark-until", "time", time)
                          : i18n::tr("control-center.appearance.auto-shell-light-until", "time", time);
  }
  m_autoStatus->setText(text);
  setShown(m_autoStatus, true);
}

void ThemeTab::rebuildPaletteGrid() {
  if (m_paletteGrid == nullptr || m_config == nullptr) {
    return;
  }
  uiAssertNotRendering("ThemeTab::rebuildPaletteGrid");

  const ThemeConfig& theme = m_config->config().theme;
  const PaletteSource source = selectedPaletteSource();
  PaletteCatalog catalog = filterPaletteCatalog(buildPaletteCatalog(source, shellThemeMode(theme)), m_paletteQuery);
  const std::string selectedValue = paletteValueForSource(source);
  if (source == PaletteSource::Wallpaper) {
    catalog.previews.assign(catalog.values.size(), {});
    for (std::size_t i = 0; i < catalog.values.size(); ++i) {
      if (catalog.values[i] == selectedValue) {
        catalog.previews[i] = currentRolePreview();
        break;
      }
    }
  }

  std::string gridKey;
  gridKey.reserve(64 + catalog.values.size() * 16);
  gridKey += std::to_string(static_cast<int>(source));
  gridKey += '|';
  gridKey += m_paletteQuery;
  gridKey += '|';
  gridKey += selectedValue;
  gridKey += '|';
  for (const auto& value : catalog.values) {
    gridKey += value;
    gridKey += ',';
  }
  if (gridKey == m_lastGridKey) {
    m_paletteValues = catalog.values;
    return;
  }
  m_lastGridKey = gridKey;
  m_paletteValues = catalog.values;

  const bool emptyCatalog = catalog.labels.empty();
  setShown(m_paletteEmpty, emptyCatalog);
  m_paletteGrid->setVisible(!emptyCatalog);
  m_paletteGrid->setParticipatesInLayout(!emptyCatalog);
  setShown(m_openPalettesButton, source == PaletteSource::Custom);
  setShown(m_openWallpaperButton, source == PaletteSource::Wallpaper);
  setShown(m_paletteFilter, source != PaletteSource::Wallpaper);
  setShown(m_paletteName, source == PaletteSource::Wallpaper);

  if (m_paletteFilter != nullptr) {
    const char* placeholderKey = "control-center.appearance.palette-search";
    if (source == PaletteSource::Custom) {
      placeholderKey = "settings.schema.appearance.custom-palette.search-placeholder";
    } else if (source == PaletteSource::Community) {
      placeholderKey = "settings.schema.appearance.community-palette.search-placeholder";
    }
    m_paletteFilter->setPlaceholder(i18n::tr(placeholderKey));
  }
  if (m_paletteEmpty != nullptr && emptyCatalog) {
    if (!m_paletteQuery.empty()) {
      m_paletteEmpty->setText(i18n::tr("ui.controls.search-picker.empty"));
    } else if (source == PaletteSource::Community) {
      m_paletteEmpty->setText(i18n::tr("control-center.appearance.community-empty"));
    } else if (source == PaletteSource::Custom) {
      m_paletteEmpty->setText(i18n::tr("control-center.appearance.custom-empty"));
    } else {
      m_paletteEmpty->setText(i18n::tr("ui.controls.search-picker.empty"));
    }
  }

  std::size_t selectedIndex = catalog.values.size();
  for (std::size_t i = 0; i < catalog.values.size(); ++i) {
    if (catalog.values[i] == selectedValue) {
      selectedIndex = i;
      break;
    }
  }
  if (m_paletteName != nullptr) {
    if (selectedIndex < catalog.labels.size()) {
      m_paletteName->setText(catalog.labels[selectedIndex]);
    } else if (!selectedValue.empty()) {
      m_paletteName->setText(selectedValue);
    } else {
      m_paletteName->setText(i18n::tr("control-center.appearance.palette"));
    }
  }

  while (!m_paletteGrid->children().empty()) {
    m_paletteGrid->removeChild(m_paletteGrid->children().back().get());
  }
  m_paletteGrid->clearChildSpans();

  const float scale = contentScale();
  const float wellHeight = Style::controlHeightLg * 1.55F * scale;
  const bool wallpaperFallback = source == PaletteSource::Wallpaper;
  for (std::size_t i = 0; i < catalog.labels.size(); ++i) {
    const bool selected = i == selectedIndex;
    const ColorSwatchPreview preview = i < catalog.previews.size() ? catalog.previews[i] : ColorSwatchPreview{};
    auto tile = ui::button({
        .text = catalog.labels[i],
        .fontSize = Style::fontSizeCaption * scale,
        .selected = selected,
        .contentAlign = ButtonContentAlign::Center,
        .customPalette = catalogTilePalette(),
        .tooltip = catalog.labels[i],
        .minHeight = Style::controlHeightLg * 2.45F * scale,
        .padding = Style::spaceSm * scale,
        .gap = Style::spaceXs * scale,
        .radius = Style::scaledRadiusXl(scale),
        .onClick = [this, i]() { applyPaletteSelection(i); },
        .configure =
            [preview, wallpaperFallback, scale, wellHeight](Button& button) {
              button.setDirection(FlexDirection::Vertical);
              button.setAlign(FlexAlign::Stretch);
              button.setJustify(FlexJustify::Center);
              if (button.label() != nullptr) {
                button.label()->setMaxLines(1);
                button.label()->setTextAlign(TextAlign::Center);
                button.label()->setEllipsize(TextEllipsize::End);
              }
              auto well = makeCatalogPreview(preview, wallpaperFallback, scale, wellHeight);
              if (well != nullptr) {
                insertBeforeLabel(button, std::move(well));
              }
            },
    });
    m_paletteGrid->addChild(std::move(tile));
  }
}

void ThemeTab::applyThemeMode(std::size_t index) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  m_config->setThemeMode(themeModeFromIndex(index));
  m_lastGridKey.clear();
  rebuildPaletteGrid();
  updateAutoStatus();
  PanelManager::instance().refresh();
}

void ThemeTab::applyPaletteSource(std::size_t index) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  m_paletteQuery.clear();
  m_lastGridKey.clear();
  if (m_paletteFilter != nullptr) {
    m_paletteFilter->setValue("");
  }
  const PaletteSource source = paletteSourceFromIndex(index);
  const PaletteCatalog catalog =
      filterPaletteCatalog(buildPaletteCatalog(source, shellThemeMode(m_config->config().theme)), m_paletteQuery);
  m_paletteValues = catalog.values;
  std::string value = paletteValueForSource(source);
  if (value.empty() && !m_paletteValues.empty()) {
    value = m_paletteValues.front();
  }
  if (!value.empty()) {
    (void)m_config->setThemeColorScheme(source, value);
  }
  PanelManager::instance().refresh();
}

void ThemeTab::applyPaletteSelection(std::size_t index) {
  if (m_syncing || m_config == nullptr || index >= m_paletteValues.size()) {
    return;
  }
  (void)m_config->setThemeColorScheme(selectedPaletteSource(), m_paletteValues[index]);
  PanelManager::instance().refresh();
}

compositors::niri::InstallResult ThemeTab::syncNiriLayoutTheme() {
  if (m_config == nullptr) {
    return {};
  }
  const auto& niriLayout = m_config->config().theme.niriLayout;
  const compositors::niri::LayoutThemeSettings settings{
      .focusRing = niriLayout.focusRing,
      .border = niriLayout.border,
      .activeColorHex = colorHex(colorForRole(ColorRole::Primary)),
      .inactiveColorHex = colorHex(colorForRole(ColorRole::Outline)),
      .urgentColorHex = colorHex(colorForRole(ColorRole::Error)),
      .tabInactiveColorHex = colorHex(colorForRole(ColorRole::Secondary)),
      .insertHintColorHex = colorHex(colorForRole(ColorRole::Primary)) + "80",
      .gaps = niriLayout.gaps,
      .struts = niriLayout.struts,
      .defaultColumnWidth = niriLayout.defaultColumnWidth,
  };
  return compositors::niri::saveNiriLayoutTheme(settings);
}

void ThemeTab::openCustomPalettesFolder() {
  const std::filesystem::path dir = noctalia::theme::customPaletteDir();
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) {
    (void)std::filesystem::create_directories(dir, ec);
  }
  if (!process::commandExists("xdg-open")) {
    if (m_exportStatus != nullptr) {
      m_exportStatus->setText(i18n::tr("control-center.appearance.open-folder-failed"));
      m_exportStatus->setColor(colorSpecFromRole(ColorRole::Error));
      setShown(m_exportStatus, true);
    }
    return;
  }
  (void)process::runAsync(std::vector<std::string>{"xdg-open", dir.string()});
}

void ThemeTab::saveCurrentPalette() {
  if (m_theme == nullptr) {
    return;
  }
  std::string paletteName;
  std::string error;
  if (!m_theme->saveCurrentPaletteAsCustom(&paletteName, &error)) {
    if (m_exportStatus != nullptr) {
      m_exportStatus->setText(
          error.empty() ? i18n::tr("control-center.appearance.customize-failed") : error
      );
      m_exportStatus->setColor(colorSpecFromRole(ColorRole::Error));
      setShown(m_exportStatus, true);
    }
    return;
  }
  if (m_exportStatus != nullptr) {
    m_exportStatus->setText(i18n::tr("control-center.appearance.export-saved", "name", paletteName));
    m_exportStatus->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    setShown(m_exportStatus, true);
  }
  PanelManager::instance().refresh();
}

void ThemeTab::pickRoleColor(ColorRole role, std::string roleLabel) {
  if (m_theme == nullptr) {
    return;
  }

  ColorPickerDialogOptions options;
  options.title = i18n::tr("control-center.appearance.edit-role-title", "role", roleLabel);
  options.initialColor = colorForRole(role);
  (void)ColorPickerDialog::open(std::move(options), [this, role](std::optional<Color> result) {
    if (!result.has_value() || m_theme == nullptr) {
      return;
    }

    Color rgb = *result;
    rgb.a = 1.0F;
    std::string paletteName;
    std::string error;
    if (!m_theme->saveRoleColorAsCustom(role, rgb, &paletteName, &error)) {
      if (m_exportStatus != nullptr) {
        m_exportStatus->setText(
            error.empty() ? i18n::tr("control-center.appearance.customize-failed") : error
        );
        m_exportStatus->setColor(colorSpecFromRole(ColorRole::Error));
        setShown(m_exportStatus, true);
      }
      return;
    }
    if (m_exportStatus != nullptr) {
      m_exportStatus->setText(i18n::tr("control-center.appearance.export-saved", "name", paletteName));
      m_exportStatus->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      setShown(m_exportStatus, true);
    }
    PanelManager::instance().refresh();
  });
}

PaletteSource ThemeTab::selectedPaletteSource() const {
  if (m_sourceSwitch == nullptr) {
    return m_config != nullptr ? m_config->config().theme.source : PaletteSource::Builtin;
  }
  return paletteSourceFromIndex(m_sourceSwitch->selectedIndex());
}

std::string ThemeTab::paletteValueForSource(PaletteSource source) const {
  if (m_config == nullptr) {
    return {};
  }
  const ThemeConfig& theme = m_config->config().theme;
  switch (source) {
  case PaletteSource::Builtin:
    return theme.builtinPalette;
  case PaletteSource::Wallpaper:
    return theme.wallpaperScheme;
  case PaletteSource::Community:
    return theme.communityPalette;
  case PaletteSource::Custom:
    return theme.customPalette;
  }
  return {};
}
