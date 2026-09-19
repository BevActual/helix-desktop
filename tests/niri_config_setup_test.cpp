#include "compositors/niri/niri_config_setup.h"

#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "niri_config_setup_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

  std::filesystem::path makeTempDir() {
    auto path = std::filesystem::temp_directory_path() / ("noctalia-niri-setup-" + std::to_string(::getpid()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
  }

} // namespace

int main() {
  bool ok = true;

  const auto dir = makeTempDir();
  const auto config = dir / "config.kdl";
  {
    std::ofstream out(config);
    out << "spawn-at-startup \"noctalia\"\n";
  }

  auto scan = compositors::niri::scanSetupFile(config, true);
  ok = expect(scan.configExists, "finds the temp niri config") && ok;
  bool autostartPresent = false;
  bool activationMissing = false;
  for (const auto& status : scan.items) {
    if (status.item.id == "autostart") {
      autostartPresent = status.present;
    }
    if (status.item.id == "xdg-activation") {
      activationMissing = !status.present;
    }
  }
  ok = expect(autostartPresent, "detects existing spawn-at-startup noctalia") && ok;
  ok = expect(activationMissing, "xdg-activation is missing before install") && ok;

  const auto installed = compositors::niri::installSetupInto(config, true);
  ok = expect(installed.ok, "installs missing setup snippets") && ok;
  ok = expect(installed.added > 0, "adds missing snippets") && ok;

  const auto after = compositors::niri::scanSetupFile(config, true);
  ok = expect(after.hasSetupInclude, "appends noctalia-setup.kdl include") && ok;
  const auto missing = compositors::niri::missingSetup(after);
  ok = expect(missing.empty(), "all catalog items are present after install") && ok;

  const auto includeText = compositors::niri::readFile(dir / "noctalia-setup.kdl");
  const auto first = includeText.find("spawn-at-startup \"noctalia\"");
  const auto second = includeText.find("spawn-at-startup \"noctalia\"", first == std::string::npos ? 0 : first + 1);
  ok = expect(first == std::string::npos, "does not duplicate autostart into the owned include") && ok;
  static_cast<void>(second);
  ok = expect(
           includeText.find(R"(session" "lid-close")") != std::string::npos,
           "installs lid-close IPC snippet"
       )
      && ok;

  const auto again = compositors::niri::installSetupInto(config, true);
  ok = expect(again.ok && again.added == 0, "second setup install is a no-op") && ok;

  const auto legacyInclude = dir / "legacy-lid-close.kdl";
  {
    std::ofstream out(legacyInclude);
    out << "switch-events {\n    lid-close { spawn \"noctalia\" \"msg\" \"session\" \"lock-and-suspend\"; }\n}\n";
  }
  std::string migrateError;
  ok = expect(
           compositors::niri::migrateLidCloseSnippetIn(legacyInclude, migrateError),
           "migrates lock-and-suspend lid-close"
       )
      && ok;
  const auto migrated = compositors::niri::readFile(legacyInclude);
  ok = expect(migrated.find("lock-and-suspend") == std::string::npos, "removes lock-and-suspend lid spawn") && ok;
  ok = expect(migrated.find(R"(session" "lid-close")") != std::string::npos, "rewrites to session lid-close") && ok;

  std::filesystem::remove_all(dir);
  return ok ? 0 : 1;
}
