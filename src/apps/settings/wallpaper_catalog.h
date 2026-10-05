#pragma once
#include <filesystem>
#include <functional>
#include <vector>

namespace helix::settings {
struct WallpaperCatalog {
  std::vector<std::filesystem::path> files;
  bool unavailable = false;
  bool truncated = false;
};
// A local, bounded catalog. Hidden files and symlinks are omitted; thumbnails
// perform their own decoding and size validation before an image is displayed.
WallpaperCatalog scanWallpapers(const std::filesystem::path& directory, bool recursive,
                                const std::function<bool()>& cancelled = {});
} // namespace helix::settings
