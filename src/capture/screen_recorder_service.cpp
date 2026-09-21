#include "capture/screen_recorder_service.h"

#include "capture/screenshot_service.h"
#include "cli/schema_msg.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "notification/notification.h"
#include "notification/notification_manager.h"
#include "shell/panel/panel_manager.h"
#include "time/time_format.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <format>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace {

  constexpr Logger kLog("screen-record");
  constexpr const char* kDefaultFilenamePattern = "recording_%Y%m%d_%H%M%S";
  constexpr auto kPollInterval = std::chrono::milliseconds(500);
  constexpr auto kStopTick = std::chrono::milliseconds(200);
  constexpr int kStopMaxTicks = 25;
  constexpr auto kReplaySaveSettle = std::chrono::milliseconds(800);
  constexpr const char* kFlatpakGsrId = "com.dec05eba.gpu_screen_recorder";

  [[nodiscard]] std::string defaultFilenamePattern() { return kDefaultFilenamePattern; }

  [[nodiscard]] std::string formatFilenameStem(std::string_view pattern, int suffix) {
    const auto now = std::chrono::system_clock::now();
    const auto unixSeconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const std::string resolvedPattern = pattern.empty() ? defaultFilenamePattern() : std::string(pattern);
    std::string stem = formatLocalUnixTime(unixSeconds, resolvedPattern);
    if (stem.empty()) {
      stem = "recording";
    }
    if (suffix > 0) {
      stem += '-';
      stem += std::to_string(suffix);
    }
    return stem;
  }

  [[nodiscard]] bool fileHasContent(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
      return false;
    }
    return std::filesystem::file_size(path, ec) > 0 && !ec;
  }

  [[nodiscard]] std::string formatElapsed(std::chrono::seconds elapsed) {
    const auto total = elapsed.count();
    const auto hours = total / 3600;
    const auto minutes = (total % 3600) / 60;
    const auto seconds = total % 60;
    if (hours > 0) {
      return std::format("{:d}:{:02}:{:02}", hours, minutes, seconds);
    }
    return std::format("{:02}:{:02}", minutes, seconds);
  }

} // namespace

ScreenRecorderService::ScreenRecorderService(
    CompositorPlatform& platform, ConfigService& config, NotificationManager& notifications,
    ScreenshotService& screenshots
)
    : m_platform(platform), m_config(config), m_notifications(notifications), m_screenshots(screenshots) {}

ScreenRecorderService::~ScreenRecorderService() {
  m_pollTimer.stop();
  m_stopTimer.stop();
  m_saveTimer.stop();
  if (m_pid > 0) {
    sendSignal(SIGINT);
  }
}

bool ScreenRecorderService::hasGpuScreenRecorder() const {
  return process::commandExists("gpu-screen-recorder") || process::flatpakAppInstalled(kFlatpakGsrId);
}

bool ScreenRecorderService::hasWfRecorder() const { return process::commandExists("wf-recorder"); }

bool ScreenRecorderService::available() const { return hasGpuScreenRecorder() || hasWfRecorder(); }

bool ScreenRecorderService::supportsSource(Source source) const {
  switch (source) {
  case Source::Focused:
    return available();
  case Source::Portal:
    return hasGpuScreenRecorder();
  case Source::Region:
    return hasWfRecorder();
  }
  return false;
}

std::vector<ScreenRecorderService::Source> ScreenRecorderService::availableSources() const {
  std::vector<Source> sources{Source::Focused};
  if (hasWfRecorder()) {
    sources.push_back(Source::Region);
  }
  if (hasGpuScreenRecorder()) {
    sources.push_back(Source::Portal);
  }
  return sources;
}

bool ScreenRecorderService::isRecording() const noexcept {
  return m_state == State::Recording || m_state == State::Stopping;
}

bool ScreenRecorderService::isStopping() const noexcept { return m_state == State::Stopping; }

bool ScreenRecorderService::isReplaying() const noexcept {
  return m_state == State::Replaying || m_state == State::StoppingReplay;
}

bool ScreenRecorderService::isReplayStopping() const noexcept { return m_state == State::StoppingReplay; }

