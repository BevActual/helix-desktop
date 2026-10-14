// Emits core/helix-theme/tests/golden/palette_expand.tsv from the C++ reference.
// Build: see tools/regen_golden.sh
#include "core/color.h"
#include "theme/fixed_palette.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace helix::theme;

namespace {
  std::string hexOf(const ::Color& c) {
    auto b = [](float v) { return static_cast<int>(std::clamp(v, 0.0F, 1.0F) * 255.0F + 0.5F); };
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", b(c.r), b(c.g), b(c.b));
    return buf;
  }
} // namespace

int main() {
  unsigned state = 424242;
  auto next = [&] {
    state = state * 1664525U + 1013904223U;
    return (state >> 8) & 255U;
  };
  for (int i = 0; i < 120; ++i) {
    char in[16][8];
    ::Palette p{};
    ::Color* fields[16] = {&p.primary,   &p.onPrimary,        &p.secondary, &p.onSecondary,
                           &p.tertiary,  &p.onTertiary,       &p.error,     &p.onError,
                           &p.surface,   &p.onSurface,        &p.surfaceVariant, &p.onSurfaceVariant,
                           &p.outline,   &p.shadow,           &p.hover,     &p.onHover};
    std::string line;
    for (int k = 0; k < 16; ++k) {
      // First few palettes use extreme/degenerate values to hit clamp branches.
      unsigned r = next(), g = next(), b = next();
      if (i == 0) r = g = b = 0;
      if (i == 1) r = g = b = 255;
      if (i == 2) r = g = b = 128;
      std::snprintf(in[k], sizeof(in[k]), "#%02x%02x%02x", r, g, b);
      *fields[k] = hex(in[k]);
      line += in[k];
      line += k == 15 ? "" : ",";
    }
    for (const bool dark : {true, false}) {
      TokenMap tokens = expandFixedPaletteMode(p, dark);
      synthesizeTerminalPaletteTokens(tokens);
      const std::map<std::string, uint32_t> sorted(tokens.begin(), tokens.end());
      std::printf("%s\t%d", line.c_str(), dark ? 1 : 0);
      for (const auto& [k, v] : sorted)
        std::printf("\t%s=#%06x", k.c_str(), v & 0xffffffU);
      const ::Palette m = mapGeneratedPaletteMode(tokens);
      const ::Color* mf[16] = {&m.primary, &m.onPrimary, &m.secondary, &m.onSecondary, &m.tertiary, &m.onTertiary,
                               &m.error,   &m.onError,   &m.surface,   &m.onSurface,   &m.surfaceVariant,
                               &m.onSurfaceVariant, &m.outline, &m.shadow, &m.hover, &m.onHover};
      std::printf("\tmapped=");
      for (int k = 0; k < 16; ++k)
        std::printf("%s%s", hexOf(*mf[k]).c_str(), k == 15 ? "" : ",");
      std::putchar('\n');
    }
  }
}
