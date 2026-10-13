#include "util/qr_code.h"

#include <qrcodegen.hpp>
#include <stdexcept>
#include <string>

namespace qr_code {

  std::optional<Bitmap> encodeRgba(std::string_view text, int modulePixels, int quietModules) {
    if (modulePixels <= 0 || quietModules < 0) {
      return std::nullopt;
    }
    try {
      const std::string payload(text);
      const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(payload.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
      const int modules = qr.getSize();
      if (modules <= 0) {
        return std::nullopt;
      }
      const int extent = modules + (quietModules * 2);
      const int pixels = extent * modulePixels;
      Bitmap bitmap;
      bitmap.modules = modules;
      bitmap.pixels = pixels;
      bitmap.rgba.resize(static_cast<std::size_t>(pixels) * static_cast<std::size_t>(pixels) * 4U, 255);

      const auto setPixel = [&bitmap, pixels](int x, int y, bool dark) {
        if (x < 0 || y < 0 || x >= pixels || y >= pixels) {
          return;
        }
        const std::size_t index =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(pixels) + static_cast<std::size_t>(x)) * 4U;
        const std::uint8_t channel = dark ? std::uint8_t{0} : std::uint8_t{255};
        bitmap.rgba[index] = channel;
        bitmap.rgba[index + 1] = channel;
        bitmap.rgba[index + 2] = channel;
        bitmap.rgba[index + 3] = 255;
      };

      for (int moduleY = 0; moduleY < modules; ++moduleY) {
        for (int moduleX = 0; moduleX < modules; ++moduleX) {
          if (!qr.getModule(moduleX, moduleY)) {
            continue;
          }
          const int originX = (moduleX + quietModules) * modulePixels;
          const int originY = (moduleY + quietModules) * modulePixels;
          for (int py = 0; py < modulePixels; ++py) {
            for (int px = 0; px < modulePixels; ++px) {
              setPixel(originX + px, originY + py, true);
            }
          }
        }
      }
      return bitmap;
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }

} // namespace qr_code
