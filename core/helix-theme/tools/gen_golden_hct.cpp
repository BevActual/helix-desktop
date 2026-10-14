// Emits tests/golden/hct.tsv from material_color_utilities for helix-theme parity.
#include "cpp/cam/hct.h"
#include "cpp/palettes/tones.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace material_color_utilities;

int main() {
  std::vector<uint32_t> colors;
  for (int r = 0; r <= 255; r += 51)
    for (int g = 0; g <= 255; g += 51)
      for (int b = 0; b <= 255; b += 51)
        colors.push_back(0xff000000U | (r << 16) | (g << 8) | b);
  unsigned state = 777;
  for (int i = 0; i < 300; ++i) {
    state = state * 1664525U + 1013904223U;
    colors.push_back(0xff000000U | ((state >> 8) & 0xffffffU));
  }
  const double tones[] = {0.0, 5.0, 10.0, 25.5, 40.0, 50.0, 60.0, 80.0, 90.0, 99.99999, 100.0};
  const double chromas[] = {0.0, 8.0, 36.0, 80.0, 200.0};
  const int paletteTones[] = {0, 5, 10, 15, 20, 25, 30, 35, 40, 50, 60, 70, 80, 90, 95, 98, 99, 100};
  for (const uint32_t argb : colors) {
    Hct hct(argb);
    std::printf("%08x\t%.9f\t%.9f\t%.9f", argb, hct.get_hue(), hct.get_chroma(), hct.get_tone());
    for (double t : tones) {
      Hct h(argb);
      h.set_tone(t);
      std::printf("\t%08x", h.ToInt());
    }
    for (double c : chromas) {
      Hct h(argb);
      h.set_chroma(c);
      std::printf("\t%08x", h.ToInt());
    }
    {
      Hct h(argb);
      h.set_hue(h.get_hue() + 90.0);
      std::printf("\t%08x", h.ToInt());
    }
    TonalPalette palette(argb);
    for (int t : paletteTones)
      std::printf("\t%08x", palette.get(static_cast<double>(t)));
    std::putchar('\n');
  }
}
