#pragma once

#include "core/color.h"
#include "theme/color_roles.h"
#include "ui/signal.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

extern Palette palette;

[[nodiscard]] const Color& colorForRole(ColorRole role) noexcept;
[[nodiscard]] Color colorForRole(ColorRole role, float alpha) noexcept;
// A fixed hue pulled up to 15° toward the current primary (Material harmonize) so system colors
// (blue for connectivity, green for power, red for alerts) sit in the wallpaper palette's family
// without losing their meaning. Resolved against the palette at call time.
[[nodiscard]] Color harmonizeToPrimary(const Color& fixed) noexcept;
[[nodiscard]] Color resolveColorSpec(const ColorSpec& color) noexcept;
[[nodiscard]] bool isLightPalette() noexcept;
// Set by ThemeService from resolved light/dark mode (not inferred from palette colors).
[[nodiscard]] bool isResolvedLightTheme() noexcept;
void setResolvedThemeLight(bool light) noexcept;

void setPalette(const Palette& p);

// Snapshot accessor safe to call from non-main threads (e.g. plugin script workers).
[[nodiscard]] Color colorForRoleSnapshot(ColorRole role);

[[nodiscard]] inline ColorSpec scrollbarTrackColor() noexcept {
  return colorSpecFromRole(ColorRole::Outline, Style::disabledOutlineAlpha);
}
[[nodiscard]] inline ColorSpec scrollbarThumbColor() noexcept {
  return colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.5F);
}
[[nodiscard]] inline ColorSpec focusRingColorSpec() noexcept { return colorSpecFromRole(ColorRole::Primary); }

// Fired after setPalette() writes. Controls subscribe in their constructor
// and re-apply palette-derived colors to their scene nodes on each emit.
Signal<>& paletteChanged();

// Linearly interpolates each field of two palettes in sRGB space. Used by
// ThemeService to drive smooth cross-fade transitions on theme changes.
Palette lerpPalette(const Palette& a, const Palette& b, float t);
