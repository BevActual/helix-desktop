#include "compositors/niri/niri_config_blur.h"

#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "niri_config_blur_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

  std::filesystem::path makeTempDir() {
    auto path = std::filesystem::temp_directory_path() / ("noctalia-niri-blur-" + std::to_string(::getpid()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
  }

} // namespace

int main() {
  bool ok = true;

  ok = expect(compositors::niri::versionSupportsBlur("26.04"), "26.04 supports blur") && ok;
  ok = expect(compositors::niri::versionSupportsBlur("26.4"), "26.4 supports blur") && ok;
  ok = expect(compositors::niri::versionSupportsBlur("niri 26.04"), "prefixed 26.04 supports blur") && ok;
  ok = expect(compositors::niri::versionSupportsBlur("27.01"), "27.01 supports blur") && ok;
  ok = expect(!compositors::niri::versionSupportsBlur("25.08"), "25.08 does not support blur") && ok;
  ok = expect(!compositors::niri::versionSupportsBlur("26.03"), "26.03 does not support blur") && ok;
  ok = expect(!compositors::niri::versionSupportsBlur(""), "empty version does not support blur") && ok;
  ok = expect(!compositors::niri::versionSupportsBlur("abc"), "non-version does not support blur") && ok;

  ok = expect(
      compositors::niri::versionSupportsLiquidGlass("26.04 helix (abc)"), "helix-niri 26.04 supports liquid-glass"
  ) && ok;
  ok = expect(
      compositors::niri::versionSupportsLiquidGlass("niri 26.04 helix (deadbeef)"),
      "prefixed helix version supports liquid-glass"
  ) && ok;
  ok = expect(!compositors::niri::versionSupportsLiquidGlass("26.04"), "stock niri does not support liquid-glass")
      && ok;
  ok = expect(
      !compositors::niri::versionSupportsLiquidGlass("25.08 helix (abc)"), "old helix-niri without blur does not"
  ) && ok;

  const auto enabled = compositors::niri::kdlForBlur({.enabled = true});
  ok = expect(enabled.find("blur true") != std::string::npos, "enabled kdl requests blur") && ok;
  ok = expect(enabled.find("xray false") != std::string::npos, "enabled kdl disables xray") && ok;
  ok = expect(enabled.find("passes 2") != std::string::npos, "enabled kdl sets default blur passes") && ok;
  ok = expect(enabled.find("offset 3.0") != std::string::npos, "enabled kdl sets default blur offset") && ok;
  ok = expect(enabled.find("noise 0.03") != std::string::npos, "enabled kdl sets default blur noise") && ok;
  ok = expect(enabled.find("noctalia-window-switcher") != std::string::npos, "enabled kdl covers the window switcher")
      && ok;
  ok = expect(enabled.find("liquid-glass {") == std::string::npos, "stock niri never emits liquid-glass") && ok;

  const auto tuned = compositors::niri::kdlForBlur({.enabled = true, .passes = 5, .offset = 4.5F, .noise = 0.08F});
  ok = expect(tuned.find("passes 5") != std::string::npos, "writes configured passes") && ok;
  ok = expect(tuned.find("offset 4.5") != std::string::npos, "writes configured offset") && ok;
  ok = expect(tuned.find("noise 0.08") != std::string::npos, "writes configured noise") && ok;

  const auto clamped = compositors::niri::kdlForBlur({.enabled = true, .passes = 99, .offset = 0.1F, .noise = 1.0F});
  ok = expect(clamped.find("passes 8") != std::string::npos, "clamps passes") && ok;
  ok = expect(clamped.find("offset 0.5") != std::string::npos, "clamps offset") && ok;
  ok = expect(clamped.find("noise 0.20") != std::string::npos, "clamps noise") && ok;

  const auto disabled = compositors::niri::kdlForBlur({.enabled = false});
  ok = expect(disabled.find("blur false") != std::string::npos, "disabled kdl clears blur") && ok;
  ok = expect(disabled.find("^noctalia-") != std::string::npos, "disabled kdl matches all noctalia layers") && ok;
  ok = expect(disabled.find("passes") == std::string::npos, "disabled kdl does not set global blur params") && ok;
  ok = expect(disabled.find("liquid-glass {") == std::string::npos, "disabled kdl never emits liquid-glass") && ok;

  const auto stockRefraction = compositors::niri::kdlForBlur({.enabled = false, .refraction = true, .helixNiri = false});
  ok = expect(stockRefraction.find("liquid-glass {") == std::string::npos, "stock refraction never emits liquid-glass")
      && ok;
  ok = expect(stockRefraction.find("blur true") != std::string::npos, "stock refraction maps to chrome blur") && ok;

  const auto helixRefraction =
      compositors::niri::kdlForBlur({.enabled = false, .refraction = true, .helixNiri = true, .refractionStrength = 0.4F});
  ok = expect(helixRefraction.find("liquid-glass {") != std::string::npos, "helix-niri refraction emits liquid-glass")
      && ok;
  ok = expect(helixRefraction.find("refraction-strength 0.40") != std::string::npos, "helix-niri writes glass strength")
      && ok;
  ok = expect(helixRefraction.find("xray false") != std::string::npos, "chrome refraction samples real backdrop")
      && ok;
  ok = expect(helixRefraction.find("Greeter") != std::string::npos, "helix-niri refraction covers the greeter") && ok;
  ok = expect(helixRefraction.find("xray true") != std::string::npos, "greeter keeps wallpaper xray") && ok;
  ok = expect(helixRefraction.find("idle-grace") != std::string::npos, "helix-niri refraction covers idle-grace") && ok;
  ok = expect(helixRefraction.find("blur false") != std::string::npos, "helix-niri refraction turns client blur off")
      && ok;
  ok = expect(helixRefraction.find("blur true") == std::string::npos, "helix-niri refraction does not stack niri blur")
      && ok;

  const auto listedParams = compositors::niri::kdlForBlur(
      {.enabled = false,
       .refraction = true,
       .helixNiri = true,
       .refractionStrength = 0.4F,
       .liquidGlassParams = {"refraction-strength", "glow-weight"}}
  );
  ok = expect(listedParams.find("refraction-strength 0.40") != std::string::npos, "listed param is written") && ok;

  const auto unlistedParams = compositors::niri::kdlForBlur(
      {.enabled = false,
       .refraction = true,
       .helixNiri = true,
       .refractionStrength = 0.4F,
       .liquidGlassParams = {"glow-weight"}}
  );
  ok = expect(unlistedParams.find("liquid-glass {") != std::string::npos, "unlisted param keeps liquid-glass block")
      && ok;
  ok = expect(unlistedParams.find("refraction-strength") == std::string::npos, "unlisted param is not written") && ok;

  const auto helixBlurAndGlass = compositors::niri::kdlForBlur(
      {.enabled = true, .refraction = true, .helixNiri = true, .refractionStrength = 0.4F}
  );
  ok = expect(helixBlurAndGlass.find("liquid-glass {") != std::string::npos, "helix-niri layout blur keeps liquid-glass")
      && ok;
  ok = expect(helixBlurAndGlass.find("blur true") != std::string::npos, "helix-niri layout blur still requests blur")
      && ok;

  const auto helixOff = compositors::niri::kdlForBlur({.enabled = true, .refraction = false, .helixNiri = true});
  ok = expect(helixOff.find("liquid-glass {") == std::string::npos, "helix-niri without refraction omits liquid-glass")
      && ok;

  const auto helixSolid =
      compositors::niri::kdlForBlur({.enabled = false, .refraction = true, .helixNiri = true, .refractionStrength = 0.0F});
  ok = expect(helixSolid.find("liquid-glass {") == std::string::npos, "solid chrome omits liquid-glass") && ok;

  const auto helixContrast =
      compositors::niri::kdlForBlur({.enabled = false, .refraction = true, .helixNiri = true, .highContrast = true});
  ok = expect(helixContrast.find("liquid-glass {") == std::string::npos, "high contrast omits liquid-glass") && ok;

  const auto dir = makeTempDir();
  const auto config = dir / "config.kdl";
  {
    std::ofstream out(config);
    out << "spawn-at-startup \"noctalia\"\n";
  }

  ok = expect(!compositors::niri::blurIncludeExists(config), "no include before save") && ok;
  const auto saved = compositors::niri::saveNiriBlurInto(config, {.enabled = true});
  ok = expect(saved.ok, "saves blur into a temp niri config") && ok;
  ok = expect(compositors::niri::blurIncludeExists(config), "creates noctalia-blur.kdl") && ok;
  ok = expect(
      compositors::niri::hasInclude(compositors::niri::readFile(config), "noctalia-blur.kdl"), "appends include"
  ) && ok;

  const auto includeText = compositors::niri::readFile(dir / "noctalia-blur.kdl");
  ok = expect(includeText.find("blur true") != std::string::npos, "owned file has enabled blur") && ok;

  const auto off = compositors::niri::saveNiriBlurInto(config, {.enabled = false});
  ok = expect(off.ok, "rewrites blur include when disabled") && ok;
  const auto offText = compositors::niri::readFile(dir / "noctalia-blur.kdl");
  ok = expect(offText.find("blur false") != std::string::npos, "owned file has disabled blur") && ok;

  std::filesystem::remove_all(dir);
  return ok ? 0 : 1;
}
