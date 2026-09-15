#pragma once

#include "system/desktop_entry.h"

#include <string>
#include <string_view>
#include <vector>

namespace shell::dock::pinned_apps {

  [[nodiscard]] bool matchesEntry(const DesktopEntry& entry, std::string_view pinnedId);
  [[nodiscard]] bool containsEntry(const std::vector<std::string>& pinned, const DesktopEntry& entry);
  [[nodiscard]] bool isUnresolved(const DesktopEntry& entry) noexcept;
  void removeEntry(std::vector<std::string>& pinned, const DesktopEntry& entry);
  [[nodiscard]] std::vector<DesktopEntry> resolveEntries(const std::vector<std::string>& pinned);
  void notifyUnresolved(const DesktopEntry& entry);

} // namespace shell::dock::pinned_apps
