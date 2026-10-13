#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace popup_dismiss_order {

  // One live popup: `parent` indexes another popup in the same list, or is empty when the
  // popup hangs off a layer surface / toplevel.
  struct Node {
    std::optional<std::size_t> parent;
  };

  // Popups to dismiss before a new grabbing popup can open under `newParent` (an index into
  // `nodes`, or empty for a layer/toplevel parent): everything except `newParent` and its
  // ancestors. Deepest first, because xdg-shell only lets a client destroy the topmost popup
  // of a chain.
  [[nodiscard]] inline std::vector<std::size_t>
  conflicting(const std::vector<Node>& nodes, std::optional<std::size_t> newParent) {
    std::vector<bool> keep(nodes.size(), false);
    for (auto at = newParent; at.has_value() && *at < nodes.size() && !keep[*at]; at = nodes[*at].parent) {
      keep[*at] = true;
    }

    auto depth = [&nodes](std::size_t index) {
      std::size_t d = 0;
      for (auto at = nodes[index].parent; at.has_value() && *at < nodes.size() && d <= nodes.size();
           at = nodes[*at].parent) {
        ++d;
      }
      return d;
    };

    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (!keep[i]) {
        out.push_back(i);
      }
    }
    std::stable_sort(out.begin(), out.end(), [&depth](std::size_t a, std::size_t b) { return depth(a) > depth(b); });
    return out;
  }

} // namespace popup_dismiss_order
