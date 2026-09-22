#pragma once

#include "config/config_types.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Pure geometry and list math behind the Home tab's drag-to-reorder for optional pins.
// A "slot" is an insertion index in the pin list *without* the lifted pin (0..count-1),
// so `slot == source` means the pin drops back where it was.
namespace shell::control_center::pin_reorder {

  // Slot the lifted pin (index `source`, rest y `restY[source]`, moved by `offset`) would drop
  // into. A neighbour is passed once the lifted pin's leading edge crosses the neighbour's rest
  // centre (half-overlap, as iOS and Quick Settings tiles swap).
  [[nodiscard]] std::size_t
  dropSlot(std::span<const float> restY, std::span<const float> heights, std::size_t source, float offset) noexcept;

  // Rows a non-lifted pin `index` slides while the lifted pin hovers over `slot`:
  // +1 (down), -1 (up), or 0.
  [[nodiscard]] int rowShift(std::size_t index, std::size_t source, std::size_t slot) noexcept;

  // New `control_center.shortcuts` after moving the pad at `source` to `slot`. `padIds` is the
  // shortcut id of every pad shown, in pad order; config entries that produced no pad (unknown
  // type) keep their place. Returns nullopt when nothing changes.
  [[nodiscard]] std::optional<std::vector<ShortcutConfig>> reorder(
      const std::vector<ShortcutConfig>& order, std::span<const std::string> padIds, std::size_t source,
      std::size_t slot
  );

} // namespace shell::control_center::pin_reorder
