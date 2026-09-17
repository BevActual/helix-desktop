#include "system/display_config_service.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "compositors/niri/niri_event_handler.h"
#include "compositors/niri/niri_runtime.h"
#include "core/log.h"

#include <algorithm>
#include <format>
#include <memory>
#include <nlohmann/json.hpp>
#include <ranges>
#include <utility>

namespace {

  constexpr Logger kLog{"display_config"};
  constexpr std::string_view kOutputsRequest = "\"Outputs\"\n";

  [[nodiscard]] std::string outputFingerprint(const display_config::Output& output) {
    return std::format(
        "{};{};{};{}x{}@{};{:.4f};{};{},{};{};{};", output.name, output.enabled ? 1 : 0, output.vrrEnabled ? 1 : 0,
        output.current.width, output.current.height, output.current.refreshMilliHz, output.scale,
        static_cast<int>(output.transform), output.x, output.y, output.logicalWidth, output.logicalHeight
    );
  }

} // namespace

class DisplayConfigService::EventBridge : public compositors::niri::NiriEventHandler {
public:
  EventBridge(compositors::niri::NiriRuntime& runtime, DisplayConfigService& service)
      : compositors::niri::NiriEventHandler(runtime), m_service(service) {}

  void handleEvent(std::string_view key, const nlohmann::json& /*value*/) override {
    if (key == "ConfigLoaded") {
      m_service.onNiriConfigLoaded();
    }
  }

  void handleStreamReset() override { m_service.refresh(); }

private:
  DisplayConfigService& m_service;
};

DisplayConfigService::DisplayConfigService(CompositorPlatform& platform) : m_platform(platform) {
  if (compositors::isNiri()) {
    m_eventBridge = std::make_unique<EventBridge>(m_platform.niriRuntime(), *this);
  }
  if (available()) {
    refresh();
  }
}

DisplayConfigService::~DisplayConfigService() = default;

void DisplayConfigService::onNiriConfigLoaded() {
  refresh();
  if (m_configLoadedCallback) {
    m_configLoadedCallback();
  }
}

bool DisplayConfigService::available() const noexcept {
  return compositors::isNiri() && m_platform.niriRuntime().available();
}

const display_config::Output* DisplayConfigService::findOutput(std::string_view name) const {
  const auto it = std::ranges::find_if(m_outputs, [&](const display_config::Output& output) {
    return output.name == name;
  });
  if (it == m_outputs.end()) {
    return nullptr;
  }
  return &*it;
}

std::size_t DisplayConfigService::enabledCount() const noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(m_outputs, [](const display_config::Output& output) {
    return output.enabled;
  }));
}

std::string DisplayConfigService::fingerprint() const {
  std::string key;
  for (const auto& output : m_outputs) {
    key += outputFingerprint(output);
  }
  return key;
}

void DisplayConfigService::refresh() {
  if (!available()) {
    if (!m_outputs.empty()) {
      m_outputs.clear();
      if (m_changeCallback) {
        m_changeCallback();
      }
    }
    return;
  }

  const auto reply = m_platform.niriRuntime().requestJson(kOutputsRequest);
  if (!reply.has_value()) {
    m_lastError = "niri outputs query failed";
    kLog.warn("{}", m_lastError);
    return;
  }
  if (const auto error = display_config::parseError(*reply); !error.empty()) {
    m_lastError = error;
    kLog.warn("niri outputs query error: {}", m_lastError);
    return;
  }

  auto next = display_config::parseOutputsReply(*reply);
  if (next == m_outputs) {
    return;
  }
  m_outputs = std::move(next);
  m_lastError.clear();
  if (m_changeCallback) {
    m_changeCallback();
  }
}

bool DisplayConfigService::setMode(std::string_view name, int width, int height, std::uint32_t refreshMilliHz) {
  const double refreshHz = static_cast<double>(refreshMilliHz) / 1000.0;
  return apply(name, display_config::modeAction(width, height, refreshHz));
}

bool DisplayConfigService::setScale(std::string_view name, double scale) {
  return apply(name, display_config::scaleAction(scale));
}

bool DisplayConfigService::setTransform(std::string_view name, display_config::Transform transform) {
  return apply(name, display_config::transformAction(transform));
}

