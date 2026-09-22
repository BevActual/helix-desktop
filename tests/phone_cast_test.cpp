// Smoke test for the phone mirror decisions shared by PhoneConnectService and the
// Control Center Phone tab: scrcpy argv, UxPlay session edges, the Helix Phone status
// reply, and the row visibility / wording the Cast card derives from them.
#include "dbus/kdeconnect/phone_cast_logic.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "phone_cast_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

  bool has(const std::vector<std::string>& args, std::string_view flag) {
    return std::ranges::find(args, flag) != args.end();
  }

  bool testScrcpyArgs() {
    using phone_cast::scrcpyArgs;
    bool ok = true;

    const auto full = scrcpyArgs("ABC123", "Messages", true, {"--max-size=1080"});
    ok = expect(full.front() == "scrcpy", "argv starts with scrcpy") && ok;
    ok = expect(has(full, "-s") && has(full, "ABC123"), "serial is passed with -s") && ok;
    ok = expect(has(full, "--max-size=1080"), "extra flags are forwarded") && ok;
    ok = expect(!has(full, "--no-audio"), "audio on leaves scrcpy's default forwarding") && ok;
    ok = expect(has(full, "--window-width=390") && has(full, "--window-height=844"), "phone silhouette size") && ok;
    ok = expect(has(full, "--window-borderless") && has(full, "--always-on-top"), "floating window flags") && ok;
    ok = expect(has(full, "--window-title=Messages · Helix Phone"), "app title joins the mirror title") && ok;

    const auto muted = scrcpyArgs("", "", false, {});
    ok = expect(!has(muted, "-s"), "empty serial omits -s") && ok;
    ok = expect(has(muted, "--no-audio"), "audio off adds --no-audio") && ok;
    ok = expect(has(muted, "--window-title=Helix Phone"), "empty label uses the mirror title") && ok;

    ok = expect(phone_cast::mirrorWindowTitle("Helix Phone") == "Helix Phone", "mirror title is not doubled") && ok;
    return ok;
  }

  bool testAirplayEdges() {
    using phone_cast::airplayStreamingEdge;
    bool ok = true;
    ok = expect(
        airplayStreamingEdge("raop_rtp_mirror starting mirroring") == true, "mirror stream open starts streaming"
    ) && ok;
    ok = expect(airplayStreamingEdge("TEARDOWN rtsp://…") == false, "TEARDOWN stops streaming") && ok;
    ok = expect(airplayStreamingEdge("Connection closed for socket 7") == false, "socket close stops") && ok;
    ok = expect(airplayStreamingEdge("raop_rtp_mirror stopped") == false, "mirror stop stops") && ok;
    ok = expect(!airplayStreamingEdge("Client connected 10.0.0.5").has_value(), "client connect is not an edge") && ok;
    ok = expect(!airplayStreamingEdge("").has_value(), "empty line is not an edge") && ok;
    return ok;
  }

  bool testPointerStatus() {
    using phone_cast::parsePointerStatus;
    using phone_cast::PointerStatus;
    bool ok = true;

    const PointerStatus idle;
    const auto down = parsePointerStatus(std::nullopt, PointerStatus{.running = true, .connected = true, .bonded = true});
    ok = expect(!down.running && !down.connected && !down.keyboard, "no reply clears the live flags") && ok;
    ok = expect(down.bonded, "no reply keeps the bond") && ok;
    ok = expect(!down.ancsAttached && down.ancsActive == 0, "no reply detaches ANCS") && ok;

    const nlohmann::json connected = {
        {"connected", true},
        {"keyboard_captured", true},
        {"bluetooth_config_ok", false},
        {"bonded", true},
        {"ancs", {{"enabled", true}, {"attached", true}, {"active", 3}, {"posted", 12}}},
    };
    const auto live = parsePointerStatus(connected, idle);
    ok = expect(live.running && live.connected && live.keyboard, "reply sets running/connected/keyboard") && ok;
    ok = expect(!live.configOk && live.bonded, "reply carries bluetoothd config and bond") && ok;
    ok = expect(live.ancsEnabled && live.ancsAttached && live.ancsActive == 3 && live.ancsPosted == 12, "ANCS fields")
        && ok;

    const auto legacy = parsePointerStatus(nlohmann::json{{"connected", false}}, live);
    ok = expect(legacy.running && !legacy.connected, "legacy reply without ancs still counts as running") && ok;
    ok = expect(!legacy.ancsAttached && legacy.ancsActive == 0, "legacy reply detaches ANCS") && ok;
    ok = expect(legacy.configOk, "missing bluetooth_config_ok defaults to ok") && ok;
    return ok;
  }

  bool testWording() {
    using phone_cast::notificationsStatusKey;
    using phone_cast::pointerStatusKey;
    bool ok = true;

    ok = expect(std::string_view(pointerStatusKey(false, false, false, false)).ends_with("-starting"), "pointer starting")
        && ok;
    ok = expect(std::string_view(pointerStatusKey(true, true, false, false)).ends_with("-lost"), "bond lost wins") && ok;
    ok = expect(std::string_view(pointerStatusKey(true, false, false, false)).ends_with("-waiting"), "pointer waiting")
        && ok;
    ok = expect(std::string_view(pointerStatusKey(true, false, true, true)).ends_with("-typing"), "pointer typing") && ok;
    ok = expect(
        std::string_view(pointerStatusKey(true, false, true, false)).ends_with("-connected"), "pointer connected"
    ) && ok;

    ok = expect(notificationsStatusKey(false, true, true, 5) == nullptr, "notifications off hides the status") && ok;
    ok = expect(std::string_view(notificationsStatusKey(true, false, false, 0)).ends_with("-starting"), "ANCS starting")
        && ok;
    ok = expect(std::string_view(notificationsStatusKey(true, true, false, 0)).ends_with("-waiting"), "ANCS waiting")
        && ok;
    ok = expect(
        std::string_view(notificationsStatusKey(true, true, true, 0)).ends_with("-allow"),
        "attached but nothing posted asks for Share System Notifications"
    ) && ok;
    ok = expect(std::string_view(notificationsStatusKey(true, true, true, 1)).ends_with("-syncing"), "ANCS syncing")
        && ok;
    return ok;
  }

  bool testCastRows() {
    using phone_cast::castRows;
    using phone_cast::CastRows;
    bool ok = true;

    ok = expect(castRows(false, false, true, true, true, true) == CastRows{}, "no supported peer shows nothing") && ok;
    ok = expect(castRows(true, false, false, true, false, true) == CastRows{}, "android without tools shows nothing")
        && ok;

    const auto androidReady = castRows(true, false, true, false, false, false);
    ok = expect(androidReady.adb && androidReady.audio, "android with tools shows adb and audio rows") && ok;
    ok = expect(!androidReady.mirror && !androidReady.search, "android needs adb before mirroring") && ok;
    ok = expect(!androidReady.alwaysOn && !androidReady.notifications && !androidReady.pointer, "no iPhone rows")
        && ok;

    const auto androidLive = castRows(true, false, true, true, false, true);
    ok = expect(androidLive.mirror && androidLive.search, "adb connected with apps shows mirror and search") && ok;
    ok = expect(!castRows(true, false, true, true, false, false).search, "no apps hides search") && ok;

    const auto apple = castRows(false, true, true, false, false, false);
    ok = expect(apple.mirror && apple.alwaysOn && apple.notifications, "iPhone with tools shows mirror and toggles")
        && ok;
    ok = expect(!apple.adb && !apple.audio && !apple.search, "iPhone has no android rows") && ok;
    ok = expect(!apple.pointer, "pointer well waits for the receiver") && ok;
    ok = expect(castRows(false, true, true, false, true, false).pointer, "receiver up shows the pointer well") && ok;
    ok = expect(!castRows(false, true, false, false, true, false).notifications, "iPhone without tools hides toggles")
        && ok;
    return ok;
  }

} // namespace

int main() {
  bool ok = true;
  ok = testScrcpyArgs() && ok;
  ok = testAirplayEdges() && ok;
  ok = testPointerStatus() && ok;
  ok = testWording() && ok;
  ok = testCastRows() && ok;
  if (!ok) {
    return 1;
  }
  std::println("phone_cast: all checks passed");
  return 0;
}
