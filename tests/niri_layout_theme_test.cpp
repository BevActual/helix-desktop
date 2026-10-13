#include "compositors/niri/niri_layout_theme.h"

#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "niri_layout_theme_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

  std::filesystem::path makeTempDir() {
    auto path = std::filesystem::temp_directory_path() / ("helix-niri-layout-" + std::to_string(::getpid()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
  }

} // namespace

int main() {
  bool ok = true;

  const compositors::niri::LayoutThemeSettings settings{
      .focusRing = false,
      .border = true,
      .activeColorHex = "#112233",
      .inactiveColorHex = "#445566",
      .urgentColorHex = "#ff0000",
      .tabInactiveColorHex = "#778899",
      .insertHintColorHex = "#11223380",
  };
  const auto kdl = compositors::niri::kdlForLayoutTheme(settings);
  ok = expect(kdl.find("focus-ring") != std::string::npos, "writes focus-ring") && ok;
  ok = expect(kdl.find("        off\n") != std::string::npos, "disabled ring is off") && ok;
  ok = expect(kdl.find("tab-indicator") != std::string::npos, "writes tab-indicator") && ok;
  ok = expect(kdl.find("insert-hint") != std::string::npos, "writes insert-hint") && ok;
  ok = expect(kdl.find("recent-windows") != std::string::npos, "writes recent-windows") && ok;
  ok = expect(kdl.find("\"#11223380\"") != std::string::npos, "insert-hint keeps alpha") && ok;
  ok = expect(kdl.find("\"#ff0000\"") != std::string::npos, "writes urgent color") && ok;
  ok = expect(kdl.find("gaps") == std::string::npos, "omits gaps until set") && ok;

  const auto withGaps = compositors::niri::kdlForLayoutTheme({
      .activeColorHex = "#112233",
      .inactiveColorHex = "#445566",
      .gaps = 20,
  });
  ok = expect(withGaps.find("    gaps 20\n") != std::string::npos, "writes configured gaps") && ok;

  const auto withLayoutExtras = compositors::niri::kdlForLayoutTheme({
      .activeColorHex = "#112233",
      .inactiveColorHex = "#445566",
      .struts = 8,
      .defaultColumnWidth = 0.5,
  });
  ok = expect(
           withLayoutExtras.find("    default-column-width { proportion 0.50; }\n") != std::string::npos,
           "writes default column width"
       )
      && ok;
  ok = expect(withLayoutExtras.find("        left 8\n") != std::string::npos, "writes equal struts") && ok;

  const auto dir = makeTempDir();
  const auto config = dir / "config.kdl";
  {
    std::ofstream out(config);
    out << "spawn-at-startup \"helix\"\n";
  }
  const auto saved = compositors::niri::saveNiriLayoutThemeInto(config, settings);
  ok = expect(saved.ok, "saves layout into a temp niri config") && ok;
  ok = expect(compositors::niri::hasInclude(compositors::niri::readFile(config), "helix-layout.kdl"), "appends include")
      && ok;

  std::filesystem::remove_all(dir);
  return ok ? 0 : 1;
}
