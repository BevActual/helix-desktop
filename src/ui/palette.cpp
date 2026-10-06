#include "ui/palette.h"

#include "cpp/blend/blend.h"
#include "core/color.h"
#include "theme/builtin_palettes.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>

Palette palette = noctalia::theme::findBuiltinPalette("Noctalia")->dark.palette;
bool g_resolvedThemeLight = false;

namespace {

  // Serializes setPalette() writes against colorForRoleSnapshot() reads from the
  // script runtime thread. All other palette reads (colorForRole, rendering) stay
  // on the main thread, so this holds only while setPalette() is main-thread-only.
  std::mutex g_paletteMutex;

  // Hover is a state, not a hue: a neutral surface tinted toward the accent, with normal
  // text on top. palette.hover stays as authored so templates export it unchanged; only the
  // shell's own Hover/OnHover roles resolve to the derived pair. Cached on the inputs so
  // palette animations and direct palette writes stay in sync.
  constexpr float kHoverAccentDark = 0.22F;
  constexpr float kHoverAccentLight = 0.16F;

  struct UiHover {
    Color surfaceVariant;
    Color primary;
    Color onSurface;
    bool light = false;
    bool valid = false;
    Color hover;
    Color onHover;
  };

  const UiHover& uiHover() noexcept {
    static UiHover cache;
    if (!cache.valid || cache.surfaceVariant != palette.surfaceVariant || cache.primary != palette.primary
        || cache.onSurface != palette.onSurface || cache.light != g_resolvedThemeLight) {
      cache.surfaceVariant = palette.surfaceVariant;
      cache.primary = palette.primary;
      cache.onSurface = palette.onSurface;
      cache.light = g_resolvedThemeLight;
      cache.hover = lerpColor(
          palette.surfaceVariant, palette.primary, g_resolvedThemeLight ? kHoverAccentLight : kHoverAccentDark
      );
      cache.hover.a = 1.0F;
      cache.onHover = palette.onSurface;
      cache.valid = true;
    }
    return cache;
  }

} // namespace

const Color& colorForRole(ColorRole role) noexcept {
  switch (role) {
  case ColorRole::Primary:
    return palette.primary;
  case ColorRole::OnPrimary:
    return palette.onPrimary;
  case ColorRole::Secondary:
    return palette.secondary;
  case ColorRole::OnSecondary:
    return palette.onSecondary;
  case ColorRole::Tertiary:
    return palette.tertiary;
  case ColorRole::OnTertiary:
    return palette.onTertiary;
  case ColorRole::Error:
    return palette.error;
  case ColorRole::OnError:
    return palette.onError;
  case ColorRole::Surface:
    return palette.surface;
  case ColorRole::OnSurface:
    return palette.onSurface;
  case ColorRole::SurfaceVariant:
    return palette.surfaceVariant;
  case ColorRole::OnSurfaceVariant:
    return palette.onSurfaceVariant;
  case ColorRole::Outline:
    return palette.outline;
  case ColorRole::Shadow:
    return palette.shadow;
  case ColorRole::Hover:
    return uiHover().hover;
  case ColorRole::OnHover:
    return uiHover().onHover;
  }

  return palette.onSurface;
}

namespace {
  std::uint32_t argbOf(const Color& c) noexcept {
    const auto ch = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F)); };
    return (0xFFU << 24U) | (ch(c.r) << 16U) | (ch(c.g) << 8U) | ch(c.b);
  }
} // namespace

Color harmonizeToPrimary(const Color& fixed) noexcept {
  const std::uint32_t out = material_color_utilities::BlendHarmonize(argbOf(fixed), argbOf(colorForRole(ColorRole::Primary, 1.0F)));
  return Color{
      .r = colorByte((out >> 16U) & 0xFFU), .g = colorByte((out >> 8U) & 0xFFU), .b = colorByte(out & 0xFFU), .a = fixed.a
  };
}

Color colorForRole(ColorRole role, float alpha) noexcept {
  Color color = colorForRole(role);
  color.a *= alpha;
  return color;
}

Color resolveColorSpec(const ColorSpec& color) noexcept {
  Color resolved = color.role.has_value() ? colorForRole(*color.role) : color.fixed;
  resolved.a *= color.alpha;
  return resolved;
}

bool isLightPalette() noexcept { return relativeLuminance(palette.surface) > 0.179F; }

bool isResolvedLightTheme() noexcept { return g_resolvedThemeLight; }

void setResolvedThemeLight(bool light) noexcept { g_resolvedThemeLight = light; }

Signal<>& paletteChanged() {
  static Signal<> signal;
  return signal;
}

void setPalette(const Palette& p) {
  {
    std::scoped_lock lock(g_paletteMutex);
    if (palette == p) {
      return;
    }
    palette = p;
  }
  paletteChanged().emit();
}

Color colorForRoleSnapshot(ColorRole role) {
  std::scoped_lock lock(g_paletteMutex);
  return colorForRole(role);
}

Palette lerpPalette(const Palette& a, const Palette& b, float t) {
  return Palette{
      .primary = lerpColor(a.primary, b.primary, t),
      .onPrimary = lerpColor(a.onPrimary, b.onPrimary, t),
      .secondary = lerpColor(a.secondary, b.secondary, t),
      .onSecondary = lerpColor(a.onSecondary, b.onSecondary, t),
      .tertiary = lerpColor(a.tertiary, b.tertiary, t),
      .onTertiary = lerpColor(a.onTertiary, b.onTertiary, t),
      .error = lerpColor(a.error, b.error, t),
      .onError = lerpColor(a.onError, b.onError, t),
      .surface = lerpColor(a.surface, b.surface, t),
      .onSurface = lerpColor(a.onSurface, b.onSurface, t),
      .surfaceVariant = lerpColor(a.surfaceVariant, b.surfaceVariant, t),
      .onSurfaceVariant = lerpColor(a.onSurfaceVariant, b.onSurfaceVariant, t),
      .outline = lerpColor(a.outline, b.outline, t),
      .shadow = lerpColor(a.shadow, b.shadow, t),
      .hover = lerpColor(a.hover, b.hover, t),
      .onHover = lerpColor(a.onHover, b.onHover, t),
  };
}
