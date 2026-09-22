#pragma once

#include "config/config_service.h"
#include "config/config_types.h"
#include "shell/surface/shadow.h"
#include "ui/style.h"

#include <algorithm>
#include <cstdint>
#include <optional>

// Sizing and styling shared by the panel surfaces: PanelManager's single active
// panel and PersistentPanelHost's long-lived ones.
namespace shell::panel_surface {

  // Extra margin around the shadow bleed. The shadow is drawn inside the surface,
  // so a surface sized to the bleed alone clips its own outermost falloff row.
  inline constexpr std::int32_t kShadowSafetyPadding = 2;

  [[nodiscard]] inline shell::surface_shadow::Bleed
  bleed(bool hasDecoration, const ShellConfig::ShadowConfig& shadow) noexcept {
    auto out = shell::surface_shadow::bleed(hasDecoration, shadow);
    if (shell::surface_shadow::enabled(hasDecoration, shadow)) {
      out.left += kShadowSafetyPadding;
      out.right += kShadowSafetyPadding;
      out.up += kShadowSafetyPadding;
      out.down += kShadowSafetyPadding;
    }
    return out;
  }

  [[nodiscard]] inline std::uint32_t
  surfaceExtent(std::uint32_t contentSize, std::int32_t before, std::int32_t after) noexcept {
    const auto total =
        static_cast<std::int64_t>(contentSize) + static_cast<std::int64_t>(before) + static_cast<std::int64_t>(after);
    return static_cast<std::uint32_t>(std::max<std::int64_t>(1, total));
  }

  [[nodiscard]] inline float contentScale(const ConfigService* configService) noexcept {
    if (configService == nullptr) {
      return 1.0F;
    }
    return std::max(0.1F, configService->config().accessibility.uiScale);
  }

  // While the Chrome slider is being dragged the chrome follows the thumb instead of the saved
  // value, so the choice is made on what it looks like rather than on a percentage. Cleared on
  // release, when the value is committed to config and the compositor.
  inline std::optional<float> g_glassPreview;
  inline void setGlassPreview(std::optional<float> glass) noexcept { g_glassPreview = glass; }
  [[nodiscard]] inline std::optional<float> glassPreview() noexcept { return g_glassPreview; }

  [[nodiscard]] inline float glass(const ConfigService* configService) noexcept {
    if (g_glassPreview.has_value()) {
      return std::clamp(*g_glassPreview, 0.0F, 1.0F);
    }
    return configService != nullptr ? resolvedPanelGlass(configService->config().shell.panel) : 0.0F;
  }

  [[nodiscard]] inline float backgroundOpacity(const ConfigService* configService) noexcept {
    if (Style::highContrastEnabled()) {
      return 1.0F;
    }
    return detachedPanelBackgroundOpacityForGlass(glass(configService));
  }

  [[nodiscard]] inline float cardOpacity(const ConfigService* configService, float panelBackgroundOpacity) noexcept {
    if (Style::highContrastEnabled()) {
      return 1.0F;
    }
    return panelCardOpacityForGlass(glass(configService), panelBackgroundOpacity);
  }

  [[nodiscard]] inline bool chromeBorderEnabled(bool panelBorders) noexcept {
    return panelBorders || Style::highContrastEnabled();
  }

  [[nodiscard]] inline float chromeBorderWidth() noexcept {
    return Style::highContrastEnabled() ? Style::emphasizedBorderWidth : Style::borderWidth;
  }

} // namespace shell::panel_surface
