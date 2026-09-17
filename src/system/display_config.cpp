#include "system/display_config.h"

#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
#include <utility>

namespace display_config {

  namespace {

    [[nodiscard]] std::optional<int> asInt(const nlohmann::json& value) {
      if (value.is_number_integer()) {
        return value.get<int>();
      }
      if (value.is_number_unsigned()) {
        const auto raw = value.get<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
          return std::nullopt;
        }
        return static_cast<int>(raw);
      }
      if (value.is_number_float()) {
        return static_cast<int>(std::lround(value.get<double>()));
      }
      return std::nullopt;
    }

    [[nodiscard]] std::optional<std::uint32_t> asUint32(const nlohmann::json& value) {
      if (value.is_number_unsigned()) {
        const auto raw = value.get<std::uint64_t>();
        if (raw > std::numeric_limits<std::uint32_t>::max()) {
          return std::nullopt;
        }
        return static_cast<std::uint32_t>(raw);
      }
      if (value.is_number_integer()) {
        const auto raw = value.get<std::int64_t>();
        if (raw < 0) {
          return std::nullopt;
        }
        return static_cast<std::uint32_t>(raw);
      }
      return std::nullopt;
    }

    [[nodiscard]] std::optional<double> asDouble(const nlohmann::json& value) {
      if (value.is_number()) {
        return value.get<double>();
      }
      return std::nullopt;
    }

    [[nodiscard]] std::string asString(const nlohmann::json& value) {
      if (value.is_string()) {
        return value.get<std::string>();
      }
      return {};
    }

    [[nodiscard]] std::optional<Mode> parseMode(const nlohmann::json& value) {
      if (!value.is_object()) {
        return std::nullopt;
      }
      const auto width = asInt(value.value("width", nlohmann::json{}));
      const auto height = asInt(value.value("height", nlohmann::json{}));
      const auto refresh = asUint32(value.value("refresh_rate", nlohmann::json{}));
      if (!width.has_value() || !height.has_value() || !refresh.has_value() || *width <= 0 || *height <= 0) {
        return std::nullopt;
      }
      Mode mode;
      mode.width = *width;
      mode.height = *height;
      mode.refreshMilliHz = *refresh;
      mode.preferred = value.value("is_preferred", false);
      return mode;
    }

