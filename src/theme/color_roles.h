#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// The named colour roles and their config/template tokens. Plain data, so the desktop service and apps can use it
// without the shell's UI toolkit; ui/palette.h includes it and adds the rendering types.
enum class ColorRole : std::uint8_t {
  Primary,
  OnPrimary,
  Secondary,
  OnSecondary,
  Tertiary,
  OnTertiary,
  Error,
  OnError,
  Surface,
  OnSurface,
  SurfaceVariant,
  OnSurfaceVariant,
  Outline,
  Shadow,
  Hover,
  OnHover,
};

struct ColorRoleToken {
  ColorRole role;
  std::string_view token;
};

inline constexpr std::array<ColorRoleToken, 16> kColorRoleTokens = {{
    {ColorRole::Primary, "primary"},
    {ColorRole::OnPrimary, "on_primary"},
    {ColorRole::Secondary, "secondary"},
    {ColorRole::OnSecondary, "on_secondary"},
    {ColorRole::Tertiary, "tertiary"},
    {ColorRole::OnTertiary, "on_tertiary"},
    {ColorRole::Error, "error"},
    {ColorRole::OnError, "on_error"},
    {ColorRole::Surface, "surface"},
    {ColorRole::OnSurface, "on_surface"},
    {ColorRole::SurfaceVariant, "surface_variant"},
    {ColorRole::OnSurfaceVariant, "on_surface_variant"},
    {ColorRole::Outline, "outline"},
    {ColorRole::Shadow, "shadow"},
    {ColorRole::Hover, "hover"},
    {ColorRole::OnHover, "on_hover"},
}};
