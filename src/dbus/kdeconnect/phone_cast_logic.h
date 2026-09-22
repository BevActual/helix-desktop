#pragma once

// Pure decisions behind the phone mirror: scrcpy invocation, UxPlay session edges,
// the Helix Phone status reply, and the Control Center wording for each state.
// Nothing here touches D-Bus, processes, or widgets, so the smoke test can drive it.

#include <initializer_list>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace phone_cast {

  // Window title for the mirror surface; `label` is the app or device shown alongside.
  [[nodiscard]] std::string mirrorWindowTitle(std::string_view label = {});

  // scrcpy argv for an Android mirror. `audio` false adds --no-audio (Android 11+
  // forwards audio by default and older phones quietly carry on without it).
  [[nodiscard]] std::vector<std::string> scrcpyArgs(
      const std::string& serial, std::string_view windowTitle, bool audio, std::initializer_list<const char*> extra
  );

  // UxPlay log line → streaming edge. true when the iPhone's RTP mirror stream opens,
  // false on TEARDOWN / connection close / mirror stop, nullopt for everything else.
  [[nodiscard]] std::optional<bool> airplayStreamingEdge(std::string_view line);

  // What helix-phone-hid.py reports on its control socket.
  struct PointerStatus {
    bool running = false;    // socket answered
    bool connected = false;  // iPhone subscribed to input reports
    bool keyboard = false;   // mirror holds the keyboard
    bool configOk = true;    // bluetoothd Privacy + JustWorksRepairing set
    bool bonded = false;     // some device is paired with the adapter
    bool ancsEnabled = true; // script posts iPhone notifications
    bool ancsAttached = false;
    int ancsActive = 0;
    int ancsPosted = 0;

    bool operator==(const PointerStatus&) const = default;
  };

  // nullopt means the script did not answer: not running, everything cleared.
  // `previous` keeps configOk/bonded/ancsEnabled when the reply omits them.
  [[nodiscard]] PointerStatus parsePointerStatus(const std::optional<nlohmann::json>& reply, const PointerStatus& previous);

  // Control Center wording. Keys under control-center.phone.*; nullptr means no row.
  [[nodiscard]] const char* pointerStatusKey(bool running, bool bondLost, bool connected, bool keyboard);
  [[nodiscard]] const char* notificationsStatusKey(bool wanted, bool running, bool ancsAttached, int ancsPosted);

  // Which Cast rows show for a peer.
  struct CastRows {
    bool adb = false;           // wireless debugging host/pair
    bool mirror = false;        // Cast / Start AirPlay button
    bool search = false;        // app search field
    bool alwaysOn = false;      // Keep receiver ready
    bool notifications = false; // iPhone notifications
    bool pointer = false;       // Helix Phone pointer well
    bool audio = false;         // Play phone audio here

    bool operator==(const CastRows&) const = default;
  };

  [[nodiscard]] CastRows castRows(
      bool androidPeer, bool applePeer, bool toolsReady, bool adbConnected, bool airplayRunning, bool hasApps
  );

} // namespace phone_cast
