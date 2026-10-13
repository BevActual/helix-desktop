#include "theme/adw_gtk3.h"

#include <filesystem>
#include <print>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "adw_gtk3_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

  std::filesystem::path makeTempDir() {
    auto path = std::filesystem::temp_directory_path() / ("helix-adw-gtk3-" + std::to_string(::getpid()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
  }

} // namespace

int main() {
  bool ok = true;

  ThemeConfig::TemplatesConfig templates;
  templates.enableBuiltinTemplates = false;
  templates.builtinIds = {"gtk3", "gtk4"};
  ok = expect(!helix::theme::gtkTemplatesEnabled(templates), "disabled catalog") && ok;

  templates.enableBuiltinTemplates = true;
  templates.builtinIds = {"niri"};
  ok = expect(!helix::theme::gtkTemplatesEnabled(templates), "gtk ids absent") && ok;

  templates.builtinIds = {"gtk3"};
  ok = expect(helix::theme::gtkTemplatesEnabled(templates), "gtk3 only") && ok;

  templates.builtinIds = {"niri", "gtk4"};
  ok = expect(helix::theme::gtkTemplatesEnabled(templates), "gtk4 only") && ok;

  std::vector<std::string> ids = {"niri"};
  ok = expect(helix::theme::appendGtkTemplateIds(ids), "appends gtk ids") && ok;
  ok = expect(ids == std::vector<std::string>{"niri", "gtk3", "gtk4"}, "order gtk3 then gtk4") && ok;
  ok = expect(!helix::theme::appendGtkTemplateIds(ids), "idempotent") && ok;

  const auto missing = makeTempDir();
  ok = expect(!helix::theme::adwGtk3ThemePresent({missing}), "missing theme dir") && ok;

  const auto present = makeTempDir();
  std::filesystem::create_directories(present / "adw-gtk3-dark");
  ok = expect(helix::theme::adwGtk3ThemePresent({present}), "adw-gtk3-dark counts") && ok;

  std::filesystem::remove_all(missing);
  std::filesystem::remove_all(present);
  return ok ? 0 : 1;
}