int ScreenRecorderService::replayDurationSeconds() const {
  const auto& range = noctalia::config::schema::kScreenRecordReplayDurationRange;
  const int minValue = static_cast<int>(range.min.value_or(2));
  const int maxValue = static_cast<int>(range.max.value_or(600));
  return std::clamp(static_cast<int>(m_config.config().shell.screenRecord.replayDuration), minValue, maxValue);
}

std::chrono::seconds ScreenRecorderService::elapsed() const {
  if (m_state == State::Idle) {
    return std::chrono::seconds{0};
  }
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_startedAt);
}

std::string ScreenRecorderService::elapsedLabel() const { return formatElapsed(elapsed()); }

std::filesystem::path ScreenRecorderService::outputPath() const { return m_outputPath; }

std::string ScreenRecorderService::backendDescription() const {
  if (process::commandExists("gpu-screen-recorder")) {
    return i18n::tr("control-center.screen-record.backend-gsr");
  }
  if (process::flatpakAppInstalled(kFlatpakGsrId)) {
    return i18n::tr("control-center.screen-record.backend-gsr-flatpak");
  }
  if (hasWfRecorder()) {
    return i18n::tr("control-center.screen-record.backend-wf");
  }
  return i18n::tr("control-center.screen-record.missing-backend");
}

void ScreenRecorderService::setChangeCallback(ChangeCallback callback) { m_onChange = std::move(callback); }

std::optional<ScreenRecorderService::Source> ScreenRecorderService::parseSource(std::string_view token) {
  const std::string value = StringUtils::toLower(StringUtils::trim(token));
  if (value.empty() || value == "focused") {
    return Source::Focused;
  }
  if (value == "portal") {
    return Source::Portal;
  }
  if (value == "region") {
    return Source::Region;
  }
  return std::nullopt;
}

std::optional<ScreenRecorderService::Source> ScreenRecorderService::parseReplaySource(std::string_view token) {
  const auto source = parseSource(token);
  if (!source.has_value() || *source == Source::Region) {
    return std::nullopt;
  }
  return source;
}

void ScreenRecorderService::notifyChanged() {
  if (m_onChange) {
    m_onChange();
  }
  PanelManager::instance().requestRedraw();
}

void ScreenRecorderService::notifySaved() {
  m_notifications.addInternal(
      "Helix", i18n::tr("control-center.screen-record.saved"), m_outputPath.string()
  );
}

void ScreenRecorderService::notifyReplaySaved() {
  m_notifications.addInternal(
      "Helix", i18n::tr("control-center.screen-record.replay-saved"), outputDirectory().string()
  );
}

void ScreenRecorderService::notifyFailed(const std::string& message) {
  m_notifications.addInternal(
      "Helix", i18n::tr("control-center.screen-record.failed"), message, Urgency::Critical
  );
}

std::filesystem::path ScreenRecorderService::outputDirectory() const {
  const auto& directory = m_config.config().shell.screenRecord.directory;
  if (directory.empty()) {
    return FileUtils::expandUserPath("~/Videos/Recordings");
  }
  return FileUtils::expandUserPath(directory);
}

std::filesystem::path ScreenRecorderService::makeOutputPath() const {
  const auto dir = outputDirectory();
  const auto& pattern = m_config.config().shell.screenRecord.filenamePattern;
  for (int suffix = 0; suffix < 100; ++suffix) {
    const auto path = dir / (formatFilenameStem(pattern, suffix) + ".mp4");
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
      return path;
    }
  }
  return dir / (formatFilenameStem(pattern, 100) + ".mp4");
}

std::string ScreenRecorderService::focusedOutputName() const {
  const auto* info = m_platform.findOutputByWl(m_platform.preferredInteractiveOutput());
  return info != nullptr ? info->connectorName : std::string{};
}

bool ScreenRecorderService::audioEnabled() const { return m_config.config().shell.screenRecord.audio; }

bool ScreenRecorderService::showCursor() const { return m_config.config().shell.screenRecord.showCursor; }

std::vector<std::string> ScreenRecorderService::gsrCommand() const {
  if (process::commandExists("gpu-screen-recorder")) {
    return {"gpu-screen-recorder"};
  }
  return {"flatpak", "run", "--command=gpu-screen-recorder", kFlatpakGsrId};
}

