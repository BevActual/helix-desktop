#include "shell/control_center/tab_search.h"

#include <print>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "tab_search_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

} // namespace

int main() {
  using control_center::tabSearchScore;

  bool ok = true;
  ok = expect(tabSearchScore("network", "Network", "wifi") > 0, "wifi alias") && ok;
  ok = expect(tabSearchScore("network", "Network", "wi-fi") > 0, "wi-fi alias") && ok;
  ok = expect(tabSearchScore("packages", "Packages", "updates") > 0, "updates alias") && ok;
  ok = expect(tabSearchScore("notifications", "Notifications", "dnd") > 0, "dnd alias") && ok;
  ok = expect(tabSearchScore("capture", "Capture", "recording") > 0, "recording alias") && ok;
  ok = expect(tabSearchScore("capture", "Capture", "screenshot") > 0, "screenshot alias") && ok;
  ok = expect(tabSearchScore("calendar", "Calendar", "accounts") > 0, "calendar accounts alias") && ok;
  ok = expect(tabSearchScore("clock", "Clock", "timezone") > 0, "clock timezone alias") && ok;
  ok = expect(tabSearchScore("clock", "Clock", "world-clock") > 0, "clock world-clock alias") && ok;
  ok = expect(
      tabSearchScore("clock", "Clock", "timezone") > tabSearchScore("calendar", "Calendar", "timezone"),
      "timezone prefers clock"
  ) && ok;
  ok = expect(tabSearchScore("ai", "AI", "claude") > 0, "ai claude alias") && ok;
  ok = expect(tabSearchScore("ai", "AI", "ollama") > 0, "ai ollama alias") && ok;
  ok = expect(tabSearchScore("ai", "AI", "ai-usage") > 0, "ai-usage alias") && ok;
  ok = expect(tabSearchScore("weather", "Weather", "location") > 0, "weather location alias") && ok;
  ok = expect(tabSearchScore("plugins", "Plugins", "plugin") > 0, "plugin prefix") && ok;
  ok = expect(tabSearchScore("network", "Network", "w") == 0, "one char") && ok;
  ok = expect(tabSearchScore("home", "Home", "wifi") == 0, "home misses wifi") && ok;
  ok = expect(
      tabSearchScore("network", "Network", "wifi") > tabSearchScore("wallpaper", "Wallpaper", "wifi"),
      "wifi prefers network"
  ) && ok;
  ok = expect(tabSearchScore("theme", "Theme", "glass") > 0, "glass alias on theme") && ok;
  ok = expect(tabSearchScore("theme", "Theme", "type") > 0, "type alias on theme") && ok;
  ok = expect(tabSearchScore("theme", "Theme", "effects") > 0, "effects alias on theme") && ok;
  ok = expect(tabSearchScore("theme", "Theme", "customize") > 0, "theme customize alias") && ok;
  ok = expect(tabSearchScore("theme", "Theme", "roundness") > 0, "roundness alias on theme") && ok;
  ok = expect(tabSearchScore("theme", "Theme", "language") > 0, "language alias on theme") && ok;
  ok = expect(tabSearchScore("desktop", "Desktop", "widgets") > 0, "widgets alias on desktop") && ok;
  ok = expect(tabSearchScore("desktop", "Desktop", "corners") > 0, "corners alias on desktop") && ok;
  ok = expect(tabSearchScore("desktop", "Desktop", "hot-corners") > 0, "hot-corners alias on desktop") && ok;
  ok = expect(tabSearchScore("desktop", "Desktop", "bar") > 0, "bar alias on desktop") && ok;
  ok = expect(tabSearchScore("desktop", "Desktop", "thickness") > 0, "thickness alias on desktop") && ok;
  ok = expect(tabSearchScore("desktop", "Desktop", "dock") > 0, "dock alias on desktop") && ok;
  ok = expect(tabSearchScore("desktop", "Desktop", "gaps") > 0, "gaps alias on desktop") && ok;
  ok = expect(
      tabSearchScore("clock", "Clock", "clock") > tabSearchScore("desktop", "Desktop", "clock"),
      "clock prefers clock tab"
  ) && ok;
  ok = expect(tabSearchScore("theme", "Theme", "font") == 0, "font uses settings search") && ok;
  ok = expect(tabSearchScore("theme", "Theme", "locale") == 0, "locale uses settings search") && ok;
  return ok ? 0 : 1;
}
