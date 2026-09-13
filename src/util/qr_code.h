#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace qr_code {

  struct Bitmap {
    int modules = 0;
    int pixels = 0;
    std::vector<std::uint8_t> rgba;
  };

  // Encodes `text` as a square RGBA bitmap (black modules on white, with a quiet zone).
  // `modulePixels` is the width of one QR module; `quietModules` is the white border.
  [[nodiscard]] std::optional<Bitmap> encodeRgba(std::string_view text, int modulePixels, int quietModules = 4);

} // namespace qr_code