bool ScreenRecorderService::processAlive() const {
  if (m_pid <= 0) {
    return false;
  }
  if (::kill(m_pid, 0) == 0) {
    return true;
  }
  return errno == EPERM;
}

void ScreenRecorderService::sendSignal(int sig) {
  if (m_pid <= 0) {
    return;
  }
  if (::kill(m_pid, sig) != 0 && errno != ESRCH) {
    kLog.warn("failed to signal recorder pid {}: {}", m_pid, errno);
  }
}

bool ScreenRecorderService::launch(
    const std::vector<std::string>& args, const std::filesystem::path& output, State nextState
) {
  const auto pid = process::launchDetachedTracked(args);
  if (!pid.has_value()) {
    kLog.warn("failed to launch recorder");
    return false;
  }
  m_pid = *pid;
  m_outputPath = output;
  m_startedAt = std::chrono::steady_clock::now();
  m_state = nextState;
  startPolling();
  notifyChanged();
  kLog.info(
      "{} started pid={} output={}", nextState == State::Replaying ? "replay" : "recording", m_pid, output.string()
  );
  return true;
}

bool ScreenRecorderService::launchGsr(const std::string& window, const std::filesystem::path& output) {
  auto args = gsrCommand();
  args.insert(args.end(), {"-w", window, "-f", "60", "-k", "h264"});
  if (audioEnabled()) {
    args.insert(args.end(), {"-a", "default_output"});
  }
  args.insert(args.end(), {"-cursor", showCursor() ? "yes" : "no", "-v", "no", "-o", output.string()});
  return launch(args, output, State::Recording);
}

bool ScreenRecorderService::launchGsrReplay(const std::string& window, const std::filesystem::path& directory) {
  auto args = gsrCommand();
  args.insert(args.end(), {"-w", window, "-c", "mp4", "-f", "60", "-k", "h264"});
  if (audioEnabled()) {
    args.insert(args.end(), {"-a", "default_output"});
  }
  args.insert(
      args.end(),
      {"-cursor", showCursor() ? "yes" : "no", "-r", std::to_string(replayDurationSeconds()), "-v", "no", "-o",
       directory.string()}
  );
  return launch(args, directory, State::Replaying);
}

bool ScreenRecorderService::launchWf(
    const std::optional<std::string>& outputName, const std::optional<LogicalRect>& region,
    const std::filesystem::path& output
) {
  std::vector<std::string> args{"wf-recorder", "-f", output.string()};
  if (outputName.has_value() && !outputName->empty()) {
    args.insert(args.end(), {"-o", *outputName});
  }
  if (region.has_value()) {
    args.insert(
        args.end(),
        {"-g", std::format("{},{} {}x{}", region->x, region->y, region->width, region->height)}
    );
  }
  if (audioEnabled()) {
    args.emplace_back("-a");
  }
  return launch(args, output, State::Recording);
}

void ScreenRecorderService::startPolling() {
  m_pollTimer.stop();
  m_pollTimer.startRepeating(kPollInterval, [this]() { onProcessCheck(); });
}

void ScreenRecorderService::onProcessCheck() {
  if (m_state == State::Replaying) {
    if (processAlive()) {
      notifyChanged();
      return;
    }
    kLog.warn("replay process exited unexpectedly");
    finalizeReplay();
    return;
  }
  if (m_state != State::Recording) {
    return;
  }
  if (processAlive()) {
    notifyChanged();
    return;
  }
  kLog.warn("recorder process exited unexpectedly");
  finalize(true);
}

void ScreenRecorderService::requestStop() {
  if (m_state != State::Recording) {
    return;
  }
  m_state = State::Stopping;
  m_pollTimer.stop();
  sendSignal(SIGINT);
  m_stopWaitTicks = 0;
  m_stopTimer.stop();
  m_stopTimer.startRepeating(kStopTick, [this]() {
    ++m_stopWaitTicks;
    if (!processAlive()) {
      m_stopTimer.stop();
      finalize(true);
      return;
    }
    if (m_stopWaitTicks >= kStopMaxTicks) {
      kLog.warn("recorder did not exit after SIGINT, sending SIGKILL");
      sendSignal(SIGKILL);
      m_stopTimer.stop();
      finalize(true);
    }
  });
  notifyChanged();
}

