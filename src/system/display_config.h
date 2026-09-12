#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace display_config {

  enum class Transform : std::uint8_t {
    Normal,
    Rotate90,
    Rotate180,
    Rotate270,
    Flipped,
    Flipped90,
    Flipped180,
    Flipped270,
  };

  enum class LayoutDirection : std::uint8_t {
    Left,
    Right,
    Top,
    Bottom,
  };

  struct Mode {
    int width = 0;
    int height = 0;
    std::uint32_t refreshMilliHz = 0;
    bool preferred = false;

    [[nodiscard]] bool operator==(const Mode&) const = default;
  };

  struct ResolutionChoice {
    int width = 0;
    int height = 0;
    bool preferred = false;
  };

  struct Output {
    std::string name;
    std::string make;
    std::string model;
    std::string serial;
    bool enabled = false;
    bool vrrSupported = false;
    bool vrrEnabled = false;
    int x = 0;
    int y = 0;
    int logicalWidth = 0;
    int logicalHeight = 0;
    double scale = 1.0;
    Transform transform = Transform::Normal;
    Mode current;
    std::vector<Mode> modes;

    [[nodiscard]] bool operator==(const Output&) const = default;
  };

  struct Size {
    int width = 0;
    int height = 0;
  };

  struct Point {
    int x = 0;
    int y = 0;
  };

  [[nodiscard]] bool isSideways(Transform transform) noexcept;
  [[nodiscard]] std::string_view jsonName(Transform transform) noexcept;
  [[nodiscard]] std::optional<Transform> parseTransform(std::string_view value);
  [[nodiscard]] std::string formatRefresh(std::uint32_t refreshMilliHz);
  [[nodiscard]] std::string formatResolution(int width, int height);
  [[nodiscard]] std::string formatScale(double scale);
  [[nodiscard]] std::string displayLabel(const Output& output);
  [[nodiscard]] Size logicalSize(const Output& output);
  [[nodiscard]] Point snapPosition(
      const Output& moving, const Output& reference, LayoutDirection direction, int offset = 0
  );
  [[nodiscard]] std::vector<ResolutionChoice> resolutionChoices(const Output& output);
  [[nodiscard]] std::vector<Mode> refreshChoices(const Output& output, int width, int height);
  [[nodiscard]] std::size_t selectedResolutionIndex(const Output& output, const std::vector<ResolutionChoice>& choices);
  [[nodiscard]] std::size_t selectedRefreshIndex(const Output& output, const std::vector<Mode>& choices);
  [[nodiscard]] std::optional<std::uint32_t>
  bestRefreshMilliHz(const Output& output, int width, int height) noexcept;

  [[nodiscard]] std::vector<Output> parseOutputsReply(const nlohmann::json& reply);
  [[nodiscard]] std::string parseError(const nlohmann::json& reply);

  [[nodiscard]] nlohmann::json modeAction(int width, int height, double refreshHz);
  [[nodiscard]] nlohmann::json scaleAction(double scale);
  [[nodiscard]] nlohmann::json transformAction(Transform transform);
  [[nodiscard]] nlohmann::json positionAction(int x, int y);
  [[nodiscard]] nlohmann::json vrrAction(bool enabled, bool onDemand = false);
  [[nodiscard]] nlohmann::json powerAction(bool on);
  [[nodiscard]] nlohmann::json outputRequest(std::string_view name, const nlohmann::json& action);
  [[nodiscard]] std::string outputRequestPayload(std::string_view name, const nlohmann::json& action);

} // namespace display_config
