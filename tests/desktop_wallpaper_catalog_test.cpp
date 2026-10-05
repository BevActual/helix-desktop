#include "apps/settings/wallpaper_catalog.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

int main() {
  char pattern[] = "/tmp/helix-wallpapers-XXXXXX";
  const auto* directory = mkdtemp(pattern); assert(directory);
  const std::filesystem::path root(directory);
  std::filesystem::create_directories(root / "nested");
  std::filesystem::create_directories(root / ".hidden");
  for (const auto& name : {"a.png", "B.JPG", "notes.txt", ".private.png", "nested/c.webp", ".hidden/d.png"})
    std::ofstream(root / name) << "fixture";
  std::filesystem::create_directory_symlink(root, root / "nested/loop");
  std::filesystem::create_symlink(root / "a.png", root / "alias.png");
  auto files = helix::settings::scanWallpapers(root, false);
  assert(!files.unavailable && files.files.size() == 2);
  assert(files.files[0].filename() == "B.JPG");
  files = helix::settings::scanWallpapers(root, true);
  assert(!files.unavailable && files.files.size() == 3);
  assert(helix::settings::scanWallpapers(root / "missing", true).unavailable);
  assert(helix::settings::scanWallpapers(root / "a.png", true).unavailable);
  assert(helix::settings::scanWallpapers(root, true, [] { return true; }).files.empty());
  for (int i = 0; i < 1010; ++i) std::ofstream(root / (std::to_string(i) + ".png"));
  files = helix::settings::scanWallpapers(root, true);
  assert(files.truncated && files.files.size() == 1000);
  std::filesystem::remove_all(root);
}
