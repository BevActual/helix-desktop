#pragma once

#include "config/config_types.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ConfigService;

namespace noctalia::bar::layout {

  // Lane path: {"bar", name, "start"|"center"|"end"} or monitor-scoped equivalent.
  [[nodiscard]] std::vector<std::string>
  lanePath(std::string_view barName, std::string_view lane, std::optional<std::string_view> monitorMatch = std::nullopt);

  [[nodiscard]] std::vector<std::string> capsuleGroupPath(const std::vector<std::string>& lanePath);

  // Move one lane entry (widget name or group:<id>) within the same lane. Returns true if config changed.
  bool reorderLaneEntry(
      ConfigService& config, const std::vector<std::string>& lanePath, std::size_t fromIndex, std::size_t toIndex
  );

  // Move one capsule member within a group. Returns true if config changed.
  bool reorderCapsuleMember(
      ConfigService& config, const std::vector<std::string>& lanePath, std::string_view groupId, std::size_t fromIndex,
      std::size_t toIndex
  );

  [[nodiscard]] const std::vector<std::string>*
  laneWidgets(const Config& cfg, const std::vector<std::string>& lanePath);

  [[nodiscard]] const std::vector<BarCapsuleGroupStyle>*
  capsuleGroups(const Config& cfg, const std::vector<std::string>& lanePath);

} // namespace noctalia::bar::layout
