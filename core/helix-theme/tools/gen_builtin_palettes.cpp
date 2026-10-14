// Emits core/helix-theme/data/builtin_palettes.json from src/theme/builtin_palettes.cpp.
#include "core/color.h"
#include "theme/builtin_palettes.h"

#include <algorithm>
#include <cstdio>
#include <string>

using namespace helix::theme;

namespace {
  std::string hexOf(const ::Color& c) {
    auto b = [](float v) { return static_cast<int>(std::clamp(v, 0.0F, 1.0F) * 255.0F + 0.5F); };
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", b(c.r), b(c.g), b(c.b));
    return buf;
  }

  void field(const char* indent, const char* key, const ::Color& c, bool last = false) {
    std::printf("%s\"%s\": \"%s\"%s\n", indent, key, hexOf(c).c_str(), last ? "" : ",");
  }

  void ansi(const char* indent, const char* key, const TerminalAnsiColors& a, bool last) {
    std::printf("%s\"%s\": {\n", indent, key);
    field("        ", "black", a.black);
    field("        ", "red", a.red);
    field("        ", "green", a.green);
    field("        ", "yellow", a.yellow);
    field("        ", "blue", a.blue);
    field("        ", "magenta", a.magenta);
    field("        ", "cyan", a.cyan);
    field("        ", "white", a.white, true);
    std::printf("%s}%s\n", indent, last ? "" : ",");
  }

  void mode(const char* key, const FixedPaletteMode& m, bool last) {
    const auto& p = m.palette;
    const auto& t = m.terminal;
    std::printf("    \"%s\": {\n", key);
    field("      ", "primary", p.primary);
    field("      ", "onPrimary", p.onPrimary);
    field("      ", "secondary", p.secondary);
    field("      ", "onSecondary", p.onSecondary);
    field("      ", "tertiary", p.tertiary);
    field("      ", "onTertiary", p.onTertiary);
    field("      ", "error", p.error);
    field("      ", "onError", p.onError);
    field("      ", "surface", p.surface);
    field("      ", "onSurface", p.onSurface);
    field("      ", "surfaceVariant", p.surfaceVariant);
    field("      ", "onSurfaceVariant", p.onSurfaceVariant);
    field("      ", "outline", p.outline);
    field("      ", "shadow", p.shadow);
    field("      ", "hover", p.hover);
    field("      ", "onHover", p.onHover);
    std::printf("      \"terminal\": {\n");
    ansi("        ", "normal", t.normal, false);
    ansi("        ", "bright", t.bright, false);
    field("        ", "foreground", t.foreground);
    field("        ", "background", t.background);
    field("        ", "cursor", t.cursor);
    field("        ", "cursorText", t.cursorText);
    field("        ", "selectionFg", t.selectionFg);
    field("        ", "selectionBg", t.selectionBg, true);
    std::printf("      }\n    }%s\n", last ? "" : ",");
  }
} // namespace

int main() {
  const auto all = builtinPalettes();
  std::puts("[");
  for (std::size_t i = 0; i < all.size(); ++i) {
    std::printf("  {\n    \"name\": \"%.*s\",\n", static_cast<int>(all[i].name.size()), all[i].name.data());
    mode("dark", all[i].dark, false);
    mode("light", all[i].light, true);
    std::printf("  }%s\n", i + 1 == all.size() ? "" : ",");
  }
  std::puts("]");
}
