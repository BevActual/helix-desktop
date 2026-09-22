#include "dbus/kdeconnect/phone_cast_logic.h"

#include "compositors/niri/niri_phone_cast.h"

#include <format>
#include <nlohmann/json.hpp>

namespace phone_cast {

  std::string mirrorWindowTitle(std::string_view label) {
    using compositors::niri::kPhoneMirrorWindowTitle;
    if (label.empty() || label == kPhoneMirrorWindowTitle) {
      return std::string(kPhoneMirrorWindowTitle);
    }
    return std::string(label) + " · " + std::string(kPhoneMirrorWindowTitle);
  }

  std::vector<std::string> scrcpyArgs(
      const std::string& serial, std::string_view windowTitle, bool audio, std::initializer_list<const char*> extra
  ) {
    using compositors::niri::kPhoneMirrorHeight;
    using compositors::niri::kPhoneMirrorWidth;
    std::vector<std::string> args{"scrcpy"};
    if (!serial.empty()) {
      args.emplace_back("-s");
      args.push_back(serial);
    }
    for (const char* flag : extra) {
      args.emplace_back(flag);
    }
    if (!audio) {
      args.emplace_back("--no-audio");
    }
    args.push_back(std::format("--window-width={}", kPhoneMirrorWidth));
    args.push_back(std::format("--window-height={}", kPhoneMirrorHeight));
    args.emplace_back("--window-borderless");
    args.emplace_back("--always-on-top");
    args.emplace_back("--background-color=#000000");
    args.push_back("--window-title=" + mirrorWindowTitle(windowTitle));
    return args;
  }

  std::optional<bool> airplayStreamingEdge(std::string_view line) {
    if (line.find("raop_rtp_mirror starting mirroring") != std::string_view::npos) {
      return true;
    }
    if (line.find("TEARDOWN") != std::string_view::npos || line.find("Connection closed") != std::string_view::npos
        || line.find("raop_rtp_mirror stopped") != std::string_view::npos) {
      return false;
    }
    return std::nullopt;
  }

  PointerStatus parsePointerStatus(const std::optional<nlohmann::json>& reply, const PointerStatus& previous) {
    PointerStatus status = previous;
    if (!reply.has_value() || !reply->is_object()) {
      status.running = false;
      status.connected = false;
      status.keyboard = false;
      status.ancsAttached = false;
      status.ancsActive = 0;
      return status;
    }
    status.running = true;
    status.connected = reply->value("connected", false);
    status.keyboard = reply->value("keyboard_captured", false);
    status.configOk = reply->value("bluetooth_config_ok", true);
    status.bonded = reply->value("bonded", false);
    if (const auto ancs = reply->find("ancs"); ancs != reply->end() && ancs->is_object()) {
      status.ancsEnabled = ancs->value("enabled", true);
      status.ancsAttached = ancs->value("attached", false);
      status.ancsActive = ancs->value("active", 0);
      status.ancsPosted = ancs->value("posted", 0);
    } else {
      status.ancsAttached = false;
      status.ancsActive = 0;
    }
    return status;
  }

  const char* pointerStatusKey(bool running, bool bondLost, bool connected, bool keyboard) {
    if (!running) {
      return "control-center.phone.cast-pointer-starting";
    }
    if (bondLost) {
      return "control-center.phone.cast-pointer-lost";
    }
    if (!connected) {
      return "control-center.phone.cast-pointer-waiting";
    }
    return keyboard ? "control-center.phone.cast-pointer-typing" : "control-center.phone.cast-pointer-connected";
  }

  const char* notificationsStatusKey(bool wanted, bool running, bool ancsAttached, int ancsPosted) {
    if (!wanted) {
      return nullptr;
    }
    if (!running) {
      return "control-center.phone.cast-notifications-starting";
    }
    if (!ancsAttached) {
      return "control-center.phone.cast-notifications-waiting";
    }
    if (ancsPosted == 0) {
      return "control-center.phone.cast-notifications-allow";
    }
    return "control-center.phone.cast-notifications-syncing";
  }

  CastRows castRows(
      bool androidPeer, bool applePeer, bool toolsReady, bool adbConnected, bool airplayRunning, bool hasApps
  ) {
    CastRows rows;
    rows.adb = androidPeer && toolsReady;
    rows.audio = rows.adb;
    rows.mirror = (androidPeer && toolsReady && adbConnected) || (applePeer && toolsReady);
    rows.search = androidPeer && rows.mirror && hasApps;
    rows.alwaysOn = applePeer && toolsReady;
    rows.notifications = applePeer && toolsReady;
    rows.pointer = applePeer && airplayRunning;
    return rows;
  }

} // namespace phone_cast
