#include "theme/adw_gtk3.h"

#include <algorithm>
#include <cstdlib>
#include <ranges>
#include <system_error>

namespace helix::theme {

  namespace {

    void addRoot(std::vector<std::filesystem::path>& roots, std::filesystem::path root) {
      if (root.empty()) {
        return;
      }
      roots.push_back(std::move(root));
    }

    void splitDataDirs(std::vector<std::filesystem::path>& roots, std::string_view dirs) {
      std::size_t start = 0;
      while (start <= dirs.size()) {
        const std::size_t end = dirs.find(':', start);
        const std::string_view part = dirs.substr(start, end == std::string_view::npos ? dirs.size() - start : end - start);
        if (!part.empty()) {
          addRoot(roots, std::filesystem::path(part) / "themes");
        }
        if (end == std::string_view::npos) {
          break;
        }
        start = end + 1;
      }
    }

    [[nodiscard]] bool themeNamePresent(std::string_view name, const std::vector<std::filesystem::path>& themeRoots) {
      std::error_code ec;
      for (const auto& root : themeRoots) {
        if (std::filesystem::is_directory(root / name, ec)) {
          return true;
        }
      }
      return false;
    }

  } // namespace

  std::vector<std::filesystem::path> adwGtk3ThemeRoots() {
    std::vector<std::filesystem::path> roots;
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      addRoot(roots, std::filesystem::path(home) / ".themes");
    }
    if (const char* xdgData = std::getenv("XDG_DATA_HOME"); xdgData != nullptr && xdgData[0] != '\0') {
      addRoot(roots, std::filesystem::path(xdgData) / "themes");
    } else if (home != nullptr && home[0] != '\0') {
      addRoot(roots, std::filesystem::path(home) / ".local/share/themes");
    }
    addRoot(roots, "/usr/share/themes");
    addRoot(roots, "/usr/local/share/themes");
    if (const char* xdgDirs = std::getenv("XDG_DATA_DIRS"); xdgDirs != nullptr && xdgDirs[0] != '\0') {
      splitDataDirs(roots, xdgDirs);
    }
    return roots;
  }

  bool adwGtk3ThemePresent(const std::vector<std::filesystem::path>& themeRoots) {
    return themeNamePresent("adw-gtk3", themeRoots) || themeNamePresent("adw-gtk3-dark", themeRoots);
  }

  bool adwGtk3ThemePresent() { return adwGtk3ThemePresent(adwGtk3ThemeRoots()); }

  bool gtkTemplatesEnabled(const ThemeConfig::TemplatesConfig& templates) {
    if (!templates.enableBuiltinTemplates) {
      return false;
    }
    return std::ranges::any_of(templates.builtinIds, [](const std::string& id) {
      return id == kGtk3TemplateId || id == kGtk4TemplateId;
    });
  }

  bool appendGtkTemplateIds(std::vector<std::string>& ids) {
    bool changed = false;
    const auto ensure = [&](std::string_view id) {
      if (std::ranges::find(ids, id) == ids.end()) {
        ids.emplace_back(id);
        changed = true;
      }
    };
    ensure(kGtk3TemplateId);
    ensure(kGtk4TemplateId);
    return changed;
  }

} // namespace helix::theme
