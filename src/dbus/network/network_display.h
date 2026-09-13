#pragma once

#include <cstdint>

struct NetworkState;

namespace network_display {

  [[nodiscard]] const char* glyphForState(const NetworkState& state) noexcept;
  [[nodiscard]] const char* vpnGlyph() noexcept;
  [[nodiscard]] const char* wifiGlyphForState(const NetworkState& state) noexcept;
  [[nodiscard]] const char* wifiGlyphForSignal(std::uint8_t signal) noexcept;
  // Signal band 0 (weakest) .. 4 (strongest) — the bands the wifi glyph draws.
  // Signal-ordered UI sorts on this rather than the raw percent, which jitters
  // on every scan update.
  [[nodiscard]] int wifiSignalBand(std::uint8_t signal) noexcept;

  enum class WifiRadioBand : std::uint8_t {
    Unknown = 0,
    Ghz24 = 1,
    Ghz5 = 2,
    Ghz6 = 3,
    Ghz60 = 4,
  };

  // Radio band of an operating frequency. Unknown when the frequency is 0
  // (backends such as iwd never report one) or falls outside the bands below.
  [[nodiscard]] WifiRadioBand wifiFrequencyBand(std::uint32_t frequencyMhz) noexcept;

  // "2.4 GHz", "5 GHz", "6 GHz" or "60 GHz". nullptr when wifiFrequencyBand is Unknown.
  [[nodiscard]] const char* wifiFrequencyBandLabel(std::uint32_t frequencyMhz) noexcept;

  // Cellular (ModemManager) glyphs, banded the same way as wifi.
  [[nodiscard]] const char* cellularGlyphForSignal(std::uint8_t signal) noexcept;
  [[nodiscard]] const char* cellularOffGlyph() noexcept;

} // namespace network_display
