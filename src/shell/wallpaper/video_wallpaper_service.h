#pragma once

#include "core/timer_manager.h"
#include "shell/wallpaper/video_wallpaper.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ConfigService;
class Wallpaper;
class WaylandConnection;

class VideoWallpaperService {
public:
  VideoWallpaperService(ConfigService& config, Wallpaper& wallpaper, WaylandConnection& wayland);
  ~VideoWallpaperService();

  VideoWallpaperService(const VideoWallpaperService&) = delete;
  VideoWallpaperService& operator=(const VideoWallpaperService&) = delete;

  void start();
  void onOutputChange();
  void onConfigChanged();

  [[nodiscard]] bool available() const;
  [[nodiscard]] std::filesystem::path directory() const;
  [[nodiscard]] bool hasAssignment(std::string_view connector) const;

  void set(std::string connector, std::string path);
  void clear(const std::optional<std::string>& connector);
  void clearAll();

private:
  struct Instance {
    int pid = -1;
    std::string path;
  };

  [[nodiscard]] wallpaper::video::LaunchSpec launchSpec(std::string connector, std::string path) const;
  [[nodiscard]] std::vector<std::string> presentConnectors() const;
  [[nodiscard]] std::filesystem::path assignmentsPath() const;
  [[nodiscard]] std::filesystem::path pluginAssignmentsPath() const;
  void loadAssignments();
  void persistAssignments() const;
  void applyAll();
  void startInstance(const std::string& connector, const std::string& path);
  void stopInstance(const std::string& connector);
  void stopPid(int pid);
  void setManaged(const std::string& connector, bool managed);
  void markManagedOutputs(const std::string& connector, bool managed);
  void hidePendingSurfaces();
  void probeAutoMode();

  ConfigService& m_config;
  Wallpaper& m_wallpaper;
  WaylandConnection& m_wayland;
  std::unordered_map<std::string, std::string> m_assignments;
  std::unordered_map<std::string, Instance> m_instances;
  std::unordered_set<std::string> m_pendingHide;
  WallpaperVideoConfig m_lastVideoConfig{};
  Timer m_hideTimer;
  bool m_autoModeSupported = false;
  bool m_started = false;
};
