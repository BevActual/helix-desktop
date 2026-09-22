// Home pins drag-to-reorder: drop slot from geometry, neighbour row shifts while hovering, and
// the resulting `control_center.shortcuts` order (including config entries with no pad).

#include "shell/control_center/pin_reorder.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

  using namespace shell::control_center::pin_reorder;

  bool check(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
  }

  std::vector<ShortcutConfig> shortcuts(std::initializer_list<const char*> types) {
    std::vector<ShortcutConfig> out;
    for (const char* t : types) {
      out.push_back(ShortcutConfig{.type = t});
    }
    return out;
  }

  std::vector<std::string> types(const std::vector<ShortcutConfig>& list) {
    std::vector<std::string> out;
    for (const auto& sc : list) {
      out.push_back(sc.type);
    }
    return out;
  }

} // namespace

int main() {
  bool ok = true;

  // Four 40px rows at 0/40/80/120.
  const std::vector<float> rest{0.0F, 40.0F, 80.0F, 120.0F};
  const std::vector<float> heights{40.0F, 40.0F, 40.0F, 40.0F};

  // --- dropSlot ---
  ok = check(dropSlot(rest, heights, 1, 0.0F) == 1, "no movement keeps the slot") && ok;
  ok = check(dropSlot(rest, heights, 1, 15.0F) == 1, "under half a row stays put") && ok;
  ok = check(dropSlot(rest, heights, 1, 25.0F) == 2, "past the next centre moves one slot down") && ok;
  ok = check(dropSlot(rest, heights, 1, 200.0F) == 3, "far below clamps to the last slot") && ok;
  ok = check(dropSlot(rest, heights, 2, -25.0F) == 1, "past the previous centre moves one slot up") && ok;
  ok = check(dropSlot(rest, heights, 2, -200.0F) == 0, "far above clamps to the first slot") && ok;
  ok = check(dropSlot(rest, heights, 0, -10.0F) == 0, "top pin dragged up stays first") && ok;
  ok = check(dropSlot({}, {}, 0, 10.0F) == 0, "empty list is inert") && ok;

  // --- rowShift ---
  // Source 1 hovering slot 3 (bottom): pins 2 and 3 slide up, pin 0 stays.
  ok = check(rowShift(0, 1, 3) == 0, "pin above an untouched span does not move") && ok;
  ok = check(rowShift(2, 1, 3) == -1, "pin between old and new slot slides up") && ok;
  ok = check(rowShift(3, 1, 3) == -1, "last pin slides up when source lands below it") && ok;
  // Source 1 hovering slot 2: only pin 2 slides up.
  ok = check(rowShift(2, 1, 2) == -1, "single pin crossed slides up") && ok;
  ok = check(rowShift(3, 1, 2) == 0, "pin beyond the new slot stays") && ok;
  // Source 2 hovering slot 0: pins 0 and 1 slide down.
  ok = check(rowShift(0, 2, 0) == 1, "top pin slides down when source lands above it") && ok;
  ok = check(rowShift(1, 2, 0) == 1, "pin just above the source slides down") && ok;
  ok = check(rowShift(3, 2, 0) == 0, "pin below the source stays when moving up") && ok;
  ok = check(rowShift(1, 1, 3) == 0, "source itself never shifts") && ok;
  ok = check(rowShift(0, 1, 1) == 0 && rowShift(2, 1, 1) == 0, "slot == source is a no-op") && ok;

  // --- reorder ---
  const auto order = shortcuts({"wifi", "bluetooth", "dnd", "night-light"});
  const std::vector<std::string> pads{"wifi", "bluetooth", "dnd", "night-light"};
  {
    const auto next = reorder(order, pads, 0, 2);
    ok = check(next.has_value(), "moving first pin down changes the order") && ok;
    ok = check(
             next.has_value() && types(*next) == std::vector<std::string>{"bluetooth", "dnd", "wifi", "night-light"},
             "wifi lands after dnd"
         )
        && ok;
  }
  {
    const auto next = reorder(order, pads, 3, 0);
    ok = check(
             next.has_value() && types(*next) == std::vector<std::string>{"night-light", "wifi", "bluetooth", "dnd"},
             "last pin to the top"
         )
        && ok;
  }
  {
    const auto next = reorder(order, pads, 1, 3);
    ok = check(
             next.has_value() && types(*next) == std::vector<std::string>{"wifi", "dnd", "night-light", "bluetooth"},
             "second pin to the bottom"
         )
        && ok;
  }
  ok = check(!reorder(order, pads, 2, 2).has_value(), "dropping in place is a no-op") && ok;
  ok = check(!reorder(order, pads, 9, 0).has_value(), "out-of-range source is a no-op") && ok;

  // A config entry with no pad (unknown plugin type) keeps its place relative to its neighbours.
  {
    const auto withGhost = shortcuts({"wifi", "plugin:missing", "bluetooth", "dnd"});
    const std::vector<std::string> shownPads{"wifi", "bluetooth", "dnd"};
    const auto next = reorder(withGhost, shownPads, 2, 0); // dnd to the top
    ok = check(
             next.has_value()
                 && types(*next) == std::vector<std::string>{"dnd", "wifi", "plugin:missing", "bluetooth"},
             "hidden entry stays between its neighbours when a pad moves above"
         )
        && ok;
    const auto down = reorder(withGhost, shownPads, 0, 1); // wifi below bluetooth
    ok = check(
             down.has_value()
                 && types(*down) == std::vector<std::string>{"plugin:missing", "bluetooth", "wifi", "dnd"},
             "moving past a hidden entry inserts before the next shown pad"
         )
        && ok;
  }

  // Duplicate types pair up in order.
  {
    const auto dupes = shortcuts({"wifi", "wifi", "dnd"});
    const std::vector<std::string> dupePads{"wifi", "wifi", "dnd"};
    const auto next = reorder(dupes, dupePads, 2, 0);
    ok = check(
             next.has_value() && types(*next) == std::vector<std::string>{"dnd", "wifi", "wifi"},
             "duplicates keep working"
         )
        && ok;
  }

  if (!ok) {
    return 1;
  }
  std::cout << "pin_reorder_test: OK\n";
  return 0;
}