    [[nodiscard]] const nlohmann::json* outputsObject(const nlohmann::json& reply) {
      if (reply.is_object()) {
        if (const auto okIt = reply.find("Ok"); okIt != reply.end() && okIt->is_object()) {
          if (const auto outputsIt = okIt->find("Outputs"); outputsIt != okIt->end() && outputsIt->is_object()) {
            return &(*outputsIt);
          }
        }
        if (const auto outputsIt = reply.find("Outputs"); outputsIt != reply.end() && outputsIt->is_object()) {
          return &(*outputsIt);
        }
      }
      return nullptr;
    }

  } // namespace

  bool isSideways(Transform transform) noexcept {
    switch (transform) {
    case Transform::Rotate90:
    case Transform::Rotate270:
    case Transform::Flipped90:
    case Transform::Flipped270:
      return true;
    default:
      return false;
    }
  }

  std::string_view jsonName(Transform transform) noexcept {
    switch (transform) {
    case Transform::Normal:
      return "Normal";
    case Transform::Rotate90:
      return "90";
    case Transform::Rotate180:
      return "180";
    case Transform::Rotate270:
      return "270";
    case Transform::Flipped:
      return "Flipped";
    case Transform::Flipped90:
      return "Flipped90";
    case Transform::Flipped180:
      return "Flipped180";
    case Transform::Flipped270:
      return "Flipped270";
    }
    return "Normal";
  }

  std::string_view kdlName(Transform transform) noexcept {
    switch (transform) {
    case Transform::Normal:
      return "normal";
    case Transform::Rotate90:
      return "90";
    case Transform::Rotate180:
      return "180";
    case Transform::Rotate270:
      return "270";
    case Transform::Flipped:
      return "flipped";
    case Transform::Flipped90:
      return "flipped-90";
    case Transform::Flipped180:
      return "flipped-180";
    case Transform::Flipped270:
      return "flipped-270";
    }
    return "normal";
  }

  std::optional<Transform> parseTransform(std::string_view value) {
    const auto normalized = StringUtils::trim(value);
    if (normalized == "Normal" || normalized == "normal") {
      return Transform::Normal;
    }
    if (normalized == "90") {
      return Transform::Rotate90;
    }
    if (normalized == "180") {
      return Transform::Rotate180;
    }
    if (normalized == "270") {
      return Transform::Rotate270;
    }
    if (normalized == "Flipped" || normalized == "flipped") {
      return Transform::Flipped;
    }
    if (normalized == "Flipped90" || normalized == "flipped-90" || normalized == "flipped90") {
      return Transform::Flipped90;
    }
    if (normalized == "Flipped180" || normalized == "flipped-180" || normalized == "flipped180") {
      return Transform::Flipped180;
    }
    if (normalized == "Flipped270" || normalized == "flipped-270" || normalized == "flipped270") {
      return Transform::Flipped270;
    }
    return std::nullopt;
  }

  std::string formatRefresh(std::uint32_t refreshMilliHz) {
    const double hz = static_cast<double>(refreshMilliHz) / 1000.0;
    if (std::abs(hz - std::round(hz)) < 0.0005) {
      return std::format("{:.0f} Hz", std::round(hz));
    }
    return std::format("{:.3f} Hz", hz);
  }

  std::string formatResolution(int width, int height) { return std::format("{}x{}", width, height); }

  std::string formatScale(double scale) {
    const double rounded = std::round(scale * 100.0) / 100.0;
    if (std::abs(rounded - std::round(rounded)) < 0.0005) {
      return std::format("{:.0f}×", std::round(rounded));
    }
    return std::format("{:.2f}×", rounded);
  }

  std::string displayLabel(const Output& output) {
    if (!output.model.empty() && output.model != "Unknown") {
      if (!output.make.empty() && output.make != "Unknown") {
        return output.make + " " + output.model;
      }
      return output.model;
    }
    if (!output.make.empty() && output.make != "Unknown") {
      return output.make;
    }
    return output.name;
  }

  Size logicalSize(const Output& output) {
    if (output.enabled && output.logicalWidth > 0 && output.logicalHeight > 0) {
      return {.width = output.logicalWidth, .height = output.logicalHeight};
    }
    const double scale = output.scale > 0.0 ? output.scale : 1.0;
    int width = static_cast<int>(std::lround(static_cast<double>(output.current.width) / scale));
    int height = static_cast<int>(std::lround(static_cast<double>(output.current.height) / scale));
    if (isSideways(output.transform)) {
      std::swap(width, height);
    }
    return {.width = std::max(0, width), .height = std::max(0, height)};
  }

  Point snapPosition(const Output& moving, const Output& reference, LayoutDirection direction, int offset) {
    const Size size = logicalSize(moving);
    const Size refSize = logicalSize(reference);
    int x = reference.x;
    int y = reference.y;
    switch (direction) {
    case LayoutDirection::Right:
      x = reference.x + refSize.width;
      y = reference.y + ((refSize.height - size.height) / 2) + offset;
      break;
    case LayoutDirection::Left:
      x = reference.x - size.width;
      y = reference.y + ((refSize.height - size.height) / 2) + offset;
      break;
    case LayoutDirection::Top:
      x = reference.x + ((refSize.width - size.width) / 2) + offset;
      y = reference.y - size.height;
      break;
    case LayoutDirection::Bottom:
      x = reference.x + ((refSize.width - size.width) / 2) + offset;
      y = reference.y + refSize.height;
      break;
    }
    return {.x = x, .y = y};
  }

  namespace {

    constexpr int kAlignMagnet = 24;
    constexpr int kMinEdgeOverlap = 32;
    constexpr long long kStaySideBias = 480LL * 480LL;

    [[nodiscard]] bool rangesOverlap(int aStart, int aExtent, int bStart, int bExtent) noexcept {
      return aStart < bStart + bExtent && bStart < aStart + aExtent;
    }

    [[nodiscard]] int rangeOverlapLength(int aStart, int aExtent, int bStart, int bExtent) noexcept {
      return std::max(0, std::min(aStart + aExtent, bStart + bExtent) - std::max(aStart, bStart));
    }

    [[nodiscard]] int minSharedEdge(int movingExtent, int refExtent) noexcept {
      return std::max(kMinEdgeOverlap, std::min(movingExtent, refExtent) / 4);
    }

    [[nodiscard]] int clampEdgeOverlap(int proposed, int refStart, int refExtent, int movingExtent) {
      const int minPos = refStart - movingExtent + 1;
      const int maxPos = refStart + refExtent - 1;
      if (minPos > maxPos) {
        return refStart + ((refExtent - movingExtent) / 2);
      }
      return std::clamp(proposed, minPos, maxPos);
    }

    [[nodiscard]] int magnetAlign(int value, int beginning, int center, int end) {
      int best = value;
      int bestDist = kAlignMagnet + 1;
      for (const int target : {beginning, center, end}) {
        const int dist = std::abs(value - target);
        if (dist < bestDist) {
          best = target;
          bestDist = dist;
        }
      }
      return bestDist <= kAlignMagnet ? best : value;
    }

    [[nodiscard]] int nearestAlign(int value, int beginning, int center, int end) noexcept {
      int best = beginning;
      int bestDist = std::abs(value - beginning);
      for (const int target : {center, end}) {
        const int dist = std::abs(value - target);
        if (dist < bestDist) {
          best = target;
          bestDist = dist;
        }
      }
      return best;
    }

    [[nodiscard]] int sharedEdgeLength(
        LayoutDirection direction, Point pos, Size movingSize, const Output& reference, Size refSize
    ) noexcept {
      if (direction == LayoutDirection::Left || direction == LayoutDirection::Right) {
        return rangeOverlapLength(pos.y, movingSize.height, reference.y, refSize.height);
      }
      return rangeOverlapLength(pos.x, movingSize.width, reference.x, refSize.width);
    }

    [[nodiscard]] Point alignedOnEdge(
        const Output& moving, const Output& reference, LayoutDirection direction, int proposedX, int proposedY,
        bool magnetOnly
    ) {
      const Size size = logicalSize(moving);
      const Size refSize = logicalSize(reference);
      auto along = [&](int proposed, int refStart, int refExtent, int movingExtent) {
        const int beginning = refStart;
        const int center = refStart + ((refExtent - movingExtent) / 2);
        const int end = refStart + refExtent - movingExtent;
        const int clamped = clampEdgeOverlap(proposed, refStart, refExtent, movingExtent);
        const int overlap = rangeOverlapLength(clamped, movingExtent, refStart, refExtent);
        if (overlap >= minSharedEdge(movingExtent, refExtent)) {
          return magnetAlign(clamped, beginning, center, end);
        }
        if (magnetOnly) {
          return clamped;
        }
        return nearestAlign(proposed, beginning, center, end);
      };
      switch (direction) {
      case LayoutDirection::Right:
        return {
            .x = reference.x + refSize.width,
            .y = along(proposedY, reference.y, refSize.height, size.height),
        };
      case LayoutDirection::Left:
        return {
            .x = reference.x - size.width,
            .y = along(proposedY, reference.y, refSize.height, size.height),
        };
      case LayoutDirection::Top:
        return {
            .x = along(proposedX, reference.x, refSize.width, size.width),
            .y = reference.y - size.height,
        };
      case LayoutDirection::Bottom:
        return {
            .x = along(proposedX, reference.x, refSize.width, size.width),
            .y = reference.y + refSize.height,
        };
      }
      return {.x = proposedX, .y = proposedY};
    }

  } // namespace

  bool outputsOverlap(const Output& a, const Output& b) noexcept {
    const Size aSize = logicalSize(a);
    const Size bSize = logicalSize(b);
    return rangesOverlap(a.x, aSize.width, b.x, bSize.width)
        && rangesOverlap(a.y, aSize.height, b.y, bSize.height);
  }

  bool isAdjacentOn(const Output& moving, const Output& reference, LayoutDirection direction) noexcept {
    const Size size = logicalSize(moving);
    const Size refSize = logicalSize(reference);
    switch (direction) {
    case LayoutDirection::Right:
      return moving.x == reference.x + refSize.width
          && rangesOverlap(moving.y, size.height, reference.y, refSize.height);
    case LayoutDirection::Left:
      return moving.x + size.width == reference.x
          && rangesOverlap(moving.y, size.height, reference.y, refSize.height);
    case LayoutDirection::Top:
      return moving.y + size.height == reference.y
          && rangesOverlap(moving.x, size.width, reference.x, refSize.width);
    case LayoutDirection::Bottom:
      return moving.y == reference.y + refSize.height
          && rangesOverlap(moving.x, size.width, reference.x, refSize.width);
    }
    return false;
  }

  std::optional<LayoutDirection> adjacency(const Output& moving, const Output& reference) noexcept {
    constexpr LayoutDirection kDirections[] = {
        LayoutDirection::Left, LayoutDirection::Right, LayoutDirection::Top, LayoutDirection::Bottom,
    };
    for (const auto direction : kDirections) {
      if (isAdjacentOn(moving, reference, direction)) {
        return direction;
      }
    }
    return std::nullopt;
  }

  Point placeAdjacent(
      const Output& moving, const Output& reference, LayoutDirection direction, int proposedX, int proposedY
  ) {
    return alignedOnEdge(moving, reference, direction, proposedX, proposedY, true);
  }

  Point placeAgainst(
      const Output& moving, const Output& reference, LayoutDirection direction, int proposedX, int proposedY
  ) {
    return alignedOnEdge(moving, reference, direction, proposedX, proposedY, false);
  }

  std::optional<SnapResult>
  snapDragPosition(const Output& moving, const std::vector<Output>& outputs, int proposedX, int proposedY) {
    std::optional<SnapResult> best;
    long long bestDist = std::numeric_limits<long long>::max();
    constexpr LayoutDirection kDirections[] = {
        LayoutDirection::Left, LayoutDirection::Right, LayoutDirection::Top, LayoutDirection::Bottom,
    };
    const Size movingSize = logicalSize(moving);
    for (const auto& output : outputs) {
      if (!output.enabled || output.name == moving.name) {
        continue;
      }
      const Size refSize = logicalSize(output);
      for (const auto direction : kDirections) {
        const bool staying = isAdjacentOn(moving, output, direction);
        const int along =
            (direction == LayoutDirection::Left || direction == LayoutDirection::Right)
                ? std::min(movingSize.height, refSize.height)
                : std::min(movingSize.width, refSize.width);
        const int minShare = minSharedEdge(along, along);
        Point snapped = placeAdjacent(moving, output, direction, proposedX, proposedY);
        if (sharedEdgeLength(direction, snapped, movingSize, output, refSize) < minShare) {
          if (staying) {
            continue;
          }
          snapped = placeAgainst(moving, output, direction, proposedX, proposedY);
          if (sharedEdgeLength(direction, snapped, movingSize, output, refSize) < minShare) {
            continue;
          }
        }
        Output candidate = moving;
        candidate.x = snapped.x;
        candidate.y = snapped.y;
        bool overlapsOther = false;
        for (const auto& other : outputs) {
          if (!other.enabled || other.name == moving.name || other.name == output.name) {
            continue;
          }
          if (outputsOverlap(candidate, other)) {
            overlapsOther = true;
            break;
          }
        }
        if (overlapsOther) {
          continue;
        }
        const long long dx = static_cast<long long>(snapped.x) - proposedX;
        const long long dy = static_cast<long long>(snapped.y) - proposedY;
        long long dist = dx * dx + dy * dy;
        if (staying) {
          dist = dist > kStaySideBias ? dist - kStaySideBias : 0;
        }
        if (!best.has_value() || dist < bestDist) {
          bestDist = dist;
          best = SnapResult{
              .position = snapped,
              .referenceName = output.name,
              .direction = direction,
          };
        }
      }
    }
    return best;
  }

  std::vector<ResolutionChoice> resolutionChoices(const Output& output) {
    std::vector<ResolutionChoice> choices;
    for (const auto& mode : output.modes) {
      auto existing = std::ranges::find_if(choices, [&](const ResolutionChoice& choice) {
        return choice.width == mode.width && choice.height == mode.height;
      });
      if (existing == choices.end()) {
        choices.push_back({.width = mode.width, .height = mode.height, .preferred = mode.preferred});
      } else if (mode.preferred) {
        existing->preferred = true;
      }
    }
    std::ranges::sort(choices, [](const ResolutionChoice& a, const ResolutionChoice& b) {
      const int aPixels = a.width * a.height;
      const int bPixels = b.width * b.height;
      if (aPixels != bPixels) {
        return aPixels > bPixels;
      }
      if (a.width != b.width) {
        return a.width > b.width;
      }
      return a.height > b.height;
    });
    return choices;
  }

  std::vector<Mode> refreshChoices(const Output& output, int width, int height) {
    std::vector<Mode> choices;
    for (const auto& mode : output.modes) {
      if (mode.width != width || mode.height != height) {
        continue;
      }
      if (std::ranges::any_of(choices, [&](const Mode& existing) {
            return existing.refreshMilliHz == mode.refreshMilliHz;
          })) {
        continue;
      }
      choices.push_back(mode);
    }
    std::ranges::sort(choices, [](const Mode& a, const Mode& b) {
      if (a.refreshMilliHz != b.refreshMilliHz) {
        return a.refreshMilliHz > b.refreshMilliHz;
      }
      return a.preferred && !b.preferred;
    });
    return choices;
  }

  std::size_t selectedResolutionIndex(const Output& output, const std::vector<ResolutionChoice>& choices) {
    for (std::size_t index = 0; index < choices.size(); ++index) {
      if (choices[index].width == output.current.width && choices[index].height == output.current.height) {
        return index;
      }
    }
    return 0;
  }

  std::size_t selectedRefreshIndex(const Output& output, const std::vector<Mode>& choices) {
    for (std::size_t index = 0; index < choices.size(); ++index) {
      if (choices[index].refreshMilliHz == output.current.refreshMilliHz) {
        return index;
      }
    }
    return 0;
  }

  std::optional<std::uint32_t> bestRefreshMilliHz(const Output& output, int width, int height) noexcept {
    const auto choices = refreshChoices(output, width, height);
    if (choices.empty()) {
      return std::nullopt;
    }
    for (const auto& choice : choices) {
      if (choice.refreshMilliHz == output.current.refreshMilliHz) {
        return choice.refreshMilliHz;
      }
    }
    for (const auto& choice : choices) {
      if (choice.preferred) {
        return choice.refreshMilliHz;
      }
    }
    return choices.front().refreshMilliHz;
  }

  std::vector<Output> parseOutputsReply(const nlohmann::json& reply) {
    const auto* outputs = outputsObject(reply);
    if (outputs == nullptr) {
      return {};
    }

    std::vector<Output> result;
    for (auto it = outputs->begin(); it != outputs->end(); ++it) {
      if (!it.value().is_object()) {
        continue;
      }
      const auto& raw = it.value();
      Output output;
      output.name = it.key();
      if (output.name.empty()) {
        output.name = asString(raw.value("name", nlohmann::json{}));
      }
      if (output.name.empty()) {
        continue;
      }
      output.make = asString(raw.value("make", nlohmann::json{}));
      output.model = asString(raw.value("model", nlohmann::json{}));
      output.serial = asString(raw.value("serial", nlohmann::json{}));
      output.vrrSupported = raw.value("vrr_supported", false);
      output.vrrEnabled = raw.value("vrr_enabled", false);

      if (const auto modesIt = raw.find("modes"); modesIt != raw.end() && modesIt->is_array()) {
        for (const auto& entry : *modesIt) {
          if (auto mode = parseMode(entry); mode.has_value()) {
            output.modes.push_back(*mode);
          }
        }
      }

      const auto currentMode = raw.value("current_mode", nlohmann::json{});
      if (const auto index = asInt(currentMode); index.has_value() && *index >= 0
          && static_cast<std::size_t>(*index) < output.modes.size()) {
        output.current = output.modes[static_cast<std::size_t>(*index)];
      }

      if (const auto logicalIt = raw.find("logical"); logicalIt != raw.end() && logicalIt->is_object()) {
        output.enabled = true;
        output.x = asInt(logicalIt->value("x", nlohmann::json{})).value_or(0);
        output.y = asInt(logicalIt->value("y", nlohmann::json{})).value_or(0);
        output.logicalWidth = asInt(logicalIt->value("width", nlohmann::json{})).value_or(0);
        output.logicalHeight = asInt(logicalIt->value("height", nlohmann::json{})).value_or(0);
        output.scale = asDouble(logicalIt->value("scale", nlohmann::json{})).value_or(1.0);
        if (const auto transform = parseTransform(asString(logicalIt->value("transform", nlohmann::json{})));
            transform.has_value()) {
          output.transform = *transform;
        }
      }

      result.push_back(std::move(output));
    }

    std::ranges::sort(result, [](const Output& a, const Output& b) {
      if (a.enabled != b.enabled) {
        return a.enabled && !b.enabled;
      }
      if (a.x != b.x) {
        return a.x < b.x;
      }
      if (a.y != b.y) {
        return a.y < b.y;
      }
      return a.name < b.name;
    });
    return result;
  }

  std::string parseError(const nlohmann::json& reply) {
    if (!reply.is_object()) {
      return {};
    }
    const auto errIt = reply.find("Err");
    if (errIt == reply.end()) {
      return {};
    }
    if (errIt->is_string()) {
      return errIt->get<std::string>();
    }
    return errIt->dump();
  }

  nlohmann::json modeAction(int width, int height, double refreshHz) {
    nlohmann::json mode = {
        {"width", width},
        {"height", height},
    };
    if (refreshHz > 0.0) {
      mode["refresh"] = refreshHz;
    }
    return nlohmann::json{
        {"Mode", {{"mode", nlohmann::json{{"Specific", std::move(mode)}}}}},
    };
  }

  nlohmann::json scaleAction(double scale) {
    return nlohmann::json{
        {"Scale", {{"scale", nlohmann::json{{"Specific", scale}}}}},
    };
  }

  nlohmann::json transformAction(Transform transform) {
    return nlohmann::json{
        {"Transform", {{"transform", jsonName(transform)}}},
    };
  }

  nlohmann::json positionAction(int x, int y) {
    return nlohmann::json{
        {"Position", {{"position", nlohmann::json{{"Specific", {{"x", x}, {"y", y}}}}}}},
    };
  }

  nlohmann::json vrrAction(bool enabled, bool onDemand) {
    return nlohmann::json{
        {"Vrr", {{"vrr", {{"vrr", enabled}, {"on_demand", onDemand}}}}},
    };
  }

  nlohmann::json powerAction(bool on) { return on ? nlohmann::json("On") : nlohmann::json("Off"); }

  nlohmann::json outputRequest(std::string_view name, const nlohmann::json& action) {
    return nlohmann::json{
        {"Output", {{"output", name}, {"action", action}}},
    };
  }

  std::string outputRequestPayload(std::string_view name, const nlohmann::json& action) {
    auto payload = outputRequest(name, action).dump();
    payload.push_back('\n');
    return payload;
  }

} // namespace display_config
