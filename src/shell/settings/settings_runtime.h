#pragma once

#include "shell/settings/settings_registry.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

class ClipboardService;

namespace settings {

  struct RuntimeActions {
    ClipboardService* clipboard = nullptr;
    bool* pendingEncryptedStorageReset = nullptr;
    std::function<void()> requestRebuild;
    std::function<void()> openCommunityTemplateStore;
    std::function<void()> retryClipboardPersistence;
    std::function<void()> resetEncryptedStorage;
    std::function<void()> syncGreeterAppearance;
    std::function<void()> applyTemplates;
    std::function<void()> resetLauncherUsage;
    std::function<void()> saveWallpaperPaletteAsCustom;
    std::function<void()> openDesktopWidgetEditor;
    std::function<void()> openLockscreenWidgetEditor;
  };

} // namespace settings
