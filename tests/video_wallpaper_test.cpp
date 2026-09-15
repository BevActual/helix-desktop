#include "shell/wallpaper/video_wallpaper.h"

#include <algorithm>
#include <print>
#include <ranges>
#include <string>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "video_wallpaper_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

} // namespace

int main() {
  using namespace wallpaper::video;
  bool ok = true;

  ok = expect(helpSupportsAutoMode("Usage: mpvpaper [--auto-mode FULL]"), "detects --auto-mode") && ok;
  ok = expect(!helpSupportsAutoMode("Usage: mpvpaper [--auto-pause]"), "missing --auto-mode is false") && ok;

  LaunchSpec spec;
  spec.connector = "eDP-1";
  spec.path = "/tmp/clip.mp4";
  spec.mute = true;
  spec.hardwareDecode = true;
  spec.autoPause = WallpaperVideoAutoPause::Full;
  spec.autoModeSupported = true;
  auto cmd = buildCommand(spec);
  ok = expect(!cmd.empty() && cmd.front() == "mpvpaper", "command starts with mpvpaper") && ok;
  ok = expect(std::ranges::find(cmd, "--auto-pause") != cmd.end(), "full auto-pause with support") && ok;
  ok = expect(std::ranges::find(cmd, "FULL") != cmd.end(), "full auto-mode token") && ok;
  ok = expect(std::ranges::find(cmd, "eDP-1") != cmd.end(), "connector is forwarded") && ok;
  ok = expect(std::ranges::find(cmd, "/tmp/clip.mp4") != cmd.end(), "path is forwarded") && ok;
  ok = expect(mpvOptionString(spec).find("no-audio") != std::string::npos, "mute adds no-audio") && ok;
  ok = expect(mpvOptionString(spec).find("hwdec=auto") != std::string::npos, "hwdec auto") && ok;

  spec.autoModeSupported = false;
  cmd = buildCommand(spec);
  ok = expect(std::ranges::find(cmd, "--auto-mode") == cmd.end(), "old mpvpaper has no --auto-mode") && ok;

  spec.autoPause = WallpaperVideoAutoPause::Off;
  spec.mute = false;
  spec.hardwareDecode = false;
  spec.extraMpvOptions = "video-zoom=0.1";
  ok = expect(autoPauseArgs(spec).empty(), "auto-pause off emits nothing") && ok;
  ok = expect(mpvOptionString(spec).find("no-audio") == std::string::npos, "unmuted omits no-audio") && ok;
  ok = expect(mpvOptionString(spec).find("hwdec=no") != std::string::npos, "software decode") && ok;
  ok = expect(mpvOptionString(spec).find("video-zoom=0.1") != std::string::npos, "extra mpv options") && ok;

  const auto parsed = parseAssignmentsJson(R"({"assignments":{"eDP-1":"/a.mp4","*":"/all.mp4"}})");
  ok = expect(parsed.size() == 2, "parses assignment map") && ok;
  ok = expect(parsed.at("eDP-1") == "/a.mp4", "parses connector path") && ok;
  const auto legacy = parseAssignmentsJson(R"({"HDMI-A-1":"/legacy.mp4"})");
  ok = expect(legacy.size() == 1 && legacy.at("HDMI-A-1") == "/legacy.mp4", "parses plugin-style map") && ok;
  const auto encoded = serializeAssignmentsJson({{"eDP-1", "/b.webm"}});
  ok = expect(encoded.find("eDP-1") != std::string::npos && encoded.find("/b.webm") != std::string::npos,
              "serializes assignments") && ok;

  if (!ok) {
    return 1;
  }
  return 0;
}
