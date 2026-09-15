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
  ok = expect(tabSearchScore("appearance", "Appearance", "font") == 0, "font is not a tab") && ok;
  return ok ? 0 : 1;
}
