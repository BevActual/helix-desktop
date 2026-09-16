#include "shell/control_center/settings_search_target.h"

#include <print>
#include <string>
#include <vector>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "settings_search_target_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

  settings::SettingEntry entry(settings::SettingsSection section, std::string searchText) {
    settings::SettingEntry out;
    out.section = section;
    out.searchText = std::move(searchText);
    return out;
  }

} // namespace

int main() {
  using control_center::resolveSettingsSearchTarget;
  using control_center::tabIdForSettingsSection;

  const std::vector<settings::SettingEntry> registry{
      entry(settings::SettingsSection::Wallpaper, "wallpaper fill mode folders"),
      entry(settings::SettingsSection::Theme, "appearance roundness font scale"),
      entry(settings::SettingsSection::Screenshot, "screenshot delay timer"),
      entry(settings::SettingsSection::ScreenRecord, "screen-record audio bitrate"),
      entry(settings::SettingsSection::Dock, "dock autohide pinned position"),
  };

  bool ok = true;
  ok = expect(tabIdForSettingsSection("location") == "weather", "location -> weather") && ok;
  ok = expect(tabIdForSettingsSection("services") == "calendar", "services -> calendar") && ok;
  ok = expect(tabIdForSettingsSection("screenshot") == "capture", "screenshot -> capture") && ok;
  ok = expect(tabIdForSettingsSection("screen-record") == "capture", "screen-record -> capture") && ok;
  ok = expect(tabIdForSettingsSection("ai-usage") == "ai", "ai-usage -> ai") && ok;
  ok = expect(tabIdForSettingsSection("wallpaper") == "wallpaper", "wallpaper stays") && ok;

  const std::vector<std::string> wallpaperSections{"wallpaper"};
  const auto fillOnWallpaper = resolveSettingsSearchTarget(
      "wallpaper", true, wallpaperSections, registry, "fill"
  );
  ok = expect(fillOnWallpaper.stayOnCurrent, "fill stays on wallpaper") && ok;
  ok = expect(fillOnWallpaper.tabId == "wallpaper", "fill tab is wallpaper") && ok;
  ok = expect(fillOnWallpaper.configureMode, "fill opens gear") && ok;

  const auto roundOnWallpaper = resolveSettingsSearchTarget(
      "wallpaper", true, wallpaperSections, registry, "roundness"
  );
  ok = expect(!roundOnWallpaper.stayOnCurrent, "roundness leaves wallpaper") && ok;
  ok = expect(roundOnWallpaper.tabId == "theme", "roundness jumps to theme") && ok;
  ok = expect(roundOnWallpaper.configureMode, "roundness opens gear") && ok;

  const auto noneOnWallpaper = resolveSettingsSearchTarget(
      "wallpaper", true, wallpaperSections, registry, "zzzznone"
  );
  ok = expect(noneOnWallpaper.stayOnCurrent, "empty stay on wallpaper") && ok;
  ok = expect(noneOnWallpaper.tabId == "wallpaper", "empty tab is wallpaper") && ok;
  ok = expect(noneOnWallpaper.configureMode, "empty still opens gear") && ok;

  const std::vector<std::string> noSections;
  const auto fillFromPackages = resolveSettingsSearchTarget(
      "packages", false, noSections, registry, "fill"
  );
  ok = expect(!fillFromPackages.stayOnCurrent, "packages jumps for fill") && ok;
  ok = expect(fillFromPackages.tabId == "wallpaper", "packages fill -> wallpaper") && ok;
  ok = expect(fillFromPackages.configureMode, "packages fill opens gear") && ok;

  const auto noneFromPackages = resolveSettingsSearchTarget(
      "packages", false, noSections, registry, "zzzznone"
  );
  ok = expect(noneFromPackages.stayOnCurrent, "packages empty stays") && ok;
  ok = expect(!noneFromPackages.configureMode, "packages empty does not open gear") && ok;
  ok = expect(noneFromPackages.tabId == "packages", "packages empty keeps packages") && ok;

  const std::vector<std::string> captureSections{"screenshot", "screen-record"};
  const auto delayOnCapture = resolveSettingsSearchTarget(
      "capture", true, captureSections, registry, "delay"
  );
  ok = expect(delayOnCapture.stayOnCurrent, "delay stays on capture") && ok;
  ok = expect(delayOnCapture.tabId == "capture", "delay tab is capture") && ok;

  const auto recordOnCapture = resolveSettingsSearchTarget(
      "capture", true, captureSections, registry, "bitrate"
  );
  ok = expect(recordOnCapture.stayOnCurrent, "record setting stays on capture") && ok;

  const auto roundOnCapture = resolveSettingsSearchTarget(
      "capture", true, captureSections, registry, "roundness"
  );
  ok = expect(!roundOnCapture.stayOnCurrent, "roundness leaves capture") && ok;
  ok = expect(roundOnCapture.tabId == "theme", "capture roundness -> theme") && ok;

  const auto dockFromHome = resolveSettingsSearchTarget("home", true, std::vector<std::string>{"control-center"}, registry, "autohide");
  ok = expect(!dockFromHome.stayOnCurrent, "home autohide jumps") && ok;
  ok = expect(dockFromHome.tabId == "dock", "home autohide -> dock") && ok;

  return ok ? 0 : 1;
}
