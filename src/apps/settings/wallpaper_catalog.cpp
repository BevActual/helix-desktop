#include "apps/settings/wallpaper_catalog.h"
#include <algorithm>
#include <cctype>
#include <string>

namespace helix::settings {
WallpaperCatalog scanWallpapers(const std::filesystem::path& directory, bool recursive,
                                const std::function<bool()>& cancelled) {
  WallpaperCatalog result;
  std::error_code error;
  std::filesystem::recursive_directory_iterator it(directory, error), end;
  if (error) { result.unavailable = true; return result; }
  std::size_t visited = 0;
  while (it != end) {
    if (cancelled && cancelled()) return {};
    if (++visited > 8192 || result.files.size() >= 1000) { result.truncated = true; break; }
    const auto path = it->path();
    const auto name = path.filename().string();
    const auto status = it->symlink_status(error);
    if (error) { result.unavailable = true; error.clear(); }
    else if (name.starts_with('.') || std::filesystem::is_symlink(status)) it.disable_recursion_pending();
    else if (std::filesystem::is_directory(status)) {
      if (!recursive || it.depth() >= 8) it.disable_recursion_pending();
    } else if (std::filesystem::is_regular_file(status)) {
      auto extension = path.extension().string();
      std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return std::tolower(c); });
      if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".webp" ||
          extension == ".avif" || extension == ".jxl" || extension == ".heic" || extension == ".bmp" || extension == ".tiff")
        result.files.push_back(path);
    }
    it.increment(error);
    if (error) { result.unavailable = true; break; }
  }
  std::ranges::sort(result.files);
  return result;
}
} // namespace helix::settings
