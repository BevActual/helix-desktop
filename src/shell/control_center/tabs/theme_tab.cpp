#include "shell/control_center/tabs/theme_tab.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "compositors/niri/niri_config_blur.h"
#include "compositors/niri/niri_layout_theme.h"
#include "compositors/niri/niri_runtime.h"
#include "compositors/niri/niri_window_rules.h"
#include "config/config_service.h"
#include "config/schema/ranges.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
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
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select.h"
#include "ui/controls/segmented.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <system_error>
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

  [[nodiscard]] std::size_t shellModeIndex(ShellThemeMode mode) {
    switch (mode) {
    case ShellThemeMode::Follow:
      return 0;
    case ShellThemeMode::Dark:
      return 1;
    case ShellThemeMode::Light:
      return 2;
    case ShellThemeMode::Auto:
    default:
      return 3;
    }
  }

  [[nodiscard]] ShellThemeMode shellModeFromIndex(std::size_t index) {
    switch (index) {
    case 1:
      return ShellThemeMode::Dark;
    case 2:
      return ShellThemeMode::Light;
    case 3:
      return ShellThemeMode::Auto;
    default:
      return ShellThemeMode::Follow;
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

  [[nodiscard]] std::string colorHex(const Color& color) {
    const auto channel = [](float v) { return static_cast<int>(std::clamp(v, 0.0F, 1.0F) * 255.0F); };
    return std::format("#{:02x}{:02x}{:02x}", channel(color.r), channel(color.g), channel(color.b));
  }

  [[nodiscard]] std::string integerLabel(double value) {
    return std::format("{}", static_cast<int>(std::lround(value)));
  }

  [[nodiscard]] std::string oneDecimalLabel(double value) { return std::format("{:.1f}", value); }

  [[nodiscard]] std::string twoDecimalLabel(double value) { return std::format("{:.2f}", value); }

  void addLabeledSlider(
      Flex& card, float scale, float captionSize, float controlHeight, const std::string& title, Slider** sliderOut,
      Label** valueOut, double minValue, double maxValue, double step, double value,
      std::function<std::string(double)> format, std::function<void()> onCommit
  ) {
    auto header = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
    });
    header->addChild(
        ui::label({
            .text = title,
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .flexGrow = 1.0F,
        })
    );
    header->addChild(
        ui::label({
            .out = valueOut,
            .text = format(value),
            .fontSize = captionSize,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    card.addChild(std::move(header));
    card.addChild(
        ui::slider({
            .out = sliderOut,
            .minValue = minValue,
            .maxValue = maxValue,
            .step = step,
            .value = value,
            .trackHeight = Style::sliderTrackHeight * scale,
            .thumbSize = Style::sliderThumbSize * scale,
            .controlHeight = controlHeight,
            .flexGrow = 1.0F,
            .onValueChanged =
                [valueOut, format](double next) {
                  if (valueOut != nullptr && *valueOut != nullptr) {
                    (*valueOut)->setText(format(next));
                  }
                },
            .onDragEnd = std::move(onCommit),
        })
    );
  }

} // namespace

ThemeTab::ThemeTab(const ControlCenterServices& services)
    : m_config(services.config), m_theme(services.theme), m_platform(services.platform),
      m_settings(std::make_unique<SettingsPane>(
          services,
          SettingsPaneOptions{
              .section = "theme",
              .includedGroups = {"interface", "accessibility", "motion", "borders", "effects", "niri"},
          }
      )) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> ThemeTab::create() {
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
      .configure = [](ScrollView& scrollView) {
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

  auto themeCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addTitle(*themeCard, i18n::tr("settings.navigation.groups.theme"), scale);

  addCaption(*themeCard, i18n::tr("settings.schema.appearance.theme-mode.label"));
  themeCard->addChild(
      ui::segmented({
          .out = &m_themeMode,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = i18n::tr("settings.options.theme.mode.dark"),
                      .tooltip = i18n::tr("control-center.appearance.always-dark"),
                  },
                  {
                      .label = i18n::tr("settings.options.theme.mode.light"),
                      .tooltip = i18n::tr("control-center.appearance.always-light"),
                  },
                  {
                      .label = i18n::tr("common.states.auto"),
                      .tooltip = i18n::tr("control-center.appearance.auto-schedule"),
                  },
              },
          .fontSize = captionSize,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyThemeMode(index); },
      })
  );

  addCaption(*themeCard, i18n::tr("settings.schema.appearance.shell-theme-mode.label"));
  themeCard->addChild(
      ui::segmented({
          .out = &m_shellMode,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = i18n::tr("settings.options.theme.shell-mode.follow"),
                      .tooltip = i18n::tr("control-center.appearance.follow-theme"),
                  },
                  {
                      .label = i18n::tr("settings.options.theme.mode.dark"),
                      .tooltip = i18n::tr("control-center.appearance.always-dark"),
                  },
                  {
                      .label = i18n::tr("settings.options.theme.mode.light"),
                      .tooltip = i18n::tr("control-center.appearance.always-light"),
                  },
                  {
                      .label = i18n::tr("common.states.auto"),
                      .tooltip = i18n::tr("control-center.appearance.auto-schedule"),
                  },
              },
          .fontSize = captionSize,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyShellMode(index); },
      })
  );
  themeCard->addChild(
      ui::label({
          .out = &m_autoStatus,
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
          .visible = false,
          .participatesInLayout = false,
      })
  );

  addCaption(*themeCard, i18n::tr("settings.schema.appearance.palette-source.label"));
  themeCard->addChild(
      ui::segmented({
          .out = &m_paletteSource,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = i18n::tr("settings.options.theme.source.built-in"),
                      .tooltip = i18n::tr("control-center.appearance.palette-builtin-tooltip"),
                  },
                  {
                      .label = i18n::tr("settings.options.theme.source.wallpaper"),
                      .tooltip = i18n::tr("control-center.appearance.palette-wallpaper-tooltip"),
                  },
                  {
                      .label = i18n::tr("settings.options.theme.source.community"),
                      .tooltip = i18n::tr("control-center.appearance.palette-community-tooltip"),
                  },
                  {
                      .label = i18n::tr("settings.options.theme.source.custom"),
                      .tooltip = i18n::tr("control-center.appearance.palette-custom-tooltip"),
                  },
              },
          .fontSize = captionSize,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyPaletteSource(index); },
      })
  );

  themeCard->addChild(
      ui::select({
          .out = &m_paletteSelect,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = controlHeight,
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0F,
          .onSelectionChanged =
              [this](std::size_t index, std::string_view) { applyPaletteSelection(index); },
      })
  );
  themeCard->addChild(
      ui::button({
          .out = &m_palettePickerButton,
          .text = i18n::tr("control-center.appearance.choose-palette"),
          .fontSize = Style::fontSizeBody * scale,
          .contentAlign = ButtonContentAlign::Start,
          .variant = ButtonVariant::Default,
          .tooltip = i18n::tr("control-center.appearance.choose-palette-tooltip"),
          .minHeight = controlHeight,
          .paddingV = Style::spaceSm * scale,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
          .flexGrow = 1.0F,
          .visible = false,
          .participatesInLayout = false,
          .onClick = [this]() { openPalettePicker(); },
      })
  );
  themeCard->addChild(
      ui::label({
          .out = &m_paletteEmpty,
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  themeCard->addChild(
      ui::button({
          .out = &m_openPalettesButton,
          .text = i18n::tr("control-center.appearance.open-palettes-folder"),
          .glyph = "folder",
          .fontSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.appearance.open-palettes-folder"),
          .minHeight = controlHeight,
          .paddingH = Style::spaceMd * scale,
          .visible = false,
          .participatesInLayout = false,
          .onClick = [this]() { openCustomPalettesFolder(); },
      })
  );

  themeCard->addChild(
      ui::button({
          .out = &m_exportButton,
          .text = i18n::tr("settings.schema.appearance.export-wallpaper-palette.button"),
          .glyph = "device-floppy",
          .fontSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.appearance.export-palette-tooltip"),
          .minHeight = controlHeight,
          .paddingH = Style::spaceMd * scale,
          .onClick = [this]() { exportWallpaperPalette(); },
      })
  );
  themeCard->addChild(
      ui::label({
          .out = &m_exportStatus,
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
          .visible = false,
      })
  );

  addCaption(*themeCard, i18n::tr("control-center.appearance.palette-hint"));

  auto swatchRow = ui::row({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });
  const float swatchHeight = Style::controlHeightSm * scale;
  for (const auto& swatch : kPaletteSwatches) {
    auto cell = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .flexGrow = 1.0F,
    });
    cell->addChild(
        ui::box({
            .fill = colorSpecFromRole(swatch.role),
            .border = colorSpecFromRole(ColorRole::Outline, 0.35F),
            .borderWidth = 1.0F,
            .radius = Style::scaledRadiusSm(scale),
            .height = swatchHeight,
        })
    );
    cell->addChild(
        ui::label({
            .text = i18n::tr(swatch.labelKey),
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
            .textAlign = TextAlign::Center,
        })
    );
    swatchRow->addChild(std::move(cell));
  }
  themeCard->addChild(std::move(swatchRow));

  auto pureBlackRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });
  pureBlackRow->addChild(
      ui::label({
          .text = i18n::tr("settings.schema.appearance.pure-black-dark.label"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0F,
      })
  );
  pureBlackRow->addChild(
      ui::toggle({
          .out = &m_pureBlack,
          .scale = scale,
          .onChange = [this](bool enabled) { applyPureBlack(enabled); },
      })
  );
  themeCard->addChild(std::move(pureBlackRow));
  content->addChild(std::move(themeCard));

  if (compositors::isNiri()) {
    auto niriCard = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .configure =
            [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
    });
    addTitle(*niriCard, i18n::tr("control-center.theme.niri-title"), scale);
    niriCard->addChild(
        ui::label({
            .text = i18n::tr("control-center.theme.niri-description"),
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 3,
        })
    );
    auto focusRingRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    focusRingRow->addChild(
        ui::label({
            .text = i18n::tr("control-center.theme.niri-focus-ring"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
    focusRingRow->addChild(
        ui::toggle({
            .out = &m_niriFocusRing,
            .scale = scale,
            .onChange = [this](bool enabled) { applyNiriFocusRing(enabled); },
        })
    );
    niriCard->addChild(std::move(focusRingRow));

    auto borderRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    borderRow->addChild(
        ui::label({
            .text = i18n::tr("control-center.theme.niri-border"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
    borderRow->addChild(
        ui::toggle({
            .out = &m_niriBorder,
            .scale = scale,
            .onChange = [this](bool enabled) { applyNiriBorder(enabled); },
        })
    );
    niriCard->addChild(std::move(borderRow));

    auto drawBorderRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    drawBorderRow->addChild(
        ui::label({
            .text = i18n::tr("control-center.theme.niri-draw-border"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
    drawBorderRow->addChild(
        ui::toggle({
            .out = &m_niriDrawBorder,
            .scale = scale,
            .onChange = [this](bool enabled) { applyNiriDrawBorder(enabled); },
        })
    );
    niriCard->addChild(std::move(drawBorderRow));
    if (niriBlurSupported()) {
      auto blurRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
      blurRow->addChild(
          ui::label({
              .text = i18n::tr("control-center.theme.niri-blur"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .flexGrow = 1.0F,
          })
      );
      blurRow->addChild(
          ui::toggle({
              .out = &m_niriBlur,
              .scale = scale,
              .onChange = [this](bool enabled) { applyNiriBlur(enabled); },
          })
      );
      niriCard->addChild(std::move(blurRow));
      const bool blurOn = m_config != nullptr && m_config->config().theme.niriLayout.blur;
      auto blurDetails = ui::column({
          .out = &m_niriBlurDetails,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceSm * scale,
          .visible = blurOn,
          .participatesInLayout = blurOn,
      });
      addLabeledSlider(
          *blurDetails, scale, captionSize, controlHeight, i18n::tr("settings.schema.niri.blur-passes.label"),
          &m_niriBlurPasses, &m_niriBlurPassesValue,
          static_cast<double>(noctalia::config::schema::kNiriBlurPassesRange.min.value_or(1)),
          static_cast<double>(noctalia::config::schema::kNiriBlurPassesRange.max.value_or(8)),
          static_cast<double>(noctalia::config::schema::kNiriBlurPassesRange.step.value_or(1)), 2.0, integerLabel,
          [this]() { commitNiriBlurPasses(); }
      );
      addLabeledSlider(
          *blurDetails, scale, captionSize, controlHeight, i18n::tr("settings.schema.niri.blur-offset.label"),
          &m_niriBlurOffset, &m_niriBlurOffsetValue,
          static_cast<double>(noctalia::config::schema::kNiriBlurOffsetRange.min.value_or(0.5F)),
          static_cast<double>(noctalia::config::schema::kNiriBlurOffsetRange.max.value_or(8.0F)),
          static_cast<double>(noctalia::config::schema::kNiriBlurOffsetRange.step.value_or(0.1F)), 3.0, oneDecimalLabel,
          [this]() { commitNiriBlurOffset(); }
      );
      addLabeledSlider(
          *blurDetails, scale, captionSize, controlHeight, i18n::tr("settings.schema.niri.blur-noise.label"),
          &m_niriBlurNoise, &m_niriBlurNoiseValue,
          static_cast<double>(noctalia::config::schema::kNiriBlurNoiseRange.min.value_or(0.0F)),
          static_cast<double>(noctalia::config::schema::kNiriBlurNoiseRange.max.value_or(0.2F)),
          static_cast<double>(noctalia::config::schema::kNiriBlurNoiseRange.step.value_or(0.01F)), 0.03, twoDecimalLabel,
          [this]() { commitNiriBlurNoise(); }
      );
      niriCard->addChild(std::move(blurDetails));
    }
    content->addChild(std::move(niriCard));
  }

  live->addChild(std::move(scroll));
  auto wrapped = m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
}

std::unique_ptr<Flex> ThemeTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void ThemeTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_themeMode = nullptr;
  m_shellMode = nullptr;
  m_autoStatus = nullptr;
  m_paletteSource = nullptr;
  m_paletteSelect = nullptr;
  m_palettePickerButton = nullptr;
  m_paletteEmpty = nullptr;
  m_openPalettesButton = nullptr;
  m_exportButton = nullptr;
  m_exportStatus = nullptr;
  m_pureBlack = nullptr;
  m_niriFocusRing = nullptr;
  m_niriBorder = nullptr;
  m_niriDrawBorder = nullptr;
  m_niriBlur = nullptr;
  m_niriBlurDetails = nullptr;
  m_niriBlurPasses = nullptr;
  m_niriBlurPassesValue = nullptr;
  m_niriBlurOffset = nullptr;
  m_niriBlurOffsetValue = nullptr;
  m_niriBlurNoise = nullptr;
  m_niriBlurNoiseValue = nullptr;
}

void ThemeTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool ThemeTab::configureMode() const { return m_settings->configureMode(); }

void ThemeTab::setSettingsSearchQuery(std::string_view query) {
  if (!query.empty()) {
    m_settings->setConfigureMode(true);
  }
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* ThemeTab::settingsPane() { return m_settings.get(); }

void ThemeTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void ThemeTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  } else {
    updateAutoStatus();
  }
}

void ThemeTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }

  const ThemeConfig& theme = m_config->config().theme;
  m_syncing = true;
  if (m_themeMode != nullptr) {
    m_themeMode->setSelectedIndex(themeModeIndex(theme.mode));
  }
  if (m_shellMode != nullptr) {
    m_shellMode->setSelectedIndex(shellModeIndex(theme.shellMode));
  }
  if (m_paletteSource != nullptr) {
    m_paletteSource->setSelectedIndex(paletteSourceIndex(theme.source));
  }
  rebuildPaletteSelect();
  updateAutoStatus();
  const bool wallpaper = theme.source == PaletteSource::Wallpaper;
  setShown(m_exportButton, wallpaper);
  setShown(m_openPalettesButton, theme.source == PaletteSource::Custom);
  if (theme.source != PaletteSource::Wallpaper && theme.source != PaletteSource::Custom) {
    setShown(m_exportStatus, false);
  }
  if (m_pureBlack != nullptr) {
    m_pureBlack->setCheckedImmediate(theme.pureBlackDark);
  }
  if (m_niriFocusRing != nullptr) {
    m_niriFocusRing->setCheckedImmediate(theme.niriLayout.focusRing);
  }
  if (m_niriBorder != nullptr) {
    m_niriBorder->setCheckedImmediate(theme.niriLayout.border);
  }
  if (m_niriDrawBorder != nullptr) {
    m_niriDrawBorder->setCheckedImmediate(theme.niriLayout.drawBorderWithBackground.value_or(true));
  }
  if (m_niriBlur != nullptr) {
    m_niriBlur->setCheckedImmediate(theme.niriLayout.blur);
  }
  if (setShown(m_niriBlurDetails, theme.niriLayout.blur)) {
    PanelManager::instance().requestLayout();
  }
  if (m_niriBlurPasses != nullptr) {
    m_niriBlurPasses->setValue(static_cast<double>(theme.niriLayout.blurPasses));
  }
  if (m_niriBlurPassesValue != nullptr) {
    m_niriBlurPassesValue->setText(integerLabel(static_cast<double>(theme.niriLayout.blurPasses)));
  }
  if (m_niriBlurOffset != nullptr) {
    m_niriBlurOffset->setValue(theme.niriLayout.blurOffset);
  }
  if (m_niriBlurOffsetValue != nullptr) {
    m_niriBlurOffsetValue->setText(oneDecimalLabel(theme.niriLayout.blurOffset));
  }
  if (m_niriBlurNoise != nullptr) {
    m_niriBlurNoise->setValue(theme.niriLayout.blurNoise);
  }
  if (m_niriBlurNoiseValue != nullptr) {
    m_niriBlurNoiseValue->setText(twoDecimalLabel(theme.niriLayout.blurNoise));
  }
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

void ThemeTab::rebuildPaletteSelect() {
  if (m_config == nullptr) {
    return;
  }

  const ThemeConfig& theme = m_config->config().theme;
  const PaletteSource source = m_paletteSource != nullptr ? selectedPaletteSource() : theme.source;
  const ThemeMode previewMode = shellThemeMode(theme);
  const PaletteCatalog catalog = buildPaletteCatalog(source, previewMode);
  m_paletteValues = catalog.values;
  std::string selectedValue = paletteValueForSource(source);

  const bool usesSelect = source == PaletteSource::Builtin || source == PaletteSource::Wallpaper;
  const bool usesPicker =
      (source == PaletteSource::Community || source == PaletteSource::Custom) && !catalog.labels.empty();
  const bool emptyCatalog =
      (source == PaletteSource::Community || source == PaletteSource::Custom) && catalog.labels.empty();

  setShown(m_paletteSelect, usesSelect && !catalog.labels.empty());
  setShown(m_palettePickerButton, usesPicker);
  setShown(m_paletteEmpty, emptyCatalog);
  setShown(m_openPalettesButton, source == PaletteSource::Custom);
  setShown(m_exportButton, source == PaletteSource::Wallpaper);

  if (m_paletteEmpty != nullptr && emptyCatalog) {
    m_paletteEmpty->setText(
        source == PaletteSource::Community ? i18n::tr("control-center.appearance.community-empty")
                                           : i18n::tr("control-center.appearance.custom-empty")
    );
  }

  std::size_t selectedIndex = 0;
  bool found = false;
  for (std::size_t i = 0; i < m_paletteValues.size(); ++i) {
    if (m_paletteValues[i] == selectedValue) {
      selectedIndex = i;
      found = true;
      break;
    }
  }

  if (m_palettePickerButton != nullptr && usesPicker) {
    if (found) {
      m_palettePickerButton->setText(catalog.labels[selectedIndex]);
    } else if (!selectedValue.empty()) {
      m_palettePickerButton->setText(selectedValue);
    } else {
      m_palettePickerButton->setText(i18n::tr("control-center.appearance.choose-palette"));
    }
  }

  if (m_paletteSelect == nullptr) {
    return;
  }
  if (!usesSelect || catalog.labels.empty()) {
    m_paletteSelect->setOptions({});
    m_paletteSelect->setColorSwatchPreviews({});
    m_paletteSelect->clearSelection();
    return;
  }

  m_paletteSelect->setOptions(catalog.labels);
  m_paletteSelect->setColorSwatchPreviews(catalog.previews);
  m_paletteSelect->setSelectedIndexSilently(selectedIndex);
}

void ThemeTab::applyThemeMode(std::size_t index) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  m_config->setThemeMode(themeModeFromIndex(index));
  rebuildPaletteSelect();
  updateAutoStatus();
  PanelManager::instance().refresh();
}

void ThemeTab::applyShellMode(std::size_t index) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride(
      {"theme", "shell_mode"}, std::string(enumToKey(kShellThemeModes, shellModeFromIndex(index)))
  );
  rebuildPaletteSelect();
  updateAutoStatus();
  PanelManager::instance().refresh();
}

void ThemeTab::applyPaletteSource(std::size_t index) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  const PaletteSource source = paletteSourceFromIndex(index);
  rebuildPaletteSelect();
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

void ThemeTab::applyPureBlack(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"theme", "pure_black_dark"}, enabled);
}

void ThemeTab::applyNiriFocusRing(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"theme", "niri_layout", "focus_ring"}, enabled);
  if (const auto result = syncNiriLayoutTheme(); !result.ok) {
    notify::error(
        "Noctalia", i18n::tr("control-center.theme.niri-save-title"),
        result.error.empty() ? i18n::tr("control-center.theme.niri-save-failed") : result.error
    );
  }
}

void ThemeTab::applyNiriBorder(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"theme", "niri_layout", "border"}, enabled);
  if (const auto result = syncNiriLayoutTheme(); !result.ok) {
    notify::error(
        "Noctalia", i18n::tr("control-center.theme.niri-save-title"),
        result.error.empty() ? i18n::tr("control-center.theme.niri-save-failed") : result.error
    );
  }
}

void ThemeTab::applyNiriDrawBorder(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"theme", "niri_layout", "draw_border_with_background"}, enabled);
  const auto result = compositors::niri::saveWindowRules({
      .cornerRadius = compositors::niri::currentWindowCornerRadius(),
      .drawBorderWithBackground = enabled,
  });
  if (!result.ok) {
    notify::error(
        "Noctalia", i18n::tr("control-center.theme.niri-save-title"),
        result.error.empty() ? i18n::tr("control-center.theme.niri-save-failed") : result.error
    );
  }
}

