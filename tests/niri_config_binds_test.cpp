#include "compositors/niri/niri_config_binds.h"

#include "core/input/key_chord.h"
#include "core/input/key_modifiers.h"

#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "niri_config_binds_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

  std::filesystem::path makeTempDir() {
    auto path = std::filesystem::temp_directory_path() / ("noctalia-niri-binds-" + std::to_string(::getpid()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
  }

} // namespace

int main() {
  bool ok = true;

  const char* kConfig = R"(
    include "noctalia.kdl"
    // include "commented-binds.kdl"
    binds {
        Mod+Q { close-window; }
        Mod+Space { spawn-sh "noctalia msg panel-toggle launcher"; }
        Mod+T hotkey-overlay-title="Terminal" { spawn "alacritty"; }
        XF86AudioRaiseVolume allow-when-locked=true { spawn-sh "noctalia msg volume-up"; }
        Mod+S { spawn "noctalia" "msg" "panel-toggle" "control-center"; }
    }
  )";

  const auto parsed = compositors::niri::parseBinds(kConfig);
  ok = expect(parsed.size() == 5, "parses five bind statements") && ok;
  if (parsed.size() >= 5) {
    ok = expect(parsed[0].key == "Mod+Q" && parsed[0].ipcCommand.empty(), "keeps non-noctalia binds") && ok;
    ok = expect(
             parsed[1].key == "Mod+Space" && parsed[1].ipcCommand == "panel-toggle launcher",
             "reads spawn-sh noctalia msg"
         )
        && ok;
    ok = expect(
             parsed[3].key == "XF86AudioRaiseVolume" && parsed[3].ipcCommand == "volume-up",
             "reads media keys with properties"
         )
        && ok;
    ok = expect(
             parsed[4].key == "Mod+S" && parsed[4].ipcCommand == "panel-toggle control-center",
             "reads spawn argv noctalia msg"
         )
        && ok;
  }

  const auto includes = compositors::niri::parseIncludes(kConfig, "/tmp/config.kdl");
  ok = expect(includes.size() == 1 && includes[0].filename() == "noctalia.kdl", "parses include and ignores comments")
      && ok;

  const auto dir = makeTempDir();
  const auto config = dir / "config.kdl";
  {
    std::ofstream out(config);
    out << "binds {\n    Mod+Q { close-window; }\n}\n";
  }

  const auto installed = compositors::niri::installMissingInto(config);
  ok = expect(installed.ok, "installs into a temp niri config") && ok;
  ok = expect(installed.added > 0, "adds recommended binds") && ok;

  const auto scan = compositors::niri::scanConfigFile(config);
  ok = expect(scan.hasBindsInclude, "appends noctalia-binds.kdl include") && ok;
  const auto missing = compositors::niri::missingRecommended(scan);
  ok = expect(missing.empty(), "recommended binds are present after install") && ok;

  const auto launcher = compositors::niri::bindStatuses(scan);
  bool launcherBound = false;
  for (const auto& status : launcher) {
    if (status.recommended.id == "launcher" && !status.boundKeys.empty()) {
      launcherBound = true;
    }
  }
  ok = expect(launcherBound, "launcher bind is reported as present") && ok;

  const auto again = compositors::niri::installMissingInto(config);
  ok = expect(again.ok && again.added == 0, "second install is a no-op") && ok;

  KeyChord superSpace{.sym = XKB_KEY_space, .modifiers = KeyMod::Super};
  ok = expect(compositors::niri::niriKeyFromChord(superSpace) == "Mod+Space", "maps Super to niri Mod") && ok;
  const auto parsedChord = compositors::niri::niriKeyToChord("Mod+Comma");
  ok = expect(parsedChord.has_value() && (parsedChord->modifiers & KeyMod::Super) != 0, "parses niri Mod as Super")
      && ok;

  const auto changed = compositors::niri::setRecommendedKeyInto(config, "launcher", "Mod+D");
  ok = expect(changed.ok, "rewrites an owned bind key") && ok;
  const auto afterChange = compositors::niri::scanConfigFile(config);
  bool launcherModD = false;
  for (const auto& status : compositors::niri::bindStatuses(afterChange)) {
    if (status.recommended.id == "launcher") {
      for (const auto& key : status.boundKeys) {
        if (key == "Mod+D") {
          launcherModD = true;
        }
      }
      ok = expect(status.inOwnedInclude, "launcher bind is in the owned include") && ok;
    }
  }
  ok = expect(launcherModD, "launcher chord changed to Mod+D") && ok;

  std::filesystem::remove_all(dir);
  return ok ? 0 : 1;
}
