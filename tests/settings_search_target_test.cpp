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

  settings::SettingEntry entry(settings::SettingsSection section, std::string searchText, std::string group = {}) {
    settings::SettingEntry out;
    out.section = section;
    out.group = std::move(group);
    out.searchText = std::move(searchText);
    return out;
  }

} // namespace

int main() {
  using control_center::resolveSettingsSearchTarget;
  using control_center::tabIdForSettingsGroup;
  using control_center::tabIdForSettingsSection;

  const std::vector<settings::SettingEntry> registry{
      entry(settings::SettingsSection::Wallpaper, "wallpaper fill mode folders"),
      entry(settings::SettingsSection::Theme, "appearance roundness font scale"),
      entry(settings::SettingsSection::Screenshot, "screenshot delay timer"),
      entry(settings::SettingsSection::ScreenRecord, "screen-record audio bitrate"),
      entry(settings::SettingsSection::Dock, "dock autohide pinned position"),
      entry(settings::SettingsSection::Services, "sound volume step overdrive", "audio"),
      entry(settings::SettingsSection::Services, "calendar week start", "calendar"),
      entry(settings::SettingsSection::Shell, "clipboard history entries", "clipboard"),
      entry(settings::SettingsSection::Location, "night light warmth schedule", "night-light"),
  };

  bool ok = true;
  ok = expect(tabIdForSettingsSection("location") == "weather", "location -> weather") && ok;
  ok = expect(tabIdForSettingsSection("services") == "calendar", "services -> calendar") && ok;
  ok = expect(tabIdForSettingsSection("clock") == "clock", "clock stays") && ok;
  ok = expect(tabIdForSettingsSection("screenshot") == "capture", "screenshot -> capture") && ok;
  ok = expect(tabIdForSettingsSection("screen-record") == "capture", "screen-record -> capture") && ok;
  ok = expect(tabIdForSettingsSection("widgets") == "desktop", "widgets -> desktop") && ok;
  ok = expect(tabIdForSettingsSection("corners") == "desktop", "corners -> desktop") && ok;
  ok = expect(tabIdForSettingsSection("hot-corners") == "desktop", "hot-corners -> desktop") && ok;
  ok = expect(tabIdForSettingsSection("screen-corners") == "desktop", "screen-corners -> desktop") && ok;
  ok = expect(tabIdForSettingsSection("bar") == "desktop", "bar -> desktop") && ok;
  ok = expect(tabIdForSettingsSection("dock") == "desktop", "dock -> desktop") && ok;
  ok = expect(tabIdForSettingsSection("windows") == "desktop", "windows -> desktop") && ok;
  ok = expect(tabIdForSettingsSection("panels") == "desktop", "panels -> desktop") && ok;
  ok = expect(tabIdForSettingsSection("osd") == "desktop", "osd -> desktop") && ok;
  ok = expect(tabIdForSettingsSection("ai-usage") == "ai", "ai-usage -> ai") && ok;
  ok = expect(tabIdForSettingsSection("wallpaper") == "wallpaper", "wallpaper stays") && ok;
  ok = expect(tabIdForSettingsSection("glass") == "theme", "glass -> theme") && ok;
  ok = expect(tabIdForSettingsSection("chrome") == "theme", "chrome -> theme") && ok;
  ok = expect(tabIdForSettingsSection("type") == "theme", "type -> theme") && ok;
  ok = expect(tabIdForSettingsSection("roundness") == "theme", "roundness -> theme") && ok;
  ok = expect(tabIdForSettingsSection("language") == "theme", "language -> theme") && ok;

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
  ok = expect(dockFromHome.tabId == "desktop", "home autohide -> desktop") && ok;

  // Shared sections route by group to the tab that shows the group.
  ok = expect(tabIdForSettingsGroup("services", "audio") == "audio", "services/audio -> audio") && ok;
  ok = expect(tabIdForSettingsGroup("services", "media") == "media", "services/media -> media") && ok;
  ok = expect(tabIdForSettingsGroup("services", "brightness") == "monitor", "services/brightness -> monitor") && ok;
  ok = expect(tabIdForSettingsGroup("services", "calendar") == "calendar", "services/calendar -> calendar") && ok;
  ok = expect(tabIdForSettingsGroup("system", "screen-time") == "screen-time", "system/screen-time") && ok;
  ok = expect(tabIdForSettingsGroup("system", "battery") == "system", "system/battery stays") && ok;
  ok = expect(tabIdForSettingsGroup("shell", "clipboard") == "clipboard", "shell/clipboard") && ok;
  ok = expect(tabIdForSettingsGroup("shell", "general") == "shell", "shell/general stays") && ok;
  ok = expect(tabIdForSettingsGroup("location", "night-light") == "monitor", "night-light -> monitor") && ok;
  ok = expect(tabIdForSettingsGroup("location", "weather") == "weather", "location/weather -> weather") && ok;
  ok = expect(tabIdForSettingsGroup("control-center", "ai-usage") == "ai", "ai-usage group -> ai") && ok;
  ok = expect(tabIdForSettingsGroup("security", "privacy") == "privacy", "security/privacy -> privacy") && ok;
  ok = expect(tabIdForSettingsGroup("dock", "") == "desktop", "no group falls back to section") && ok;

  // Searching for an audio setting used to open Calendar (section "services" -> calendar).
  const auto volumeFromPackages = resolveSettingsSearchTarget("packages", false, noSections, registry, "volume");
  ok = expect(volumeFromPackages.tabId == "audio", "volume -> audio, not calendar") && ok;
  ok = expect(volumeFromPackages.sectionId == "services", "volume keeps its section") && ok;
  const auto weekFromPackages = resolveSettingsSearchTarget("packages", false, noSections, registry, "week start");
  ok = expect(weekFromPackages.tabId == "calendar", "week start -> calendar") && ok;
  const auto historyFromPackages = resolveSettingsSearchTarget("packages", false, noSections, registry, "clipboard");
  ok = expect(historyFromPackages.tabId == "clipboard", "clipboard -> clipboard, not shell") && ok;
  const auto warmthFromPackages = resolveSettingsSearchTarget("packages", false, noSections, registry, "warmth");
  ok = expect(warmthFromPackages.tabId == "monitor", "night light -> monitor, not weather") && ok;

  return ok ? 0 : 1;
}
