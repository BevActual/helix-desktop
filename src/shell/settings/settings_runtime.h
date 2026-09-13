#pragma once

#include "shell/settings/settings_registry.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

class CalendarService;
class ClipboardService;

namespace settings {

  struct RuntimeActions {
    CalendarService* calendar = nullptr;
    ClipboardService* clipboard = nullptr;
    bool* pendingEncryptedStorageReset = nullptr;
    std::function<void()> requestRebuild;
    std::function<void()> openCommunityTemplateStore;
    std::function<void()> retryCalendarCredentials;
    std::function<void()> retryCalendarStorage;
    std::function<void()> retryClipboardPersistence;
    std::function<void()> resetEncryptedStorage;
    std::function<void()> syncGreeterAppearance;
    std::function<void()> resetLauncherUsage;
    std::function<void()> resetScreenTime;
    std::function<void()> saveWallpaperPaletteAsCustom;
    std::function<void()> openDesktopWidgetEditor;
    std::function<void()> openLockscreenWidgetEditor;
    std::function<void(std::optional<std::string>)> openCalendarAccount;
  };

  void enrichSettingsRegistry(
      std::vector<SettingEntry>& registry, const Config& cfg, const RegistryEnvironment& env,
      const RuntimeActions& actions
  );

} // namespace settings
