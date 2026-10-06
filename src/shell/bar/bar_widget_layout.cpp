#include "shell/bar/bar_widget_layout.h"

#include "config/config_service.h"

#include <algorithm>
#include <utility>

namespace noctalia::bar::layout {
  namespace {

    const BarConfig* findBar(const Config& cfg, std::string_view name) {
      const auto it = std::ranges::find(cfg.bars, name, &BarConfig::name);
      return it != cfg.bars.end() ? &*it : nullptr;
    }

    const BarMonitorOverride* findMonitor(const BarConfig& bar, std::string_view match) {
      const auto it = std::ranges::find(bar.monitorOverrides, match, &BarMonitorOverride::match);
      return it != bar.monitorOverrides.end() ? &*it : nullptr;
    }

    bool moveIndex(std::vector<std::string>& items, std::size_t from, std::size_t to) {
      if (from >= items.size() || to >= items.size() || from == to) {
        return false;
      }
      std::string moved = std::move(items[from]);
      items.erase(items.begin() + static_cast<std::ptrdiff_t>(from));
      const std::size_t insert = std::min(to, items.size());
      items.insert(items.begin() + static_cast<std::ptrdiff_t>(insert), std::move(moved));
      return true;
    }

  } // namespace

  std::vector<std::string>
  lanePath(std::string_view barName, std::string_view lane, std::optional<std::string_view> monitorMatch) {
    if (monitorMatch.has_value() && !monitorMatch->empty()) {
      return {"bar", std::string(barName), "monitor", std::string(*monitorMatch), std::string(lane)};
    }
    return {"bar", std::string(barName), std::string(lane)};
  }

  std::vector<std::string> capsuleGroupPath(const std::vector<std::string>& lanePath) {
    std::vector<std::string> path(lanePath.begin(), lanePath.end() - 1);
    path.emplace_back("capsule_group");
    return path;
  }

  const std::vector<std::string>* laneWidgets(const Config& cfg, const std::vector<std::string>& path) {
    if (path.size() < 3 || path[0] != "bar") {
      return nullptr;
    }
    const BarConfig* bar = findBar(cfg, path[1]);
    if (bar == nullptr) {
      return nullptr;
    }
    const std::string& lane = path.back();
    if (path.size() == 5 && path[2] == "monitor") {
      const BarMonitorOverride* ovr = findMonitor(*bar, path[3]);
      if (ovr == nullptr) {
        return nullptr;
      }
      const std::optional<std::vector<std::string>>& laneOverride =
          lane == "start" ? ovr->startWidgets : (lane == "center" ? ovr->centerWidgets : ovr->endWidgets);
      if (laneOverride.has_value()) {
        return &*laneOverride;
      }
    }
    return lane == "start" ? &bar->startWidgets : (lane == "center" ? &bar->centerWidgets : &bar->endWidgets);
  }

  const std::vector<BarCapsuleGroupStyle>* capsuleGroups(const Config& cfg, const std::vector<std::string>& path) {
    if (path.size() < 3 || path[0] != "bar") {
      return nullptr;
    }
    const BarConfig* bar = findBar(cfg, path[1]);
    if (bar == nullptr) {
      return nullptr;
    }
    if (path.size() == 5 && path[2] == "monitor") {
      const BarMonitorOverride* ovr = findMonitor(*bar, path[3]);
      if (ovr != nullptr && ovr->widgetCapsuleGroups.has_value()) {
        return &*ovr->widgetCapsuleGroups;
      }
    }
    return &bar->widgetCapsuleGroups;
  }

  bool reorderLaneEntry(
      ConfigService& config, const std::vector<std::string>& path, std::size_t fromIndex, std::size_t toIndex
  ) {
    const std::vector<std::string>* current = laneWidgets(config.config(), path);
    if (current == nullptr) {
      return false;
    }
    std::vector<std::string> next = *current;
    if (!moveIndex(next, fromIndex, toIndex)) {
      return false;
    }
    return config.setOverride(path, std::move(next));
  }

  bool reorderCapsuleMember(
      ConfigService& config, const std::vector<std::string>& path, std::string_view groupId, std::size_t fromIndex,
      std::size_t toIndex
  ) {
    const std::vector<BarCapsuleGroupStyle>* current = capsuleGroups(config.config(), path);
    if (current == nullptr) {
      return false;
    }
    std::vector<BarCapsuleGroupStyle> next = *current;
    auto it = std::ranges::find(next, groupId, &BarCapsuleGroupStyle::id);
    if (it == next.end()) {
      return false;
    }
    if (!moveIndex(it->members, fromIndex, toIndex)) {
      return false;
    }
    return config.setOverride(capsuleGroupPath(path), std::move(next));
  }

} // namespace noctalia::bar::layout
