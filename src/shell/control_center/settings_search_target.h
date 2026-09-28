#pragma once

#include "shell/settings/settings_registry.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace control_center {

  struct SettingsSearchTarget {
    std::string tabId;
    std::string sectionId;
    bool stayOnCurrent = true;
    bool configureMode = false;
  };

  // Maps a settings registry section id to the Control Center tab that hosts it.
  [[nodiscard]] std::string tabIdForSettingsSection(std::string_view section);
  // Like tabIdForSettingsSection, but routes groups of shared sections to the tab that shows
  // them (services/audio -> audio, shell/clipboard -> clipboard, ...).
  [[nodiscard]] std::string tabIdForSettingsGroup(std::string_view section, std::string_view group);

  // Stay on the current tab's gear sheet when that section matches. Jump to the
  // best matching section only when the current one has no hits. Never dump onto
  // Appearance unless Appearance actually matches.
  [[nodiscard]] SettingsSearchTarget resolveSettingsSearchTarget(
      std::string_view currentTabId, bool hasSettingsPane, std::span<const std::string> currentSectionIds,
      std::span<const settings::SettingEntry> registry, std::string_view query
  );

} // namespace control_center