void ThemeTab::applyNiriBlur(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"theme", "niri_layout", "blur"}, enabled);
  if (setShown(m_niriBlurDetails, enabled)) {
    PanelManager::instance().requestLayout();
  }
  if (const auto result = syncNiriBlur(); !result.ok) {
    notify::error(
        "Noctalia", i18n::tr("control-center.theme.niri-blur-save-title"),
        result.error.empty() ? i18n::tr("control-center.theme.niri-save-failed") : result.error
    );
  }
}

void ThemeTab::commitNiriBlurPasses() {
  if (m_syncing || m_config == nullptr || m_niriBlurPasses == nullptr) {
    return;
  }
  (void)m_config->setOverride(
      {"theme", "niri_layout", "blur_passes"}, static_cast<std::int64_t>(std::lround(m_niriBlurPasses->value()))
  );
  if (const auto result = syncNiriBlur(); !result.ok) {
    notify::error(
        "Noctalia", i18n::tr("control-center.theme.niri-blur-save-title"),
        result.error.empty() ? i18n::tr("control-center.theme.niri-save-failed") : result.error
    );
  }
}

void ThemeTab::commitNiriBlurOffset() {
  if (m_syncing || m_config == nullptr || m_niriBlurOffset == nullptr) {
    return;
  }
  (void)m_config->setOverride({"theme", "niri_layout", "blur_offset"}, m_niriBlurOffset->value());
  if (const auto result = syncNiriBlur(); !result.ok) {
    notify::error(
        "Noctalia", i18n::tr("control-center.theme.niri-blur-save-title"),
        result.error.empty() ? i18n::tr("control-center.theme.niri-save-failed") : result.error
    );
  }
}