void ScreenRecorderService::requestStopReplay() {
  if (m_state != State::Replaying) {
    return;
  }
  m_state = State::StoppingReplay;
  m_pollTimer.stop();
  m_saveTimer.stop();
  sendSignal(SIGINT);
  m_stopWaitTicks = 0;
  m_stopTimer.stop();
  m_stopTimer.startRepeating(kStopTick, [this]() {
    ++m_stopWaitTicks;
    if (!processAlive()) {
      m_stopTimer.stop();
      finalizeReplay();
      return;
    }
    if (m_stopWaitTicks >= kStopMaxTicks) {
      kLog.warn("replay process did not exit after SIGINT, sending SIGKILL");
      sendSignal(SIGKILL);
      m_stopTimer.stop();
      finalizeReplay();
    }
  });
  notifyChanged();
}

void ScreenRecorderService::finalize(bool expectFile) {
  m_pollTimer.stop();
  m_stopTimer.stop();
  m_saveTimer.stop();
  m_pid = -1;
  m_state = State::Idle;
  const auto path = m_outputPath;
  if (expectFile && fileHasContent(path)) {
    notifySaved();
  } else if (expectFile) {
    notifyFailed(i18n::tr("control-center.screen-record.stopped-early"));
  }
  notifyChanged();
}

void ScreenRecorderService::finalizeReplay() {
  m_pollTimer.stop();
  m_stopTimer.stop();
  m_saveTimer.stop();
  m_pid = -1;
  m_state = State::Idle;
  notifyChanged();
}

std::string ScreenRecorderService::start(Source source) {
  if (isReplaying()) {
    return "error: a replay buffer is already running\n";
  }
  if (m_state != State::Idle) {
    return "error: a recording is already in progress\n";
  }
  if (source == Source::Portal && !hasGpuScreenRecorder()) {
    return "error: portal recording requires gpu-screen-recorder\n";
  }
  if (source == Source::Region && !hasWfRecorder()) {
    return "error: region recording requires wf-recorder\n";
  }
  if (source != Source::Portal && source != Source::Region && !available()) {
    return "error: gpu-screen-recorder or wf-recorder is not installed\n";
  }
  if (source == Source::Portal && !process::desktopPortalAvailable()) {
    return "error: xdg-desktop-portal is not running\n";
  }
  if (source == Source::Region && m_screenshots.overlayBusy()) {
    return "error: a screenshot overlay is already active\n";
  }

  std::error_code ec;
  const auto dir = outputDirectory();
  if (!std::filesystem::create_directories(dir, ec) && ec) {
    return "error: could not create the recordings folder\n";
  }

  PanelManager::instance().closePanel();

  if (source == Source::Region) {
    auto* renderContext = PanelManager::instance().renderContext();
    if (renderContext == nullptr) {
      return "error: render context unavailable\n";
    }
    m_screenshots.beginRegionPick(*renderContext, [this](std::optional<LogicalRect> region) {
      if (!region.has_value() || m_state != State::Idle) {
        return;
      }
      if (region->width < 2 || region->height < 2) {
        notifyFailed(i18n::tr("control-center.screen-record.region-too-small"));
        return;
      }
      const auto path = makeOutputPath();
      if (!launchWf(std::nullopt, region, path)) {
        notifyFailed(i18n::tr("control-center.screen-record.launch-failed"));
      }
    });
    return "ok\n";
  }

  const auto path = makeOutputPath();
  if (source == Source::Portal) {
    if (!launchGsr("portal", path)) {
      return "error: failed to start gpu-screen-recorder\n";
    }
    return "ok\n";
  }

  const std::string monitor = focusedOutputName();
  if (monitor.empty()) {
    return "error: no focused monitor to record\n";
  }
  if (hasGpuScreenRecorder()) {
    if (!launchGsr(monitor, path)) {
      return "error: failed to start gpu-screen-recorder\n";
    }
    return "ok\n";
  }
  if (!launchWf(monitor, std::nullopt, path)) {
    return "error: failed to start wf-recorder\n";
  }
  return "ok\n";
}

