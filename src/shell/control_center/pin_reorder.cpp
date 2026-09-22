#include "shell/control_center/pin_reorder.h"

#include <algorithm>
#include <iterator>

namespace shell::control_center::pin_reorder {

  std::size_t
  dropSlot(std::span<const float> restY, std::span<const float> heights, std::size_t source, float offset) noexcept {
    const std::size_t count = restY.size();
    if (count == 0 || heights.size() != count || source >= count) {
      return source;
    }
    // Half-overlap rule: a neighbour swaps once the lifted pin's leading edge crosses its centre.
    const float top = restY[source] + offset;
    const float bottom = top + heights[source];
    std::size_t slot = 0;
    for (std::size_t i = 0; i < count; ++i) {
      if (i == source) {
        continue;
      }
      const float centre = restY[i] + heights[i] * 0.5F;
      if (i < source ? top >= centre : bottom > centre) {
        ++slot; // the lifted pin still sits below this neighbour
      }
    }
    return std::min(slot, count - 1);
  }

  int rowShift(std::size_t index, std::size_t source, std::size_t slot) noexcept {
    if (index == source) {
      return 0;
    }
    // A pin below the source sits at `index - 1` in the list without the lifted pin.
    if (index < source && index >= slot) {
      return 1;
    }
    if (index > source && index <= slot) {
      return -1;
    }
    return 0;
  }

  std::optional<std::vector<ShortcutConfig>> reorder(
      const std::vector<ShortcutConfig>& order, std::span<const std::string> padIds, std::size_t source,
      std::size_t slot
  ) {
    const std::size_t padCount = padIds.size();
    if (source >= padCount || slot == source) {
      return std::nullopt;
    }
    // Map each pad to its config entry by id; duplicates pair up in order.
    std::vector<std::size_t> padToConfig;
    padToConfig.reserve(padCount);
    std::vector<bool> used(order.size(), false);
    for (const auto& id : padIds) {
      std::size_t found = order.size();
      for (std::size_t i = 0; i < order.size(); ++i) {
        if (!used[i] && order[i].type == id) {
          found = i;
          break;
        }
      }
      if (found < order.size()) {
        used[found] = true;
      }
      padToConfig.push_back(found);
    }
    const std::size_t sourceCfg = padToConfig[source];
    if (sourceCfg >= order.size()) {
      return std::nullopt;
    }
    // The pad the lifted one lands in front of, in the full pad list.
    std::size_t padBefore = slot >= source ? slot + 1 : slot;
    while (padBefore < padCount && padToConfig[padBefore] >= order.size()) {
      ++padBefore;
    }
    std::size_t beforeCfg = padBefore < padCount ? padToConfig[padBefore] : order.size();

    std::vector<ShortcutConfig> next = order;
    ShortcutConfig moved = std::move(next[sourceCfg]);
    next.erase(next.begin() + static_cast<std::ptrdiff_t>(sourceCfg));
    if (beforeCfg > sourceCfg) {
      --beforeCfg;
    }
    next.insert(next.begin() + static_cast<std::ptrdiff_t>(std::min(beforeCfg, next.size())), std::move(moved));
    if (next == order) {
      return std::nullopt;
    }
    return next;
  }

} // namespace shell::control_center::pin_reorder