void ThemeTab::commitNiriBlurNoise() {
  if (m_syncing || m_config == nullptr || m_niriBlurNoise == nullptr) {
    return;
  }
  (void)m_config->setOverride({"theme", "niri_layout", "blur_noise"}, m_niriBlurNoise->value());
  if (const auto result = syncNiriBlur(); !result.ok) {
    notify::error(
        "Noctalia", i18n::tr("control-center.theme.niri-blur-save-title"),
        result.error.empty() ? i18n::tr("control-center.theme.niri-save-failed") : result.error
    );
  }
}

bool ThemeTab::niriBlurSupported() const {
  if (m_platform == nullptr || !compositors::isNiri()) {
    return false;
  }
  const auto version = m_platform->niriRuntime().compositorVersion();
  return version.has_value() && compositors::niri::versionSupportsBlur(*version);
}

compositors::niri::InstallResult ThemeTab::syncNiriBlur() {
  if (m_config == nullptr) {
    return {};
  }
  const auto& niri = m_config->config().theme.niriLayout;
  return compositors::niri::saveNiriBlur({
      .enabled = niri.blur,
      .passes = niri.blurPasses,
      .offset = niri.blurOffset,
      .noise = niri.blurNoise,
  });
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

void ThemeTab::openPalettePicker() {
  if (m_config == nullptr || m_settings == nullptr) {
    return;
  }
  const PaletteSource source = selectedPaletteSource();
  if (source != PaletteSource::Community && source != PaletteSource::Custom) {
    return;
  }
  const ThemeMode previewMode = shellThemeMode(m_config->config().theme);
  const PaletteCatalog catalog = buildPaletteCatalog(source, previewMode);
  if (catalog.values.empty()) {
    return;
  }

  std::vector<settings::SelectOption> options;
  options.reserve(catalog.values.size());
  for (std::size_t i = 0; i < catalog.values.size(); ++i) {
    settings::SelectOption option;
    option.value = catalog.values[i];
    option.label = catalog.labels[i];
    if (i < catalog.previews.size()) {
      option.preview = catalog.previews[i];
    }
    options.push_back(std::move(option));
  }

  const bool community = source == PaletteSource::Community;
  m_settings->openSearchPicker(
      settings::SearchPickerOpenRequest{
          .title = i18n::tr(
              community ? "settings.schema.appearance.community-palette.label"
                        : "settings.schema.appearance.custom-palette.label"
          ),
          .options = std::move(options),
          .selectedValue = paletteValueForSource(source),
          .placeholder = i18n::tr(
              community ? "settings.schema.appearance.community-palette.search-placeholder"
                        : "settings.schema.appearance.custom-palette.search-placeholder"
          ),
          .emptyText = i18n::tr("ui.controls.search-picker.empty"),
          .settingPath = community ? std::vector<std::string>{"theme", "community_palette"}
                                    : std::vector<std::string>{"theme", "custom_palette"},
          .onSelect =
              [this, source](const std::string& value) {
                if (m_config != nullptr) {
                  (void)m_config->setThemeColorScheme(source, value);
                }
                rebuildPaletteSelect();
                PanelManager::instance().refresh();
              },
      }
  );
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

void ThemeTab::exportWallpaperPalette() {
  if (m_theme == nullptr) {
    return;
  }
  std::string paletteName;
  std::string error;
  if (!m_theme->saveWallpaperPaletteAsCustom(&paletteName, &error)) {
    if (m_exportStatus != nullptr) {
      m_exportStatus->setText(
          error.empty() ? i18n::tr("settings.errors.export-wallpaper-palette") : error
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

PaletteSource ThemeTab::selectedPaletteSource() const {
  if (m_paletteSource == nullptr) {
    return m_config != nullptr ? m_config->config().theme.source : PaletteSource::Builtin;
  }
  return paletteSourceFromIndex(m_paletteSource->selectedIndex());
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
