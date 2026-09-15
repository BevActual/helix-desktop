#include "shell/dock/pinned_apps.h"

#include "core/log.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "system/internal_app_metadata.h"
#include "util/string_utils.h"

#include <algorithm>

namespace shell::dock::pinned_apps {
  namespace {

    constexpr Logger kLog("dock");

    [[nodiscard]] DesktopEntry placeholderEntry(std::string_view pinnedId) {
      DesktopEntry placeholder;
      placeholder.id = std::string(pinnedId);
      placeholder.name = i18n::tr("dock.missing-app");
      placeholder.nameLower = StringUtils::toLower(placeholder.name);
      placeholder.comment = std::string(pinnedId);
      return placeholder;
    }

  } // namespace

  bool matchesEntry(const DesktopEntry& entry, std::string_view pinnedId) {
    if (pinnedId.empty()) {
      return false;
    }

    return entry.id == pinnedId;
  }

  bool containsEntry(const std::vector<std::string>& pinned, const DesktopEntry& entry) {
    return std::ranges::any_of(pinned, [&](const std::string& pinnedId) { return matchesEntry(entry, pinnedId); });
  }

  void removeEntry(std::vector<std::string>& pinned, const DesktopEntry& entry) {
    std::erase_if(pinned, [&](const std::string& pinnedId) { return matchesEntry(entry, pinnedId); });
  }

  std::vector<DesktopEntry> resolveEntries(const std::vector<std::string>& pinned) {
    std::vector<DesktopEntry> resolved;
    resolved.reserve(pinned.size());

    const auto& entries = desktopEntries();
    for (const auto& pinnedId : pinned) {
      const auto match = std::ranges::find_if(entries, [&](const DesktopEntry& entry) {
        return !entry.hidden && !entry.noDisplay && entry.id == pinnedId;
      });

      DesktopEntry entry = match != entries.end() ? *match : [&]() {
        kLog.debug("pinned app not found: {}", pinnedId);
        return placeholderEntry(pinnedId);
      }();
      internal_apps::applyMetadataToDesktopEntry(entry);
      resolved.push_back(std::move(entry));
    }

    return resolved;
  }

  bool isUnresolved(const DesktopEntry& entry) noexcept {
    return entry.exec.empty() && entry.path.empty() && !entry.dbusActivatable;
  }

  void notifyUnresolved(const DesktopEntry& entry) {
    notify::error(
        "Noctalia", i18n::tr("dock.missing-app"), i18n::tr("dock.missing-app-body", "id", entry.id)
    );
  }

} // namespace shell::dock::pinned_apps
