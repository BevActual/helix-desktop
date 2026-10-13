#pragma once

#include "inference/backend.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

namespace helix::inference {

// Limits for pictures a client may send. The bytes are untrusted: they are sniffed, bounded and decoded with Wuffs (memory
// safe) before any engine sees them.
inline constexpr std::size_t kMaxImageBytes = 8U * 1024 * 1024;   // one encoded picture
inline constexpr std::size_t kMaxImages = 4;                      // per request
inline constexpr std::size_t kMaxImageBytesTotal = 16U * 1024 * 1024;
inline constexpr std::uint32_t kMaxImageSide = 12000;             // refused when the header says larger
inline constexpr std::uint64_t kMaxImagePixels = 50'000'000;      // refused before any pixel memory is allocated
inline constexpr std::uint32_t kEngineImageSide = 1568;           // larger pictures are scaled down (area average) to this

// PNG, JPEG, GIF (first frame), BMP and WebP only. Transparency is composited over white. Returns the picture ready for an engine, or why not.
[[nodiscard]] std::expected<ImageInput, std::string> decodeImageInput(const std::uint8_t* data, std::size_t size);

// Pure helper (tested separately): scales an RGB picture so neither side exceeds `maxSide`, averaging source pixels.
[[nodiscard]] ImageInput scaleDown(const ImageInput& image, std::uint32_t maxSide);

}  // namespace helix::inference
