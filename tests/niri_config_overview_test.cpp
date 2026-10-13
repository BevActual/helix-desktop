#include "compositors/niri/niri_config_overview.h"

#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "niri_config_overview_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

  std::filesystem::path makeTempDir() {
    auto path = std::filesystem::temp_directory_path() / ("helix-niri-overview-" + std::to_string(::getpid()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
  }

} // namespace

int main() {
  bool ok = true;

  ok = expect(compositors::niri::overviewWantsBackdrop(NiriOverviewPreset::Backdrop), "backdrop preset wants backdrop")
      && ok;
  ok = expect(!compositors::niri::overviewWantsBackdrop(NiriOverviewPreset::Stationary), "stationary does not") && ok;
  ok = expect(!compositors::niri::overviewWantsBackdrop(NiriOverviewPreset::Flat), "flat does not") && ok;
  ok = expect(!compositors::niri::overviewWantsBackdrop(NiriOverviewPreset::None), "none does not") && ok;

  ok = expect(compositors::niri::sanitizedFlatColorHex("#abc") == "#26233a", "rejects short colors") && ok;
  ok = expect(compositors::niri::sanitizedFlatColorHex("#112233") == "#112233", "keeps valid hex") && ok;

  const auto none = compositors::niri::kdlForOverview({.preset = NiriOverviewPreset::None});
  ok = expect(none.find("layer-rule") == std::string::npos, "none writes no layer-rule") && ok;

  const auto backdrop = compositors::niri::kdlForOverview({.preset = NiriOverviewPreset::Backdrop});
  ok = expect(backdrop.find("helix-backdrop") != std::string::npos, "backdrop matches backdrop layer") && ok;
  ok = expect(backdrop.find("place-within-backdrop") != std::string::npos, "backdrop places within overview") && ok;

  const auto stationary = compositors::niri::kdlForOverview({.preset = NiriOverviewPreset::Stationary});
  ok = expect(stationary.find("helix-wallpaper") != std::string::npos, "stationary matches wallpaper") && ok;
  ok = expect(
           stationary.find("background-color \"transparent\"") != std::string::npos, "stationary clears workspace fill"
       )
      && ok;
  ok = expect(stationary.find("workspace-shadow") != std::string::npos, "stationary turns off workspace shadows") && ok;

  const auto flat = compositors::niri::kdlForOverview({.preset = NiriOverviewPreset::Flat, .flatColorHex = "#ff00aa"});
  ok = expect(flat.find("backdrop-color \"#ff00aa\"") != std::string::npos, "flat uses the palette color") && ok;

  const auto dir = makeTempDir();
  const auto config = dir / "config.kdl";
  {
    std::ofstream out(config);
    out << "spawn-at-startup \"helix\"\n";
  }

  const auto saved = compositors::niri::saveNiriOverviewInto(config, {.preset = NiriOverviewPreset::Stationary});
  ok = expect(saved.ok, "saves overview into a temp niri config") && ok;
  ok = expect(
           compositors::niri::hasInclude(compositors::niri::readFile(config), "helix-overview.kdl"), "appends include"
       )
      && ok;
  const auto includeText = compositors::niri::readFile(dir / "helix-overview.kdl");
  ok = expect(includeText.find("helix-wallpaper") != std::string::npos, "owned file has stationary rules") && ok;

  const auto cleared = compositors::niri::saveNiriOverviewInto(config, {.preset = NiriOverviewPreset::None});
  ok = expect(cleared.ok, "rewrites overview include for none") && ok;
  const auto clearedText = compositors::niri::readFile(dir / "helix-overview.kdl");
  ok = expect(clearedText.find("layer-rule") == std::string::npos, "none overwrite drops layer-rules") && ok;

  std::filesystem::remove_all(dir);
  return ok ? 0 : 1;
}
