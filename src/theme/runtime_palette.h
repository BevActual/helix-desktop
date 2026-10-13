#pragma once

#include "theme/color_roles.h"
#include "theme/palette.h"

#include <functional>

// Platform-owned current palette (no UI toolkit dependency).
extern Palette palette;

[[nodiscard]] bool isResolvedLightTheme() noexcept;
void setResolvedThemeLight(bool light) noexcept;
void setPalette(const Palette& p);

// Optional listeners (templates / services). No UI Signal type.
using PaletteChangedFn = std::function<void()>;
void addPaletteChangedListener(PaletteChangedFn fn);

Palette lerpPalette(const Palette& a, const Palette& b, float t);
