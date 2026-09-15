#pragma once

#include <string_view>

namespace control_center {

  // Score a Control Center tab against a typed query. 0 means no match.
  // Title, id (including hyphen/space variants), and a few aliases all count.
  [[nodiscard]] int tabSearchScore(std::string_view id, std::string_view title, std::string_view query);

  [[nodiscard]] inline bool tabSearchMatches(std::string_view id, std::string_view title, std::string_view query) {
    return tabSearchScore(id, title, query) > 0;
  }

} // namespace control_center
