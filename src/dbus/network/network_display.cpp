#include "dbus/network/network_display.h"

#include "dbus/network/network_types.h"

namespace network_display {

  const char* glyphForState(const NetworkState& state) noexcept {
    if (state.kind == NetworkConnectivity::Wired) {
      return state.connected ? "ethernet" : "ethernet-off";
    }
    if (state.kind == NetworkConnectivity::Cellular) {
      return state.connected ? "cell-signal-1" : cellularOffGlyph();
    }
    return wifiGlyphForState(state);
  }

  const char* vpnGlyph() noexcept { return "shield-check"; }

  const char* wifiGlyphForState(const NetworkState& state) noexcept {
    if (!state.wirelessEnabled) {
      return "wifi-off";
    }
    if (state.kind == NetworkConnectivity::Unknown) {
      return "wifi-question";
    }
    if (state.kind == NetworkConnectivity::Wireless && state.connected) {
      return wifiGlyphForSignal(state.signalStrength);
    }
    return "wifi-exclamation";
  }

  int wifiSignalBand(std::uint8_t signal) noexcept {
    if (signal >= 80) {
      return 4;
    }
    if (signal >= 60) {
      return 3;
    }
    if (signal >= 35) {
      return 2;
    }
    if (signal >= 15) {
      return 1;
    }
    return 0;
  }

  const char* wifiGlyphForSignal(std::uint8_t signal) noexcept {
    switch (wifiSignalBand(signal)) {
    case 4:
      return "wifi";
    case 3:
      return "wifi-3";
    case 2:
      return "wifi-2";
    case 1:
      return "wifi-1";
    default:
      return "wifi-0";
    }
  }

  WifiRadioBand wifiFrequencyBand(std::uint32_t frequencyMhz) noexcept {
    // Broad envelopes around each band rather than exact channel centers — the input
    // is whatever operating frequency the backend reports. Band membership follows
    // cfg80211: 802.11j 4.9 GHz channels and the U-NII-4/ITS block are 5 GHz, and
    // 5925 MHz is the 5/6 GHz boundary.
    if (frequencyMhz >= 2400 && frequencyMhz <= 2500) {
      return WifiRadioBand::Ghz24;
    }
    if (frequencyMhz >= 4900 && frequencyMhz <= 5924) {
      return WifiRadioBand::Ghz5;
    }
    if (frequencyMhz >= 5925 && frequencyMhz <= 7125) {
      return WifiRadioBand::Ghz6;
    }
    if (frequencyMhz >= 57000 && frequencyMhz <= 71000) {
      return WifiRadioBand::Ghz60;
    }
    return WifiRadioBand::Unknown;
  }

  const char* wifiFrequencyBandLabel(std::uint32_t frequencyMhz) noexcept {
    switch (wifiFrequencyBand(frequencyMhz)) {
    case WifiRadioBand::Ghz24:
      return "2.4 GHz";
    case WifiRadioBand::Ghz5:
      return "5 GHz";
    case WifiRadioBand::Ghz6:
      return "6 GHz";
    case WifiRadioBand::Ghz60:
      return "60 GHz";
    case WifiRadioBand::Unknown:
      break;
    }
    return nullptr;
  }

  const char* cellularGlyphForSignal(std::uint8_t signal) noexcept {
    // cell-signal-* rather than antenna-bars-*: the antenna-bars outlines in the
    // vendored tabler.ttf are mangled and render as dots, while cell-signal
    // renders correctly.
    switch (wifiSignalBand(signal)) {
    case 4:
      return "cell-signal-5";
    case 3:
      return "cell-signal-4";
    case 2:
      return "cell-signal-3";
    case 1:
      return "cell-signal-2";
    default:
      return "cell-signal-1";
    }
  }

  const char* cellularOffGlyph() noexcept { return "cell-signal-off"; }

} // namespace network_display
