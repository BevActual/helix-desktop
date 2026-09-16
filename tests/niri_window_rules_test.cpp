#include "compositors/niri/niri_window_rules.h"

#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "niri_window_rules_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

  std::filesystem::path makeTempDir() {
    auto path = std::filesystem::temp_directory_path() / ("noctalia-niri-window-rules-" + std::to_string(::getpid()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
  }

} // namespace

int main() {
  bool ok = true;

  const auto radiusOnly = compositors::niri::kdlForWindowCornerRadius(20);
  ok = expect(radiusOnly.find("geometry-corner-radius 20") != std::string::npos, "writes corner radius") && ok;
  ok = expect(radiusOnly.find("clip-to-geometry true") != std::string::npos, "clips to geometry") && ok;
  ok = expect(
      radiusOnly.find("draw-border-with-background") == std::string::npos, "omits draw-border until set"
  ) && ok;

  const auto extras = compositors::niri::kdlForWindowRules({
      .cornerRadius = 12,
      .drawBorderWithBackground = false,
  });
  ok = expect(extras.find("geometry-corner-radius 12") != std::string::npos, "keeps radius with extras") && ok;
  ok = expect(
      extras.find("draw-border-with-background false") != std::string::npos, "writes draw-border false"
  ) && ok;

  const auto borderOnly = compositors::niri::kdlForWindowRules({.drawBorderWithBackground = true});
  ok = expect(borderOnly.find("geometry-corner-radius") == std::string::npos, "extras-only omits radius") && ok;
  ok = expect(
      borderOnly.find("draw-border-with-background true") != std::string::npos, "writes draw-border true"
  ) && ok;

  const auto dir = makeTempDir();
  const auto config = dir / "config.kdl";
  {
    std::ofstream out(config);
    out << "spawn-at-startup \"noctalia\"\n";
  }
  const auto saved = compositors::niri::saveWindowRulesInto(config, {.drawBorderWithBackground = false});
  ok = expect(saved.ok, "saves window rules into a temp niri config") && ok;
  ok = expect(
      compositors::niri::hasInclude(compositors::niri::readFile(config), "noctalia-window-rules.kdl"),
      "appends include"
  ) && ok;

  std::filesystem::remove_all(dir);
  return ok ? 0 : 1;
}
