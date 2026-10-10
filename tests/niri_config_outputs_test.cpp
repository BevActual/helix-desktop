#include "compositors/niri/niri_config_outputs.h"
#include "system/display_config.h"

#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "niri_config_outputs_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

  std::filesystem::path makeTempDir() {
    auto path = std::filesystem::temp_directory_path() / ("helix-niri-outputs-" + std::to_string(::getpid()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
  }

} // namespace

int main() {
  bool ok = true;

  display_config::Output laptop;
  laptop.name = "eDP-1";
  laptop.enabled = true;
  laptop.x = 0;
  laptop.y = 0;
  laptop.scale = 1.65;
  laptop.current = {.width = 2560, .height = 1600, .refreshMilliHz = 240000};
  laptop.vrrEnabled = true;

  display_config::Output tv;
  tv.name = "HDMI-A-1";
  tv.enabled = true;
  tv.x = 1552;
  tv.y = 0;
  tv.scale = 1.0;
  tv.current = {.width = 3840, .height = 2160, .refreshMilliHz = 60000};

  display_config::Output unused;
  unused.name = "DP-2";
  unused.enabled = false;

  const std::vector<display_config::Output> outputs{laptop, tv, unused};
  const auto kdl = compositors::niri::kdlForOutputs(outputs);
  ok = expect(kdl.find("output \"eDP-1\"") != std::string::npos, "writes enabled output") && ok;
  ok = expect(kdl.find("position x=0 y=0") != std::string::npos, "writes laptop position") && ok;
  ok = expect(kdl.find("position x=1552 y=0") != std::string::npos, "writes tv position") && ok;
  ok = expect(kdl.find("variable-refresh-rate") != std::string::npos, "writes vrr") && ok;
  ok = expect(kdl.find("output \"DP-2\"") != std::string::npos && kdl.find("    off") != std::string::npos, "writes off")
      && ok;

  const auto parsed = compositors::niri::parseOutputs(kdl);
  ok = expect(parsed.size() == 3, "parses three output blocks") && ok;
  if (parsed.size() >= 2) {
    ok = expect(parsed[0].name == "eDP-1" && parsed[0].x == 0 && parsed[0].vrrEnabled, "parses laptop block") && ok;
    ok = expect(parsed[1].name == "HDMI-A-1" && parsed[1].x == 1552, "parses tv block") && ok;
  }

  const auto dir = makeTempDir();
  const auto config = dir / "config.kdl";
  {
    std::ofstream out(config);
    out << "output \"eDP-1\" { scale 1; }\n";
  }

  const auto saved = compositors::niri::saveOutputsInto(config, outputs);
  ok = expect(saved.ok, "saves outputs into a temp niri config") && ok;
  const auto includeText = compositors::niri::readFile(dir / "helix-outputs.kdl");
  ok = expect(includeText.find("HDMI-A-1") != std::string::npos, "owned include contains HDMI-A-1") && ok;
  ok = expect(compositors::niri::hasInclude(compositors::niri::readFile(config), "helix-outputs.kdl"), "appends include")
      && ok;
  ok = expect(compositors::niri::savedLayoutMatches(outputs, config), "saved layout matches live snapshot") && ok;

  auto moved = outputs;
  moved[1].x = 2000;
  ok = expect(!compositors::niri::savedLayoutMatches(moved, config), "detects a live layout drift") && ok;

  std::filesystem::remove_all(dir);
  return ok ? 0 : 1;
}
