#include "shell/wallpaper/video_wallpaper_service.h"

#include "config/config_service.h"
#include "config/widget_setting_value.h"
#include "core/log.h"
#include "core/process/process.h"
#include "shell/wallpaper/wallpaper.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace {

  constexpr Logger kLog("video-wallpaper");
  constexpr auto kHideDelay = std::chrono::milliseconds(500);

  [[nodiscard]] bool pidAlive(int pid) {
    if (pid <= 0) {
      return false;
    }
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path("/proc") / std::to_string(pid), ec) && !ec;
  }

  [[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
      return {};
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  bool writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return false;
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
      return false;
    }
    out << contents;
    return static_cast<bool>(out);
  }

} // namespace

VideoWallpaperService::VideoWallpaperService(ConfigService& config, Wallpaper& wallpaper, WaylandConnection& wayland)
    : m_config(config), m_wallpaper(wallpaper), m_wayland(wayland) {
  m_wallpaper.setVideoWallpaperService(this);
}

VideoWallpaperService::~VideoWallpaperService() {
  m_hideTimer.stop();
  std::vector<std::string> connectors;
  connectors.reserve(m_instances.size());
  for (const auto& [connector, _] : m_instances) {
    connectors.push_back(connector);
  }
  for (const auto& connector : connectors) {
    stopInstance(connector);
  }
  m_wallpaper.setVideoWallpaperService(nullptr);
}

void VideoWallpaperService::start() {
  m_started = true;
  m_lastVideoConfig = m_config.config().wallpaper.video;
  probeAutoMode();
  loadAssignments();
  applyAll();
}

void VideoWallpaperService::onOutputChange() {
  if (!m_started || !available()) {
    return;
  }
  const auto present = presentConnectors();
  if (const auto it = m_assignments.find(std::string(wallpaper::video::kWildcardConnector));
      it != m_assignments.end()) {
    startInstance(std::string(wallpaper::video::kWildcardConnector), it->second);
    return;
  }
  for (const auto& connector : present) {
    const auto it = m_assignments.find(connector);
    if (it != m_assignments.end()) {
      startInstance(connector, it->second);
    }
  }
}

void VideoWallpaperService::onConfigChanged() {
  if (!m_started) {
    return;
  }
  const auto next = m_config.config().wallpaper.video;
  if (next == m_lastVideoConfig) {
    return;
  }
  m_lastVideoConfig = next;
  applyAll();
}

bool VideoWallpaperService::available() const { return wallpaper::video::binaryAvailable(); }

std::filesystem::path VideoWallpaperService::directory() const {
  const std::string configured = m_config.config().wallpaper.video.directory;
  return wallpaper::video::resolveDirectory(configured);
}

bool VideoWallpaperService::hasAssignment(std::string_view connector) const {
  if (m_assignments.contains(std::string(wallpaper::video::kWildcardConnector))) {
    return true;
  }
  return m_assignments.contains(std::string(connector));
}

void VideoWallpaperService::set(std::string connector, std::string path) {
  if (connector.empty()) {
    connector = std::string(wallpaper::video::kWildcardConnector);
  }
  if (path.empty() || !available()) {
    return;
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    kLog.warn("video wallpaper path missing: {}", path);
    return;
  }

  if (connector == wallpaper::video::kWildcardConnector) {
    std::vector<std::string> specific;
    for (const auto& [name, _] : m_assignments) {
      if (name != wallpaper::video::kWildcardConnector) {
        specific.push_back(name);
      }
    }
    for (const auto& name : specific) {
      clear(name);
    }
  } else if (m_assignments.contains(std::string(wallpaper::video::kWildcardConnector))) {
    clear(std::string(wallpaper::video::kWildcardConnector));
  }

  m_assignments[connector] = path;
  persistAssignments();
  startInstance(connector, path);
  kLog.info("applied video wallpaper {} to {}", path, connector);
}

void VideoWallpaperService::clear(const std::optional<std::string>& connector) {
  if (!connector.has_value() || connector->empty() || *connector == wallpaper::video::kWildcardConnector) {
    clearAll();
    return;
  }
  stopInstance(*connector);
  m_assignments.erase(*connector);
  persistAssignments();
}

void VideoWallpaperService::clearAll() {
  std::vector<std::string> connectors;
  connectors.reserve(m_instances.size());
  for (const auto& [connector, _] : m_instances) {
    connectors.push_back(connector);
  }
  for (const auto& connector : connectors) {
    stopInstance(connector);
  }
  m_assignments.clear();
  persistAssignments();
}

