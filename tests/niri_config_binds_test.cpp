#include "compositors/niri/niri_config_binds.h"

#include "core/input/key_chord.h"
#include "core/input/key_modifiers.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
#include <string>
#include <unistd.h>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "niri_config_binds_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

  std::filesystem::path makeTempDir() {
    auto path = std::filesystem::temp_directory_path() / ("helix-niri-binds-" + std::to_string(::getpid()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
  }

} // namespace

int main() {
  bool ok = true;

  const char* kConfig = R"(
    include "helix.kdl"
    // include "commented-binds.kdl"
    binds {
        Mod+Q { close-window; }
        Mod+Space { spawn-sh "helix-panel panel-toggle launcher"; }
        Mod+T hotkey-overlay-title="Terminal" { spawn "alacritty"; }
        XF86AudioRaiseVolume allow-when-locked=true { spawn-sh "helix-panel volume-up"; }
        Mod+S { spawn "helix" "msg" "panel-toggle" "control-center"; }
    }
  )";

  const auto parsed = compositors::niri::parseBinds(kConfig);
  ok = expect(parsed.size() == 5, "parses five bind statements") && ok;
  if (parsed.size() >= 5) {
    ok = expect(parsed[0].key == "Mod+Q" && parsed[0].ipcCommand.empty(), "keeps non-helix binds") && ok;
    ok = expect(
             parsed[1].key == "Mod+Space" && parsed[1].ipcCommand == "panel-toggle launcher",
             "reads spawn-sh helix-panel"
         )
        && ok;
    ok = expect(
             parsed[3].key == "XF86AudioRaiseVolume" && parsed[3].ipcCommand == "volume-up",
             "reads media keys with properties"
         )
        && ok;
    ok = expect(
             parsed[4].key == "Mod+S" && parsed[4].ipcCommand == "panel-toggle control-center",
             "reads spawn argv helix-panel"
         )
        && ok;
  }

  const auto includes = compositors::niri::parseIncludes(kConfig, "/tmp/config.kdl");
  ok = expect(includes.size() == 1 && includes[0].filename() == "helix.kdl", "parses include and ignores comments")
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
  ok = expect(scan.hasBindsInclude, "appends helix-binds.kdl include") && ok;
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

  const auto cleared = compositors::niri::clearRecommendedKeyInto(config, "launcher");
  ok = expect(cleared.ok && cleared.added == 1, "clears an owned bind") && ok;
  bool launcherGone = true;
  for (const auto& status : compositors::niri::bindStatuses(compositors::niri::scanConfigFile(config))) {
    if (status.recommended.id == "launcher" && !status.boundKeys.empty()) {
      launcherGone = false;
    }
  }
  ok = expect(launcherGone, "launcher bind is missing after clear") && ok;
  const auto clearAgain = compositors::niri::clearRecommendedKeyInto(config, "launcher");
  ok = expect(!clearAgain.ok, "clearing an unmanaged bind fails") && ok;

  const auto rebound = compositors::niri::setRecommendedKeyInto(config, "volume-up", "Mod+Shift+Equal");
  ok = expect(rebound.ok, "remaps volume-up") && ok;
  const auto reset = compositors::niri::resetRecommendedKeysInto(config);
  ok = expect(reset.ok && reset.added >= 1, "resets remapped managed binds") && ok;
  bool volumeDefault = false;
  bool volumeLocked = false;
  const auto afterReset = compositors::niri::scanConfigFile(config);
  for (const auto& status : compositors::niri::bindStatuses(afterReset)) {
    if (status.recommended.id == "volume-up") {
      volumeDefault = !status.boundKeys.empty() && status.boundKeys.front() == "XF86AudioRaiseVolume";
    }
  }
  ok = expect(volumeDefault, "volume-up restored to XF86AudioRaiseVolume") && ok;
  {
    std::ifstream in(afterReset.bindsIncludePath);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    volumeLocked = text.find("XF86AudioRaiseVolume allow-when-locked=true") != std::string::npos;
  }
  ok = expect(volumeLocked, "writes allow-when-locked for media keys") && ok;

  const auto play = compositors::niri::niriKeyToChord("XF86AudioPlay");
  ok = expect(play.has_value() && play->sym == XKB_KEY_XF86AudioPlay, "parses XF86 media keys") && ok;
  bool sawCapture = false;
  bool sawMedia = false;
  bool sawSystem = false;
  for (const auto& bind : compositors::niri::recommendedBinds()) {
    sawCapture = sawCapture || bind.kind == compositors::niri::BindKind::Capture;
    sawMedia = sawMedia || bind.kind == compositors::niri::BindKind::Media;
    sawSystem = sawSystem || bind.kind == compositors::niri::BindKind::System;
  }
  ok = expect(sawCapture && sawMedia && sawSystem, "catalog is grouped by kind") && ok;
  ok = expect(
           compositors::niri::kdlLine(compositors::niri::recommendedBinds()[0]).find("allow-when-locked")
               == std::string::npos,
           "launcher kdl has no allow-when-locked"
       )
      && ok;

  const std::vector<std::string> kitty{"kitty"};
  const auto appAdded = compositors::niri::setAppBindInto(config, "Mod+T", kitty);
  ok = expect(appAdded.ok && appAdded.added == 1, "adds an owned app spawn bind") && ok;
  const auto apps = compositors::niri::ownedAppBinds(compositors::niri::scanConfigFile(config));
  bool kittyBound = false;
  for (const auto& app : apps) {
    if (app.key == "Mod+T" && app.spawnArgs == kitty) {
      kittyBound = true;
    }
  }
  ok = expect(kittyBound, "owned app bind is listed") && ok;
  const auto remappedApp = compositors::niri::setAppBindInto(config, "Mod+Return", kitty);
  ok = expect(remappedApp.ok, "remaps an owned app bind") && ok;
  bool kittyMoved = false;
  bool kittyOldGone = true;
  for (const auto& app : compositors::niri::ownedAppBinds(compositors::niri::scanConfigFile(config))) {
    if (app.spawnArgs == kitty && app.key == "Mod+Return") {
      kittyMoved = true;
    }
    if (app.key == "Mod+T") {
      kittyOldGone = false;
    }
  }
  ok = expect(kittyMoved && kittyOldGone, "app bind key moved to Mod+Return") && ok;
  const auto clearedApp = compositors::niri::clearAppBindInto(config, "Mod+Return");
  ok = expect(clearedApp.ok, "clears an owned app bind") && ok;
  ok = expect(
           compositors::niri::ownedAppBinds(compositors::niri::scanConfigFile(config)).empty(),
           "no owned app binds after clear"
       )
      && ok;

  const auto stockDir = makeTempDir() / "stock";
  std::filesystem::create_directories(stockDir);
  const auto stockConfig = stockDir / "config.kdl";
  {
    std::ofstream stock(stockDir / "default-config.kdl");
    stock << "binds {\n    Print { screenshot; }\n    Mod+Shift+P { power-off-monitors; }\n}\n";
    std::ofstream out(stockConfig);
    out << "include \"default-config.kdl\"\nbinds {\n    Mod+Shift+C { center-column; }\n}\n";
  }
  const auto stockScan = compositors::niri::scanConfigFile(stockConfig);
  bool printConflict = false;
  bool sessionConflict = false;
  bool caffeineConflict = false;
  for (const auto& status : compositors::niri::bindStatuses(stockScan)) {
    if (status.recommended.id == "screenshot-region") {
      printConflict = status.keyConflict;
    }
    if (status.recommended.id == "session") {
      sessionConflict = status.keyConflict;
    }
    if (status.recommended.id == "caffeine") {
      caffeineConflict = status.keyConflict;
    }
  }
  ok = expect(!printConflict && !sessionConflict, "stock include keys are not conflicts") && ok;
  ok = expect(caffeineConflict, "keys in the user's binds block stay conflicts") && ok;

  const auto stockInstall = compositors::niri::installMissingInto(stockConfig);
  ok = expect(stockInstall.ok && stockInstall.added > 0, "installs over stock include keys") && ok;
  ok = expect(stockInstall.skippedConflicts == 0, "caffeine falls back instead of skipping") && ok;
  bool printOwned = false;
  bool sessionOwned = false;
  bool caffeineFallback = false;
  for (const auto& status : compositors::niri::bindStatuses(compositors::niri::scanConfigFile(stockConfig))) {
    if (status.recommended.id == "screenshot-region") {
      printOwned = !status.boundKeys.empty() && status.boundKeys.front() == "Print";
    }
    if (status.recommended.id == "session") {
      sessionOwned = !status.boundKeys.empty() && status.boundKeys.front() == "Mod+Shift+P";
    }
    if (status.recommended.id == "caffeine") {
      caffeineFallback = !status.boundKeys.empty() && status.boundKeys.front() == "Mod+Shift+Z";
    }
  }
  ok = expect(printOwned && sessionOwned, "overrides stock Print and Mod+Shift+P") && ok;
  ok = expect(caffeineFallback, "caffeine uses Mod+Shift+Z when Mod+Shift+C is taken") && ok;

  const auto userPrint = makeTempDir() / "user-print";
  std::filesystem::create_directories(userPrint);
  const auto userPrintConfig = userPrint / "config.kdl";
  {
    std::ofstream out(userPrintConfig);
    out << "binds {\n    Print { screenshot; }\n}\n";
  }
  bool userPrintConflict = false;
  for (const auto& status : compositors::niri::bindStatuses(compositors::niri::scanConfigFile(userPrintConfig))) {
    if (status.recommended.id == "screenshot-region") {
      userPrintConflict = status.keyConflict;
    }
  }
  ok = expect(userPrintConflict, "Print in config.kdl is still a conflict") && ok;
  const auto blocked = compositors::niri::setRecommendedKeyInto(userPrintConfig, "screenshot-region", "Print");
  ok = expect(!blocked.ok && blocked.skippedConflicts == 1, "does not steal Print from the user's binds") && ok;

  const auto quickInsertLine = compositors::niri::kdlLine(
      {.id = "quick-insert", .ipcCommand = "panel-toggle quick-insert", .key = "F19"}
  );
  ok = expect(
           quickInsertLine.find("F19") != std::string::npos && quickInsertLine.find("repeat=false") != std::string::npos,
           "Quick Insert bind is F19 without key repeat"
       )
      && ok;

  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(stockDir);
  std::filesystem::remove_all(userPrint);
  return ok ? 0 : 1;
}
