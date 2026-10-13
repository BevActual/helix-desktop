#include "tests/test_check.h"
#include "wayland/popup_dismiss_order.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace {

  using popup_dismiss_order::Node;

  std::size_t position(const std::vector<std::size_t>& order, std::size_t index) {
    for (std::size_t i = 0; i < order.size(); ++i) {
      if (order[i] == index) {
        return i;
      }
    }
    return order.size();
  }

  bool contains(const std::vector<std::size_t>& order, std::size_t index) {
    return position(order, index) != order.size();
  }

  // Dock opens its item menu (layer parent) while a tray menu with an open submenu is up:
  // both tray popups go, submenu before its parent.
  void layerParentDismissesOtherChainsDeepestFirst() {
    const std::vector<Node> nodes{
        {.parent = std::nullopt}, // 0: tray menu
        {.parent = 0},            // 1: tray submenu
    };
    const auto order = popup_dismiss_order::conflicting(nodes, std::nullopt);
    TEST_CHECK(order.size() == 2);
    TEST_CHECK(position(order, 1) < position(order, 0));
  }

  // A submenu opening on the topmost menu keeps the whole chain it hangs off.
  void childOfTopmostKeepsItsChain() {
    const std::vector<Node> nodes{
        {.parent = std::nullopt}, // 0: tray menu
        {.parent = 0},            // 1: tray submenu (new popup's parent)
    };
    TEST_CHECK(popup_dismiss_order::conflicting(nodes, 1).empty());
  }

  // Opening a submenu on a lower menu level closes the deeper sibling branch and anything
  // unrelated, but keeps the new parent and its ancestors.
  void siblingBranchAndUnrelatedPopupsGo() {
    const std::vector<Node> nodes{
        {.parent = std::nullopt}, // 0: root menu
        {.parent = 0},            // 1: submenu A (new parent)
        {.parent = 0},            // 2: submenu B
        {.parent = 2},            // 3: submenu under B
        {.parent = std::nullopt}, // 4: bar tooltip
    };
    const auto order = popup_dismiss_order::conflicting(nodes, 1);
    TEST_CHECK(order.size() == 3);
    TEST_CHECK(!contains(order, 0));
    TEST_CHECK(!contains(order, 1));
    TEST_CHECK(contains(order, 2));
    TEST_CHECK(contains(order, 3));
    TEST_CHECK(contains(order, 4));
    TEST_CHECK(position(order, 3) < position(order, 2));
  }

  // A child popup (tooltip) of a menu that is being dismissed goes before that menu.
  void childTooltipBeforeItsMenu() {
    const std::vector<Node> nodes{
        {.parent = 1},            // 0: tooltip on the menu (listed before its parent)
        {.parent = std::nullopt}, // 1: bar context menu
    };
    const auto order = popup_dismiss_order::conflicting(nodes, std::nullopt);
    TEST_CHECK(order.size() == 2);
    TEST_CHECK(order[0] == 0);
    TEST_CHECK(order[1] == 1);
  }

  // Bad parent links (cycle, out of range) must not hang or crash.
  void malformedParentsTerminate() {
    const std::vector<Node> nodes{
        {.parent = 1},
        {.parent = 0},
        {.parent = 42},
    };
    TEST_CHECK(popup_dismiss_order::conflicting(nodes, std::nullopt).size() == 3);
    TEST_CHECK(popup_dismiss_order::conflicting(nodes, 0).size() == 1);
  }

} // namespace

int main() {
  layerParentDismissesOtherChainsDeepestFirst();
  childOfTopmostKeepsItsChain();
  siblingBranchAndUnrelatedPopupsGo();
  childTooltipBeforeItsMenu();
  malformedParentsTerminate();
  return 0;
}