std::string ScreenRecorderService::stop() {
  if (m_state == State::Idle || m_state == State::Stopping || m_state == State::StoppingReplay) {
    return "ok\n";
  }
  if (m_state == State::Replaying) {
    return "error: a replay buffer is running; use screen-record-replay-stop\n";
  }
  requestStop();
  return "ok\n";
}

std::string ScreenRecorderService::toggle(Source source) {
  if (isReplaying()) {
    return "error: a replay buffer is already running\n";
  }
  if (m_state != State::Idle) {
    return stop();
  }
  return start(source);
}

std::string ScreenRecorderService::startReplay(Source source) {
  if (isRecording()) {
    return "error: a recording is already in progress\n";
  }
  if (m_state != State::Idle) {
    return "error: a replay buffer is already running\n";
  }
  if (!hasGpuScreenRecorder()) {
    return "error: replay buffer requires gpu-screen-recorder\n";
  }
  if (source == Source::Region) {
    return "error: replay source must be focused or portal\n";
  }
  if (source == Source::Portal && !process::desktopPortalAvailable()) {
    return "error: xdg-desktop-portal is not running\n";
  }

  std::error_code ec;
  const auto dir = outputDirectory();
  if (!std::filesystem::create_directories(dir, ec) && ec) {
    return "error: could not create the recordings folder\n";
  }

  PanelManager::instance().closePanel();

  if (source == Source::Portal) {
    if (!launchGsrReplay("portal", dir)) {
      return "error: failed to start gpu-screen-recorder replay\n";
    }
    return "ok\n";
  }

  const std::string monitor = focusedOutputName();
  if (monitor.empty()) {
    return "error: no focused monitor to record\n";
  }
  if (!launchGsrReplay(monitor, dir)) {
    return "error: failed to start gpu-screen-recorder replay\n";
  }
  return "ok\n";
}

std::string ScreenRecorderService::stopReplay() {
  if (m_state == State::Idle || m_state == State::StoppingReplay) {
    return "ok\n";
  }
  if (m_state != State::Replaying) {
    return "error: replay buffer is not running\n";
  }
  requestStopReplay();
  return "ok\n";
}

std::string ScreenRecorderService::saveReplay() {
  if (m_state != State::Replaying) {
    return "error: replay buffer is not running\n";
  }
  sendSignal(SIGUSR1);
  m_saveTimer.stop();
  m_saveTimer.start(kReplaySaveSettle, [this]() {
    notifyReplaySaved();
    notifyChanged();
  });
  return "ok\n";
}

std::string ScreenRecorderService::toggleReplay(Source source) {
  if (isRecording()) {
    return "error: a recording is already in progress\n";
  }
  if (m_state != State::Idle) {
    return stopReplay();
  }
  return startReplay(source);
}

void ScreenRecorderService::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::screenRecordStart, [this](const std::string& args) -> std::string {
    const auto source = parseSource(args);
    if (!source.has_value()) {
      return "error: source must be focused, portal, or region\n";
    }
    return start(*source);
  });
  ipc.bind(noctalia::cli::msg::screenRecordStop, [this](const std::string& /*args*/) -> std::string { return stop(); });
  ipc.bind(noctalia::cli::msg::screenRecordToggle, [this](const std::string& args) -> std::string {
    const auto source = parseSource(args);
    if (!source.has_value()) {
      return "error: source must be focused, portal, or region\n";
    }
    return toggle(*source);
  });
  ipc.bind(noctalia::cli::msg::screenRecordReplayStart, [this](const std::string& args) -> std::string {
    const auto source = parseReplaySource(args);
    if (!source.has_value()) {
      return "error: source must be focused or portal\n";
    }
    return startReplay(*source);
  });
  ipc.bind(noctalia::cli::msg::screenRecordReplayStop, [this](const std::string& /*args*/) -> std::string {
    return stopReplay();
  });
  ipc.bind(noctalia::cli::msg::screenRecordReplaySave, [this](const std::string& /*args*/) -> std::string {
    return saveReplay();
  });
  ipc.bind(noctalia::cli::msg::screenRecordReplayToggle, [this](const std::string& args) -> std::string {
    const auto source = parseReplaySource(args);
    if (!source.has_value()) {
      return "error: source must be focused or portal\n";
    }
    return toggleReplay(*source);
  });
}