bool DisplayConfigService::setPosition(std::string_view name, int x, int y) {
  std::vector<std::pair<std::string, display_config::Point>> others;
  const display_config::Output* moving = nullptr;
  for (const auto& output : m_outputs) {
    if (!output.enabled) {
      continue;
    }
    if (output.name == name) {
      moving = &output;
    } else {
      others.push_back({output.name, {.x = output.x, .y = output.y}});
    }
  }
  if (moving == nullptr) {
    m_lastError = "display not found";
    return false;
  }

  display_config::Output candidate = *moving;
  candidate.x = x;
  candidate.y = y;
  for (const auto& output : m_outputs) {
    if (!output.enabled || output.name == name) {
      continue;
    }
    if (display_config::outputsOverlap(candidate, output)) {
      m_lastError = "layout would overlap another display";
      kLog.warn("{}: {} at {},{}", m_lastError, name, x, y);
      return false;
    }
  }

  std::ranges::sort(others, [](const auto& a, const auto& b) { return a.first < b.first; });
  bool ok = true;
  for (const auto& [outputName, point] : others) {
    if (!apply(outputName, display_config::positionAction(point.x, point.y), false)) {
      ok = false;
    }
  }
  if (!apply(name, display_config::positionAction(x, y), false)) {
    ok = false;
  }
  refresh();
  return ok;
}

bool DisplayConfigService::setOrigin(std::string_view name) {
  const auto* target = findOutput(name);
  if (target == nullptr || !target->enabled) {
    m_lastError = "display not found";
    return false;
  }
  const int dx = target->x;
  const int dy = target->y;
  if (dx == 0 && dy == 0) {
    return true;
  }

  std::vector<std::pair<std::string, display_config::Point>> next;
  for (const auto& output : m_outputs) {
    if (!output.enabled) {
      continue;
    }
    next.push_back({output.name, {.x = output.x - dx, .y = output.y - dy}});
  }
  std::ranges::sort(next, [](const auto& a, const auto& b) { return a.first < b.first; });
  bool ok = true;
  for (const auto& [outputName, point] : next) {
    if (!apply(outputName, display_config::positionAction(point.x, point.y), false)) {
      ok = false;
    }
  }
  refresh();
  return ok;
}

bool DisplayConfigService::setVrr(std::string_view name, bool enabled) {
  return apply(name, display_config::vrrAction(enabled));
}

bool DisplayConfigService::setEnabled(std::string_view name, bool enabled) {
  if (!enabled && enabledCount() <= 1) {
    m_lastError = "cannot disable the last display";
    return false;
  }
  return apply(name, display_config::powerAction(enabled));
}

bool DisplayConfigService::placeRelative(
    std::string_view name, std::string_view referenceName, display_config::LayoutDirection direction, int offset
) {
  const auto* moving = findOutput(name);
  const auto* reference = findOutput(referenceName);
  if (moving == nullptr || reference == nullptr) {
    m_lastError = "display not found";
    return false;
  }
  int proposedX = moving->x;
  int proposedY = moving->y;
  if (direction == display_config::LayoutDirection::Left || direction == display_config::LayoutDirection::Right) {
    proposedY += offset;
  } else {
    proposedX += offset;
  }
  const auto point = display_config::placeAgainst(*moving, *reference, direction, proposedX, proposedY);
  return setPosition(name, point.x, point.y);
}

void DisplayConfigService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void DisplayConfigService::setConfigLoadedCallback(ChangeCallback callback) {
  m_configLoadedCallback = std::move(callback);
}

bool DisplayConfigService::apply(std::string_view name, const nlohmann::json& action, bool refreshAfter) {
  if (!available()) {
    m_lastError = "niri display configuration is unavailable";
    return false;
  }
  const auto payload = display_config::outputRequestPayload(name, action);
  const auto reply = m_platform.niriRuntime().requestJson(payload);
  if (!reply.has_value()) {
    m_lastError = "niri display request failed";
    kLog.warn("{} ({})", m_lastError, payload);
    return false;
  }
  if (const auto error = display_config::parseError(*reply); !error.empty()) {
    m_lastError = error;
    kLog.warn("niri display request error: {}", m_lastError);
    return false;
  }
  if (!reply->contains("Ok")) {
    m_lastError = "niri display request was rejected";
    kLog.warn("{}: {}", m_lastError, reply->dump());
    return false;
  }
  m_lastError.clear();
  kLog.info("applied niri output change for {}", name);
  if (refreshAfter) {
    refresh();
  }
  return true;
}