wallpaper::video::LaunchSpec VideoWallpaperService::launchSpec(std::string connector, std::string path) const {
  const auto& video = m_config.config().wallpaper.video;
  return {
      .connector = std::move(connector),
      .path = std::move(path),
      .mute = video.mute,
      .hardwareDecode = video.hardwareDecode,
      .autoPause = video.autoPause,
      .extraMpvOptions = video.mpvOptions,
      .autoModeSupported = m_autoModeSupported,
  };
}

std::vector<std::string> VideoWallpaperService::presentConnectors() const {
  std::vector<std::string> out;
  for (const auto& output : m_wayland.outputs()) {
    if (!output.connectorName.empty()) {
      out.push_back(output.connectorName);
    }
  }
  return out;
}

std::filesystem::path VideoWallpaperService::assignmentsPath() const {
  const auto dir = FileUtils::stateDir();
  if (dir.empty()) {
    return {};
  }
  return std::filesystem::path(dir) / "video-wallpaper.json";
}

std::filesystem::path VideoWallpaperService::pluginAssignmentsPath() const {
  const auto dir = FileUtils::stateDir();
  if (dir.empty()) {
    return {};
  }
  return std::filesystem::path(dir) / "mpvpaper" / "assignments.json";
}

void VideoWallpaperService::loadAssignments() {
  const auto native = assignmentsPath();
  std::string json = native.empty() ? std::string{} : readFile(native);
  if (json.empty()) {
    json = readFile(pluginAssignmentsPath());
  }
  m_assignments = wallpaper::video::parseAssignmentsJson(json);
}

void VideoWallpaperService::persistAssignments() const {
  const auto path = assignmentsPath();
  if (path.empty()) {
    return;
  }
  if (!writeFile(path, wallpaper::video::serializeAssignmentsJson(m_assignments))) {
    kLog.warn("failed to persist video wallpaper assignments to {}", path.string());
  }
}

void VideoWallpaperService::applyAll() {
  if (!available()) {
    return;
  }
  for (const auto& [connector, path] : m_assignments) {
    startInstance(connector, path);
  }
}

void VideoWallpaperService::startInstance(const std::string& connector, const std::string& path) {
  stopInstance(connector);
  if (!available()) {
    return;
  }
  const auto args = wallpaper::video::buildCommand(launchSpec(connector, path));
  const auto pid = process::launchDetachedTracked(args);
  if (!pid.has_value()) {
    kLog.warn("failed to start mpvpaper on {}", connector);
    return;
  }
  m_instances[connector] = Instance{.pid = *pid, .path = path};
  m_pendingHide.insert(connector);
  m_hideTimer.start(kHideDelay, [this]() { hidePendingSurfaces(); });
}

void VideoWallpaperService::stopInstance(const std::string& connector) {
  m_pendingHide.erase(connector);
  const auto it = m_instances.find(connector);
  if (it == m_instances.end()) {
    markManagedOutputs(connector, false);
    return;
  }
  const int pid = it->second.pid;
  m_instances.erase(it);
  stopPid(pid);
  markManagedOutputs(connector, false);
}

void VideoWallpaperService::stopPid(int pid) {
  if (pid <= 0) {
    return;
  }
  if (::kill(-pid, SIGTERM) != 0) {
    ::kill(pid, SIGTERM);
  }
  if (pidAlive(pid)) {
    if (::kill(-pid, SIGKILL) != 0) {
      ::kill(pid, SIGKILL);
    }
  }
}

void VideoWallpaperService::markManagedOutputs(const std::string& connector, bool managed) {
  if (connector == wallpaper::video::kWildcardConnector) {
    for (const auto& name : presentConnectors()) {
      m_wallpaper.setOutputExternallyManaged(name, managed);
    }
    return;
  }
  m_wallpaper.setOutputExternallyManaged(connector, managed);
}

void VideoWallpaperService::hidePendingSurfaces() {
  const auto pending = m_pendingHide;
  m_pendingHide.clear();
  for (const auto& connector : pending) {
    if (m_instances.contains(connector)) {
      markManagedOutputs(connector, true);
    }
  }
}

void VideoWallpaperService::probeAutoMode() {
  const auto result = process::runSync({"mpvpaper", "--help"});
  m_autoModeSupported = wallpaper::video::helpSupportsAutoMode(result.out + result.err);
}
