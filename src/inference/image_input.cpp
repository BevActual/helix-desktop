#include "inference/image_input.h"

#include "wuffs-v0.4.c"  // declarations only: the implementation is compiled once, in core/image_decoder.cpp

#include <algorithm>
#include <cstring>
#include <memory>

namespace helix::inference {

namespace {

bool starts(const std::uint8_t* data, std::size_t size, const char* magic, std::size_t length) {
  return size >= length && std::memcmp(data, magic, length) == 0;
}

// What the bytes claim to be; anything else (JPEG XL, ICO, SVG, TIFF…) is not offered to an engine.
bool supported(const std::uint8_t* data, std::size_t size) {
  return starts(data, size, "\x89PNG\r\n\x1a\n", 8) || starts(data, size, "\xff\xd8\xff", 3) || starts(data, size, "GIF8", 4) ||
         starts(data, size, "BM", 2) || (starts(data, size, "RIFF", 4) && size >= 12 && std::memcmp(data + 8, "WEBP", 4) == 0);
}

// Straight RGBA out of Wuffs, with the size checked after the header is read and before pixel memory exists.
class CappedCallbacks final : public wuffs_aux::DecodeImageCallbacks {
public:
  wuffs_base__pixel_format SelectPixfmt(const wuffs_base__image_config&) override {
    return wuffs_base__make_pixel_format(WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL);
  }
  AllocPixbufResult AllocPixbuf(const wuffs_base__image_config& config, bool allowUninitialized) override {
    const std::uint32_t width = config.pixcfg.width();
    const std::uint32_t height = config.pixcfg.height();
    if (width == 0 || height == 0) return AllocPixbufResult(std::string("the picture is empty"));
    if (width > kMaxImageSide || height > kMaxImageSide || static_cast<std::uint64_t>(width) * height > kMaxImagePixels)
      return AllocPixbufResult(std::string("the picture is too large"));
    return wuffs_aux::DecodeImageCallbacks::AllocPixbuf(config, allowUninitialized);
  }
};

}  // namespace

ImageInput scaleDown(const ImageInput& image, std::uint32_t maxSide) {
  if (image.width == 0 || image.height == 0 || (image.width <= maxSide && image.height <= maxSide)) return image;
  const double factor = static_cast<double>(maxSide) / std::max(image.width, image.height);
  ImageInput out;
  out.width = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(image.width * factor));
  out.height = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(image.height * factor));
  out.rgb.resize(static_cast<std::size_t>(out.width) * out.height * 3);
  for (std::uint32_t y = 0; y < out.height; ++y) {
    const std::uint32_t y0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * image.height / out.height);
    const std::uint32_t y1 = std::max(y0 + 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(y + 1) * image.height / out.height));
    for (std::uint32_t x = 0; x < out.width; ++x) {
      const std::uint32_t x0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * image.width / out.width);
      const std::uint32_t x1 = std::max(x0 + 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(x + 1) * image.width / out.width));
      std::uint64_t sum[3] = {0, 0, 0};
      for (std::uint32_t sy = y0; sy < y1 && sy < image.height; ++sy)
        for (std::uint32_t sx = x0; sx < x1 && sx < image.width; ++sx) {
          const std::uint8_t* pixel = &image.rgb[(static_cast<std::size_t>(sy) * image.width + sx) * 3];
          sum[0] += pixel[0]; sum[1] += pixel[1]; sum[2] += pixel[2];
        }
      const std::uint64_t count = static_cast<std::uint64_t>(y1 - y0) * (x1 - x0);
      std::uint8_t* target = &out.rgb[(static_cast<std::size_t>(y) * out.width + x) * 3];
      for (int c = 0; c < 3; ++c) target[c] = static_cast<std::uint8_t>((sum[c] + count / 2) / count);
    }
  }
  return out;
}

std::expected<ImageInput, std::string> decodeImageInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size == 0) return std::unexpected("the picture is empty");
  if (size > kMaxImageBytes) return std::unexpected("the picture is larger than 8 MiB");
  if (!supported(data, size)) return std::unexpected("unsupported picture format (use PNG, JPEG, GIF, BMP or WebP)");

  CappedCallbacks callbacks;
  wuffs_aux::sync_io::MemoryInput input(data, size);
  auto result = wuffs_aux::DecodeImage(callbacks, input);
  if (!result.error_message.empty()) return std::unexpected("the picture could not be decoded: " + result.error_message);
  const auto plane = result.pixbuf.plane(0);
  const std::uint32_t width = result.pixbuf.pixcfg.width();
  const std::uint32_t height = result.pixbuf.pixcfg.height();
  if (plane.ptr == nullptr || width == 0 || height == 0) return std::unexpected("the picture has no pixels");

  ImageInput image;
  image.width = width;
  image.height = height;
  image.rgb.resize(static_cast<std::size_t>(width) * height * 3);
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint8_t* row = plane.ptr + static_cast<std::size_t>(y) * plane.stride;
    std::uint8_t* target = &image.rgb[static_cast<std::size_t>(y) * width * 3];
    for (std::uint32_t x = 0; x < width; ++x) {
      const unsigned alpha = row[x * 4 + 3];
      for (int c = 0; c < 3; ++c)  // straight alpha over white
        target[x * 3 + c] = static_cast<std::uint8_t>((row[x * 4 + c] * alpha + 255U * (255U - alpha) + 127U) / 255U);
    }
  }
  return scaleDown(image, kEngineImageSide);
}

}  // namespace helix::inference
