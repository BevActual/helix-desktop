// Emits core/helix-theme/tests/golden/color_contrast.tsv from the C++ reference.
// Build: g++ -std=c++20 -I src core/helix-theme/tools/gen_golden.cpp src/theme/color.cpp src/theme/contrast.cpp
#include "theme/color.h"
#include "theme/contrast.h"

#include <cstdio>
#include <vector>

using namespace helix::theme;

int main() {
  std::vector<Color> colors;
  for (int r = 0; r <= 255; r += 51)
    for (int g = 0; g <= 255; g += 51)
      for (int b = 0; b <= 255; b += 51)
        colors.emplace_back(r, g, b);
  unsigned state = 12345;
  for (int i = 0; i < 400; ++i) {
    state = state * 1664525U + 1013904223U;
    const int r = (state >> 8) & 255;
    state = state * 1664525U + 1013904223U;
    const int g = (state >> 8) & 255;
    state = state * 1664525U + 1013904223U;
    const int b = (state >> 8) & 255;
    colors.emplace_back(r, g, b);
  }
  const Color backgrounds[] = {Color(0, 0, 0), Color(255, 255, 255), Color(0x20, 0x20, 0x28), Color(0xe8, 0xe0, 0xd0),
                               Color(0x80, 0x80, 0x80)};
  std::puts("# hex\th\ts\tl\tfromhsl\tec[bg0..4]@4.5\tec[bg0..4]@3.0");
  for (const auto& c : colors) {
    const auto [h, s, l] = c.toHsl();
    std::printf("%s\t%.12f\t%.12f\t%.12f\t%s", c.toHex().c_str(), h, s, l, Color::fromHsl(h, s, l).toHex().c_str());
    for (const double ratio : {4.5, 3.0})
      for (const auto& bg : backgrounds)
        std::printf("\t%s", ensureContrast(c, bg, ratio).toHex().c_str());
    std::putchar('\n');
  }
}
