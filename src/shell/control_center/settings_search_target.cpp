#include "shell/control_center/settings_search_target.h"

#include <string>
#include <unordered_map>
#include <utility>

namespace control_center {
  namespace {

    int sectionMatchCount(
        const std::unordered_map<std::string, int>& counts, std::string_view sectionId
    ) {
      const auto it = counts.find(std::string(sectionId));
      return it == counts.end() ? 0 : it->second;
    }

    std::unordered_map<std::string, int> sectionMatchCounts(
        std::span<const settings::SettingEntry> registry, std::string_view normalizedQuery
    ) {
      std::unordered_map<std::string, int> counts;
      if (normalizedQuery.empty()) {
        return counts;
      }
      for (const auto& entry : registry) {
        if (!settings::matchesNormalizedSettingQuery(entry, normalizedQuery)) {
          continue;
        }
        ++counts[std::string(settings::settingsSectionId(entry.section))];
      }
      return counts;
    }

    bool currentSectionMatches(
        const std::unordered_map<std::string, int>& counts, std::string_view currentTabId,
        std::span<const std::string> currentSectionIds
    ) {
      if (currentSectionIds.empty()) {
        return sectionMatchCount(counts, currentTabId) > 0;
      }
      for (const auto& sectionId : currentSectionIds) {
        if (sectionMatchCount(counts, sectionId) > 0) {
          return true;
        }
      }
      return false;
    }

    std::string bestMatchingSectionId(const std::unordered_map<std::string, int>& counts) {
      int bestCount = 0;
      std::string bestSection;
      for (const auto& [section, count] : counts) {
        if (count > bestCount) {
          bestCount = count;
          bestSection = section;
        }
      }
      if (bestCount <= 0) {
        return {};
      }
      return bestSection;
    }

  } // namespace

  std::string tabIdForSettingsSection(std::string_view section) {
    if (section.empty() || section == "appearance") {
      return "theme";
    }
    if (section == "glass"
        || section == "icons"
        || section == "type"
        || section == "scale"
        || section == "roundness"
        || section == "effects"
        || section == "language") {
      return "theme";
    }
    if (section == "location") {
      return "weather";
    }
    if (section == "services") {
      return "calendar";
    }
    if (section == "screenshot" || section == "screen-record") {
      return "capture";
    }
    if (section == "ai-usage") {
      return "ai";
    }
    return std::string(section);
  }

  SettingsSearchTarget resolveSettingsSearchTarget(
      std::string_view currentTabId, bool hasSettingsPane, std::span<const std::string> currentSectionIds,
      std::span<const settings::SettingEntry> registry, std::string_view query
  ) {
    const std::string normalized = settings::normalizedSettingQuery(query);
    const auto counts = sectionMatchCounts(registry, normalized);
    const bool localMatch = currentSectionMatches(counts, currentTabId, currentSectionIds);
    const std::string bestSection = bestMatchingSectionId(counts);
    const std::string bestTab = bestSection.empty() ? std::string{} : tabIdForSettingsSection(bestSection);

    SettingsSearchTarget out;
    out.tabId = std::string(currentTabId);
    out.sectionId = bestSection;
    out.stayOnCurrent = true;
    out.configureMode = hasSettingsPane;

    if (hasSettingsPane && (localMatch || bestTab.empty())) {
      return out;
    }
    if (!bestTab.empty() && bestTab != currentTabId) {
      out.tabId = bestTab;
      out.stayOnCurrent = false;
      out.configureMode = true;
      return out;
    }
    if (hasSettingsPane) {
      return out;
    }
    out.configureMode = false;
    return out;
  }

} // namespace control_center
