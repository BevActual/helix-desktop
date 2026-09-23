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
#include "theme/fixed_palette.h"
#include "theme/image_loader.h"
#include "theme/palette_generator.h"
#include "theme/scheme.h"
#include "theme/theme_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/color_swatch_preview.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/dialogs/color_picker_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
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

  [[nodiscard]] std::vector<ColorSwatchPreview>
  wallpaperSchemePreviews(ConfigService& config, ThemeMode previewMode) {
    struct Cache {
      std::string path;
      std::int64_t mtimeNs = 0;
      ThemeMode mode = ThemeMode::Dark;
      std::vector<ColorSwatchPreview> previews;
    };
    static Cache cache;

    const std::string path = config.getPaletteWallpaperPath();
    std::int64_t mtimeNs = 0;
    if (!path.empty()) {
      std::error_code ec;
      const auto writeTime = std::filesystem::last_write_time(path, ec);
      mtimeNs = ec ? 0 : writeTime.time_since_epoch().count();
    }
    if (cache.path == path && cache.mtimeNs == mtimeNs && cache.mode == previewMode
        && cache.previews.size() == kWallpaperSchemeValues.size()) {
      return cache.previews;
    }

    cache.path = path;
    cache.mtimeNs = mtimeNs;
    cache.mode = previewMode;
    cache.previews.assign(kWallpaperSchemeValues.size(), {});
    if (path.empty()) {
      return cache.previews;
    }

    std::optional<noctalia::theme::LoadedImage> materialImage;
    std::optional<noctalia::theme::LoadedImage> customImage;
    const auto rgbForScheme = [&](noctalia::theme::Scheme scheme) -> const std::vector<std::uint8_t>* {
      std::optional<noctalia::theme::LoadedImage>& slot =
          noctalia::theme::isMaterialScheme(scheme) ? materialImage : customImage;
      if (!slot.has_value()) {
        auto loaded = noctalia::theme::loadAndResize(path, scheme);
        if (!loaded) {
          return nullptr;
        }
        slot = std::move(*loaded);
      }
      return &slot->rgb;
    };

    for (std::size_t i = 0; i < kWallpaperSchemeValues.size(); ++i) {
      const auto scheme = noctalia::theme::schemeFromString(kWallpaperSchemeValues[i]);
      if (!scheme.has_value()) {
        continue;
      }
      const std::vector<std::uint8_t>* rgb = rgbForScheme(*scheme);
      if (rgb == nullptr) {
        continue;
      }
      const auto generated = noctalia::theme::generate(*rgb, *scheme);
      if (!generated) {
        continue;
      }
      const noctalia::theme::TokenMap& tokens =
          previewMode == ThemeMode::Light ? generated->light : generated->dark;
      cache.previews[i] = previewFromPalette(noctalia::theme::mapGeneratedPaletteMode(tokens));
    }
    return cache.previews;
  }

  struct PaletteCatalog {
    std::vector<std::string> values;
    std::vector<std::string> labels;
    std::vector<ColorSwatchPreview> previews;
  };

  [[nodiscard]] PaletteCatalog
  buildPaletteCatalog(PaletteSource source, ThemeMode previewMode, ConfigService* config) {
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
    case PaletteSource::Wallpaper: {
      const std::vector<ColorSwatchPreview> previews =
          config != nullptr ? wallpaperSchemePreviews(*config, previewMode) : std::vector<ColorSwatchPreview>{};
      for (std::size_t i = 0; i < kWallpaperSchemeValues.size(); ++i) {
        catalog.values.emplace_back(kWallpaperSchemeValues[i]);
        catalog.labels.emplace_back(i18n::tr(kWallpaperSchemeLabelKeys[i]));
        if (i < previews.size()) {
          catalog.previews.push_back(previews[i]);
        }
      }
      break;
    }
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

  [[nodiscard]] ColorSpec opaqueSpec(const ColorSpec& spec) {
    Color color = resolveColorSpec(spec);
    color.a = 1.0F;
    return fixedColorSpec(color);
  }

  [[nodiscard]] ColorSpec leftoverFill(const ColorSpec& overlay) {
    const Color top = resolveColorSpec(overlay);
    const Color base = resolveColorSpec(colorSpecFromRole(ColorRole::Surface));
    const float a = std::clamp(top.a, 0.0F, 1.0F);
    return fixedColorSpec(Color{
        .r = top.r * a + base.r * (1.0F - a),
        .g = top.g * a + base.g * (1.0F - a),
        .b = top.b * a + base.b * (1.0F - a),
        .a = 1.0F,
    });
  }

  [[nodiscard]] Node* contentChild(Flex& flex, std::size_t index) {
    std::size_t n = 0;
    for (const auto& child : flex.children()) {
      if (child == nullptr || child->zIndex() < 0 || child->type() == NodeType::Rect) {
        continue;
      }
      if (n == index) {
        return child.get();
      }
      ++n;
    }
    return nullptr;
  }

  class PaletteGridAdapter final : public VirtualGridAdapter {
  public:
    explicit PaletteGridAdapter(float scale) : m_scale(scale) {}

    void setCatalog(PaletteCatalog catalog, std::string fallbackGlyph) {
      m_catalog = std::move(catalog);
      m_fallbackGlyph = std::move(fallbackGlyph);
    }

    [[nodiscard]] std::size_t itemCount() const override { return m_catalog.labels.size(); }

    [[nodiscard]] std::unique_ptr<Node> createTile() override {
      const float wellHeight = Style::controlHeight * 1.15F * m_scale;
      auto tile = ui::column({
          .align = FlexAlign::Stretch,
          .justify = FlexJustify::Center,
          .gap = Style::spaceXs * m_scale,
          .padding = Style::spaceSm * m_scale,
          .fill = leftoverFill(colorSpecFromRole(ColorRole::Surface)),
          .radius = Style::scaledRadiusXl(m_scale),
          .border = colorSpecFromRole(ColorRole::Outline, 0.28F),
          .borderWidth = 2.0F,
      });
      auto well = ui::column({
          .align = FlexAlign::Stretch,
          .justify = FlexJustify::Center,
          .gap = 0.0F,
          .fill = opaqueSpec(colorSpecFromRole(ColorRole::Surface)),
          .radius = Style::scaledRadiusMd(m_scale),
          .minHeight = wellHeight,
          .maxHeight = wellHeight,
          .fillWidth = true,
          .clipChildren = true,
          .height = wellHeight,
      });
      well->addChild(
          ui::glyph({
              .glyph = "palette",
              .glyphSize = Style::fontSizeTitle * m_scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
      auto accents = ui::row({
          .align = FlexAlign::Stretch,
          .gap = 0.0F,
          .minHeight = wellHeight,
          .maxHeight = wellHeight,
          .fillWidth = true,
          .visible = false,
          .participatesInLayout = false,
      });
      for (std::size_t i = 0; i < ColorSwatchPreviewStrip::kMaxSwatches; ++i) {
        accents->addChild(
            ui::row({
                .fill = opaqueSpec(colorSpecFromRole(ColorRole::Surface)),
                .fillHeight = true,
                .flexGrow = 1.0F,
            })
        );
      }
      well->addChild(std::move(accents));
      tile->addChild(std::move(well));
      tile->addChild(
          ui::label({
              .fontSize = Style::fontSizeCaption * m_scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
              .textAlign = TextAlign::Center,
              .ellipsize = TextEllipsize::End,
          })
      );
      return tile;
    }

    void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
      auto* flex = dynamic_cast<Flex*>(&tile);
      if (flex == nullptr || index >= m_catalog.labels.size()) {
        return;
      }
      auto* well = dynamic_cast<Flex*>(contentChild(*flex, 0));
      auto* name = dynamic_cast<Label*>(contentChild(*flex, 1));
      const ColorSpec fill = selected ? leftoverFill(colorSpecFromRole(ColorRole::Primary, 0.18F))
          : hovered                   ? leftoverFill(colorSpecFromRole(ColorRole::Hover))
                                      : leftoverFill(colorSpecFromRole(ColorRole::Surface));
      const ColorSpec border = selected ? colorSpecFromRole(ColorRole::Primary)
                                        : colorSpecFromRole(ColorRole::Outline, 0.28F);
      flex->setFill(fill);
      flex->setBorder(border, 2.0F);
      if (name != nullptr) {
        name->setText(m_catalog.labels[index]);
      }
      if (well == nullptr) {
        return;
      }
      auto* glyph = dynamic_cast<Glyph*>(contentChild(*well, 0));
      auto* accents = dynamic_cast<Flex*>(contentChild(*well, 1));
      const ColorSwatchPreview preview =
          index < m_catalog.previews.size() ? m_catalog.previews[index] : ColorSwatchPreview{};
      const bool hasSwatches = !preview.swatches.empty();
      well->setFill(opaqueSpec(preview.surface.value_or(colorSpecFromRole(ColorRole::SurfaceVariant))));
      well->setJustify(hasSwatches ? FlexJustify::Start : FlexJustify::Center);
      well->setAlign(hasSwatches ? FlexAlign::Stretch : FlexAlign::Center);
      if (glyph != nullptr) {
        glyph->setGlyph(m_fallbackGlyph);
        glyph->setVisible(!hasSwatches);
        glyph->setParticipatesInLayout(!hasSwatches);
      }
      if (accents != nullptr) {
        accents->setVisible(hasSwatches);
        accents->setParticipatesInLayout(hasSwatches);
        std::size_t colorIndex = 0;
        for (const auto& child : accents->children()) {
          auto* swatch = dynamic_cast<Flex*>(child.get());
          if (swatch == nullptr || child->zIndex() < 0 || child->type() == NodeType::Rect) {
            continue;
          }
          const bool shown = colorIndex < preview.swatches.size();
          swatch->setVisible(shown);
          swatch->setParticipatesInLayout(shown);
          if (shown) {
            swatch->setFill(opaqueSpec(preview.swatches[colorIndex]));
          }
          ++colorIndex;
        }
      }
    }

    [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
      if (index >= m_catalog.labels.size()) {
        return {};
      }
      return m_catalog.labels[index];
    }

  private:
    float m_scale = 1.0F;
    PaletteCatalog m_catalog;
    std::string m_fallbackGlyph = "palette";
  };

  void openControlCenterTab(std::string_view tab) {
    PanelManager::instance().togglePanel("control-center", PanelOpenRequest{.context = tab});
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

  [[nodiscard]] std::string colorHex(const Color& color) {
    const auto channel = [](float v) { return static_cast<int>(std::clamp(v, 0.0F, 1.0F) * 255.0F); };
    return std::format("#{:02x}{:02x}{:02x}", channel(color.r), channel(color.g), channel(color.b));
  }

} // namespace

ThemeTab::Section ThemeTab::sectionFromIndex(std::size_t index) noexcept {
  switch (index) {
  case 1:
    return Section::Chrome;
  case 2:
    return Section::Icons;
  case 3:
    return Section::Type;
  case 4:
    return Section::Effects;
  case 5:
    return Section::Language;
  case 0:
  default:
    return Section::Palettes;
  }
}

std::size_t ThemeTab::indexFromSection(Section section) noexcept {
  switch (section) {
  case Section::Chrome:
    return 1;
  case Section::Icons:
    return 2;
  case Section::Type:
    return 3;
  case Section::Effects:
    return 4;
  case Section::Language:
    return 5;
  case Section::Palettes:
  default:
    return 0;
  }
}

std::string_view ThemeTab::idFromSection(Section section) noexcept {
  switch (section) {
  case Section::Chrome:
    return "chrome";
  case Section::Icons:
    return "icons";
  case Section::Type:
    return "type";
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
  if (id == "chrome" || id == "glass" || id == "scale" || id == "roundness") {
    return Section::Chrome;
  }
  if (id == "icons") {
    return Section::Icons;
  }
  if (id == "type") {
    return Section::Type;
  }
  if (id == "effects") {
    return Section::Effects;
  }
  if (id == "language") {
    return Section::Language;
  }
  return Section::Palettes;
}

std::size_t ThemeTab::indexFromChromeCatalog(ChromeCatalog catalog) noexcept {
  switch (catalog) {
  case ChromeCatalog::Scale:
    return 1;
  case ChromeCatalog::Roundness:
    return 2;
  case ChromeCatalog::Glass:
  default:
    return 0;
  }
}

ThemeTab::ChromeCatalog ThemeTab::chromeCatalogFromIndex(std::size_t index) noexcept {
  switch (index) {
  case 1:
    return ChromeCatalog::Scale;
  case 2:
    return ChromeCatalog::Roundness;
  case 0:
  default:
    return ChromeCatalog::Glass;
  }
}

ThemeTab::ChromeCatalog ThemeTab::chromeCatalogFromId(std::string_view id) noexcept {
  if (id == "scale") {
    return ChromeCatalog::Scale;
  }
  if (id == "roundness") {
    return ChromeCatalog::Roundness;
  }
  return ChromeCatalog::Glass;
}

std::string_view ThemeTab::idFromChromeCatalog(ChromeCatalog catalog) noexcept {
  switch (catalog) {
  case ChromeCatalog::Scale:
    return "scale";
  case ChromeCatalog::Roundness:
    return "roundness";
  case ChromeCatalog::Glass:
  default:
    return "glass";
  }
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
      if (*section == "glass") {
        m_section = Section::Chrome;
        m_chromeCatalog = ChromeCatalog::Glass;
      } else if (*section == "scale") {
        m_section = Section::Chrome;
        m_chromeCatalog = ChromeCatalog::Scale;
      } else if (*section == "roundness") {
        m_section = Section::Chrome;
        m_chromeCatalog = ChromeCatalog::Roundness;
      } else {
        m_section = sectionFromId(*section);
      }
    }
    if (const std::optional<std::string> chrome = m_config->stateString("theme_tab", "chrome")) {
      m_chromeCatalog = chromeCatalogFromId(*chrome);
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
  m_paletteAdapter = std::make_unique<PaletteGridAdapter>(scale);

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto compactCard = ui::column({
      .configure =
          [scale, opacity](Flex& card) {
            applyCompactCardStyle(card, scale, opacity, false);
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
                      .tooltip = i18n::tr("settings.navigation.sections.chrome"),
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

  const bool palettesOpen = m_section == Section::Palettes;
  const bool chromeOpen = m_section == Section::Chrome;
  auto palettesToolbar = ui::column({
      .out = &m_palettesToolbar,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .visible = palettesOpen,
      .participatesInLayout = palettesOpen,
  });

  // No wrap: wrapped rows skip flexGrow, which pinned the palette search at its minimum width.
  auto browse = ui::row({
      .align = FlexAlign::Center,
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
      ui::segmented({
          .out = &m_modeSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = "",
                      .glyph = "moon",
                      .tooltip = i18n::tr("settings.options.theme.mode.dark"),
                  },
                  {
                      .label = "",
                      .glyph = "sun",
                      .tooltip = i18n::tr("settings.options.theme.mode.light"),
                  },
                  {
                      .label = "",
                      .glyph = "sun-moon",
                      .tooltip = i18n::tr("common.states.auto"),
                  },
              },
          .selectedIndex = modeIndex,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = opacity,
          .onChange = [this](std::size_t index) { applyThemeMode(index); },
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
          .glyphSize = Style::fontSizeBody * scale,
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
          .glyphSize = Style::fontSizeBody * scale,
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
          .glyphSize = Style::fontSizeBody * scale,
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
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
  });
  toolbar->addChild(std::move(browse));
  auto currentPick = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceXs * scale,
  });
  auto currentSwatches = std::make_unique<ColorSwatchPreviewStrip>();
  currentSwatches->setMetricsFromFontSize(captionSize);
  currentSwatches->setPreview(currentRolePreview());
  m_currentSwatches = currentSwatches.get();
  currentPick->addChild(std::move(currentSwatches));
  currentPick->addChild(
      ui::label({
          .out = &m_paletteName,
          .text = i18n::tr("control-center.appearance.palette"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      })
  );
  toolbar->addChild(std::move(currentPick));
  palettesToolbar->addChild(std::move(toolbar));
  palettesToolbar->addChild(
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
      .wrap = true,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
  });
  const float swatchSize = Style::controlHeightSm * scale;
  for (const auto& swatch : kPaletteSwatches) {
    const std::string roleLabel = i18n::tr(swatch.labelKey);
    heroWell->addChild(
        ui::button({
            .customPalette = roleSwatchPalette(swatch.role),
            .tooltip = i18n::tr("control-center.appearance.edit-role-tooltip", "role", roleLabel),
            .minWidth = swatchSize,
            .minHeight = swatchSize,
            .maxWidth = swatchSize,
            .maxHeight = swatchSize,
            .padding = 0.0F,
            .radius = swatchSize * 0.5F,
            .width = swatchSize,
            .height = swatchSize,
            .onClick = [this, role = swatch.role, roleLabel]() { pickRoleColor(role, roleLabel); },
        })
    );
  }
  palettesToolbar->addChild(std::move(heroWell));
  palettesToolbar->addChild(
      ui::label({
          .out = &m_exportStatus,
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  compactCard->addChild(std::move(palettesToolbar));

  auto chromeToolbar = ui::column({
      .out = &m_chromeToolbar,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .visible = chromeOpen,
      .participatesInLayout = chromeOpen,
  });
  chromeToolbar->addChild(
      ui::row(
          {
              .align = FlexAlign::Center,
              .wrap = true,
              .gap = Style::spaceXs * scale,
          },
          ui::segmented({
              .out = &m_chromeSwitch,
              .options =
                  std::vector<ui::SegmentedOption>{
                      {
                          .label = "",
                          .glyph = "glass",
                          .tooltip = i18n::tr("settings.navigation.sections.glass"),
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
                  },
              .selectedIndex = indexFromChromeCatalog(m_chromeCatalog),
              .scale = scale,
              .compact = true,
              .surfaceOpacity = opacity,
              .equalSegmentWidths = false,
              .onChange = [this](std::size_t index) { applyChromeCatalog(index); },
              .configure =
                  [](Segmented& sources) {
                    sources.setWrap(true);
                    sources.setFillWidth(false);
                  },
          })
      )
  );
  compactCard->addChild(std::move(chromeToolbar));
  Flex* compactHost = compactCard.get();
  tab->addChild(std::move(compactCard));

  auto palettesHost = ui::column({
      .out = &m_palettesHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .fillHeight = true,
      .flexGrow = 1.0F,
  });

  auto leftover = ui::column({
      .out = &m_palettesLeftover,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillHeight = true,
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
  leftover->addChild(
      ui::virtualGridView({
          .out = &m_paletteGrid,
          .contentScale = scale,
          .columns = 0,
          .minCellWidth = 148.0F * scale,
          .cellHeight = Style::controlHeightLg * 1.85F * scale,
          .squareCells = false,
          .columnGap = Style::spaceSm * scale,
          .rowGap = Style::spaceSm * scale,
          .scrollbarVisible = true,
          .adapter = m_paletteAdapter.get(),
          .flexGrow = 1.0F,
          .onSelectionChanged =
              [this](std::optional<std::size_t> index) {
                if (!index.has_value() || m_syncing || m_paletteAdapter == nullptr) {
                  return;
                }
                applyPaletteSelection(*index);
              },
          .configure =
              [](VirtualGridView& grid) {
                grid.scrollView().clearFill();
                grid.scrollView().clearBorder();
                grid.setFillWidth(true);
              },
      })
  );
  palettesHost->addChild(std::move(leftover));
  tab->addChild(std::move(palettesHost));

  auto addHost = [&]<typename Hosted>(Flex** out, Hosted* child, bool leftoverWell) {
    // Catalog sources keep their toolbar on the Theme compact card; only the leftover lands in the
    // host. Chrome editors have no toolbar of their own.
    auto makeBody = [&]() -> std::unique_ptr<Flex> {
      if constexpr (requires { child->createLiveBody(true, compactHost); }) {
        return child->createLiveBody(true, leftoverWell ? nullptr : compactHost);
      } else {
        return child->createLiveBody(true);
      }
    };
    std::unique_ptr<Flex> host;
    if (leftoverWell) {
      host = ui::column({
          .out = out,
          .align = FlexAlign::Stretch,
          .gap = 0.0F,
          .flexGrow = 1.0F,
          .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
      });
      auto scroll = ui::scrollView({
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
      auto* content = scroll->content();
      content->setDirection(FlexDirection::Vertical);
      content->setAlign(FlexAlign::Stretch);
      content->setGap(Style::spaceSm * scale);
      content->addChild(makeBody());
      host->addChild(std::move(scroll));
    } else {
      host = ui::column({
          .out = out,
          .align = FlexAlign::Stretch,
          .gap = 0.0F,
          .flexGrow = 1.0F,
      });
      host->addChild(makeBody());
    }
    tab->addChild(std::move(host));
  };
  addHost(&m_glassHost, m_glass.get(), true);
  addHost(&m_scaleHost, m_scale.get(), true);
  addHost(&m_roundnessHost, m_roundness.get(), true);
  addHost(&m_iconsHost, m_icons.get(), false);
  addHost(&m_typeHost, m_type.get(), false);
  addHost(&m_effectsHost, m_effects.get(), false);
  addHost(&m_languageHost, m_language.get(), false);

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
  m_palettesToolbar = nullptr;
  m_chromeToolbar = nullptr;
  m_chromeSwitch = nullptr;
  m_palettesHost = nullptr;
  m_palettesLeftover = nullptr;
  m_currentSwatches = nullptr;
  m_glassHost = nullptr;
  m_iconsHost = nullptr;
  m_typeHost = nullptr;
  m_scaleHost = nullptr;
  m_roundnessHost = nullptr;
  m_effectsHost = nullptr;
  m_languageHost = nullptr;
  m_modeSwitch = nullptr;
  m_autoStatus = nullptr;
  m_paletteName = nullptr;
  m_sourceSwitch = nullptr;
  m_paletteFilter = nullptr;
  m_catalogToolbar = nullptr;
  m_paletteGrid = nullptr;
  m_paletteAdapter.reset();
  m_paletteEmpty = nullptr;
  m_openWallpaperButton = nullptr;
  m_openPalettesButton = nullptr;
  m_saveCustomButton = nullptr;
  m_exportStatus = nullptr;
  m_sectionSyncing = false;
  m_chromeSyncing = false;
}

void ThemeTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool ThemeTab::configureMode() const { return m_settings->configureMode(); }

void ThemeTab::setSettingsSearchQuery(std::string_view query) {
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* ThemeTab::settingsPane() { return m_settings.get(); }

void ThemeTab::setActive(bool active) {
  const bool chrome = active && m_section == Section::Chrome;
  if (m_glass != nullptr) {
    m_glass->setActive(chrome && m_chromeCatalog == ChromeCatalog::Glass);
  }
  if (m_icons != nullptr) {
    m_icons->setActive(active && m_section == Section::Icons);
  }
  if (m_type != nullptr) {
    m_type->setActive(active && m_section == Section::Type);
  }
  if (m_scale != nullptr) {
    m_scale->setActive(chrome && m_chromeCatalog == ChromeCatalog::Scale);
  }
  if (m_roundness != nullptr) {
    m_roundness->setActive(chrome && m_chromeCatalog == ChromeCatalog::Roundness);
  }
  if (m_effects != nullptr) {
    m_effects->setActive(active && m_section == Section::Effects);
  }
  if (m_language != nullptr) {
    m_language->setActive(active && m_section == Section::Language);
  }
}

void ThemeTab::applyOpenSection(Section section, bool persist) {
  if (m_section != section) {
    m_section = section;
    if (persist) {
      persistSection();
      if (m_section == Section::Chrome) {
        persistChrome();
      }
    }
    if (m_sectionSwitch != nullptr) {
      m_sectionSyncing = true;
      m_sectionSwitch->setSelectedIndex(indexFromSection(m_section));
      m_sectionSyncing = false;
    }
  }
  syncHosts();
  syncSettingsSection();
  setActive(true);
  PanelManager::instance().requestLayout();
}

void ThemeTab::applyOpenContext(std::string_view context, bool persist) {
  if (context.empty() || context == "appearance" || context == "theme") {
    return;
  }
  ChromeCatalog catalog = m_chromeCatalog;
  if (context == "glass") {
    catalog = ChromeCatalog::Glass;
  } else if (context == "scale") {
    catalog = ChromeCatalog::Scale;
  } else if (context == "roundness") {
    catalog = ChromeCatalog::Roundness;
  }
  const bool catalogChanged = catalog != m_chromeCatalog;
  m_chromeCatalog = catalog;
  if (catalogChanged) {
    if (persist) {
      persistChrome();
    }
    if (m_chromeSwitch != nullptr) {
      m_chromeSyncing = true;
      m_chromeSwitch->setSelectedIndex(indexFromChromeCatalog(m_chromeCatalog));
      m_chromeSyncing = false;
    }
  }
  applyOpenSection(sectionFromId(context), persist);
}

bool ThemeTab::matchesOpenContext(std::string_view context) const noexcept {
  if (context == "glass") {
    return m_section == Section::Chrome && m_chromeCatalog == ChromeCatalog::Glass;
  }
  if (context == "scale") {
    return m_section == Section::Chrome && m_chromeCatalog == ChromeCatalog::Scale;
  }
  if (context == "roundness") {
    return m_section == Section::Chrome && m_chromeCatalog == ChromeCatalog::Roundness;
  }
  if (context == "chrome") {
    return m_section == Section::Chrome;
  }
  if (context == "palettes" || context == "theme" || context == "appearance") {
    return m_section == Section::Palettes;
  }
  return m_section == sectionFromId(context);
}

void ThemeTab::applySection(std::size_t index) {
  if (m_sectionSyncing) {
    return;
  }
  applyOpenSection(sectionFromIndex(index), true);
}

void ThemeTab::applyChromeCatalog(std::size_t index) {
  if (m_chromeSyncing) {
    return;
  }
  const ChromeCatalog catalog = chromeCatalogFromIndex(index);
  if (catalog == m_chromeCatalog) {
    return;
  }
  m_chromeCatalog = catalog;
  persistChrome();
  syncHosts();
  syncSettingsSection();
  setActive(true);
  PanelManager::instance().requestLayout();
}

void ThemeTab::persistSection() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("theme_tab", "section", std::string(idFromSection(m_section)));
}

void ThemeTab::persistChrome() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("theme_tab", "chrome", std::string(idFromChromeCatalog(m_chromeCatalog)));
}

void ThemeTab::syncHosts() {
  const bool palettes = m_section == Section::Palettes;
  const bool chrome = m_section == Section::Chrome;
  setShown(m_palettesToolbar, palettes);
  setShown(m_chromeToolbar, chrome);
  setShown(m_palettesHost, palettes);
  setShown(m_glassHost, chrome && m_chromeCatalog == ChromeCatalog::Glass);
  setShown(m_scaleHost, chrome && m_chromeCatalog == ChromeCatalog::Scale);
  setShown(m_roundnessHost, chrome && m_chromeCatalog == ChromeCatalog::Roundness);
  setShown(m_iconsHost, m_section == Section::Icons);
  setShown(m_typeHost, m_section == Section::Type);
  setShown(m_effectsHost, m_section == Section::Effects);
  setShown(m_languageHost, m_section == Section::Language);
  if (m_icons != nullptr) {
    setShown(m_icons->hostedCompact(), m_section == Section::Icons);
  }
  if (m_type != nullptr) {
    setShown(m_type->hostedCompact(), m_section == Section::Type);
  }
  if (m_effects != nullptr) {
    setShown(m_effects->hostedCompact(), m_section == Section::Effects);
  }
  if (m_language != nullptr) {
    setShown(m_language->hostedCompact(), m_section == Section::Language);
  }
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
  if (m_section == Section::Chrome) {
    m_settings->setSection(std::string(idFromChromeCatalog(m_chromeCatalog)));
    m_settings->setIncludedGroups({});
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
  case Section::Chrome:
    switch (m_chromeCatalog) {
    case ChromeCatalog::Scale:
      return m_scale.get();
    case ChromeCatalog::Roundness:
      return m_roundness.get();
    case ChromeCatalog::Glass:
    default:
      return m_glass.get();
    }
  case Section::Icons:
    return m_icons.get();
  case Section::Type:
    return m_type.get();
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
  case Section::Chrome:
    switch (m_chromeCatalog) {
    case ChromeCatalog::Scale:
      return m_scaleHost;
    case ChromeCatalog::Roundness:
      return m_roundnessHost;
    case ChromeCatalog::Glass:
    default:
      return m_glassHost;
    }
  case Section::Icons:
    return m_iconsHost;
  case Section::Type:
    return m_typeHost;
  case Section::Effects:
    return m_effectsHost;
  case Section::Language:
    return m_languageHost;
  case Section::Palettes:
  default:
    return m_palettesHost;
  }
}

void ThemeTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  if (Tab* child = hostedTab(); child != nullptr) {
    Flex* host = hostNode();
    float hostWidth = contentWidth;
    float hostHeight = bodyHeight;
    if (host != nullptr && host->width() > 1.0F) {
      hostWidth = std::max(1.0F, host->width() - host->paddingLeft() - host->paddingRight());
    }
    if (host != nullptr && host->height() > 1.0F) {
      hostHeight = std::max(1.0F, host->height() - host->paddingTop() - host->paddingBottom());
    }
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
  if (m_modeSwitch != nullptr) {
    m_modeSwitch->setSurfaceOpacity(opacity);
  }
  if (m_chromeSwitch != nullptr) {
    m_chromeSwitch->setSurfaceOpacity(opacity);
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
  if (m_modeSwitch != nullptr) {
    m_modeSwitch->setSelectedIndex(modeIndex);
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
  if (m_paletteGrid == nullptr || m_paletteAdapter == nullptr || m_config == nullptr) {
    return;
  }
  uiAssertNotRendering("ThemeTab::rebuildPaletteGrid");

  const ThemeConfig& theme = m_config->config().theme;
  const PaletteSource source = selectedPaletteSource();
  PaletteCatalog catalog = filterPaletteCatalog(buildPaletteCatalog(source, shellThemeMode(theme), m_config), m_paletteQuery);
  const std::string selectedValue = paletteValueForSource(source);

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
  if (m_currentSwatches != nullptr) {
    if (selectedIndex < catalog.previews.size() && !catalog.previews[selectedIndex].empty()) {
      m_currentSwatches->setPreview(catalog.previews[selectedIndex]);
    } else {
      m_currentSwatches->setPreview(currentRolePreview());
    }
  }

  const bool emptyCatalog = catalog.labels.empty();
  setShown(m_paletteEmpty, emptyCatalog);
  m_paletteGrid->setVisible(!emptyCatalog);
  m_paletteGrid->setParticipatesInLayout(!emptyCatalog);
  setShown(m_openPalettesButton, source == PaletteSource::Custom);
  setShown(m_openWallpaperButton, source == PaletteSource::Wallpaper);
  setShown(m_paletteFilter, true);
  setShown(m_paletteName, true);

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

  m_paletteValues = catalog.values;
  auto* adapter = static_cast<PaletteGridAdapter*>(m_paletteAdapter.get());
  adapter->setCatalog(std::move(catalog), source == PaletteSource::Wallpaper ? "photo" : "palette");
  const std::optional<std::size_t> selected =
      selectedIndex < m_paletteValues.size() ? std::optional<std::size_t>{selectedIndex} : std::nullopt;
  const bool wasSyncing = m_syncing;
  m_syncing = true;
  if (m_paletteGrid->selectedIndex() != selected) {
    m_paletteGrid->setSelectedIndex(selected);
  }
  m_syncing = wasSyncing;
  m_paletteGrid->notifyDataChanged();
}

void ThemeTab::applyThemeMode(std::size_t index) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  m_config->setThemeMode(themeModeFromIndex(index));
  rebuildPaletteGrid();
  updateAutoStatus();
  PanelManager::instance().refresh();
}

void ThemeTab::applyPaletteSource(std::size_t index) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  m_paletteQuery.clear();
  if (m_paletteFilter != nullptr) {
    m_paletteFilter->setValue("");
  }
  const PaletteSource source = paletteSourceFromIndex(index);
  const PaletteCatalog catalog =
      filterPaletteCatalog(buildPaletteCatalog(source, shellThemeMode(m_config->config().theme), m_config), m_paletteQuery);
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
