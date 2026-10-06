#pragma once

#include "shell/control_center/shortcut_services.h"
#include "shell/settings/settings_runtime.h"
#include "ui/signal.h"

#include <functional>

class AccountsService;
class AsyncTextureCache;
class BluetoothAgent;
class BluetoothService;
class BrightnessService;
class DisplayConfigService;
class ClipboardService;
class CompositorPlatform;
class ConfigService;
class DependencyService;
class FileWatcher;
class HttpClient;
class IdleInhibitor;
class INetworkService;
class IpcService;
class MprisService;
class NetworkSecretAgent;
class NotificationManager;
class PipeWireService;
class PipeWireSpectrum;
class PowerProfilesService;
class ScreenRecorderService;
class SessionActionRunner;
class SystemMonitorService;
class ThumbnailService;
class UPowerService;
class VideoWallpaperService;
class Wallpaper;
class GammaService;
class WallpaperScanner;
class DesktopWidgetsController;

namespace noctalia::theme {
  class ThemeService;
  class TemplateApplyService;
}
namespace settings {
  class SettingsModalHost;
}

struct ControlCenterServices {
  NotificationManager* notifications = nullptr;
  PipeWireService* audio = nullptr;
  MprisService* mpris = nullptr;
  ConfigService* config = nullptr;
  HttpClient* httpClient = nullptr;
  PipeWireSpectrum* spectrum = nullptr;
  UPowerService* upower = nullptr;
  PowerProfilesService* powerProfiles = nullptr;
  INetworkService* network = nullptr;
  NetworkSecretAgent* networkSecrets = nullptr;
  BluetoothService* bluetooth = nullptr;
  BluetoothAgent* bluetoothAgent = nullptr;
  BrightnessService* brightness = nullptr;
  DisplayConfigService* displayConfig = nullptr;
  SystemMonitorService* sysmon = nullptr;
  GammaService* nightLight = nullptr;
  noctalia::theme::ThemeService* theme = nullptr;
  noctalia::theme::TemplateApplyService* templateApply = nullptr;
  IdleInhibitor* idleInhibitor = nullptr;
  SessionActionRunner* session = nullptr;
  DependencyService* dependencies = nullptr;
  CompositorPlatform* platform = nullptr;
  IpcService* ipc = nullptr;
  Wallpaper* wallpaper = nullptr;
  FileWatcher* fileWatcher = nullptr;
  ClipboardService* clipboard = nullptr;
  std::function<void(bool isImage)> scheduleClipboardAutoPaste;
  AccountsService* accounts = nullptr;
  ThumbnailService* thumbnails = nullptr;
  WallpaperScanner* wallpaperScanner = nullptr;
  AsyncTextureCache* asyncTextures = nullptr;
  VideoWallpaperService* videoWallpaper = nullptr;
  ScreenRecorderService* screenRecorder = nullptr;
  DesktopWidgetsController* desktopWidgets = nullptr;
  std::function<void()> upgradeShell;
  std::function<bool()> upgradeAvailable;
  std::function<bool()> upgradeBusy;
  std::function<void(std::function<void(bool)>)> ensurePolkit;
  settings::SettingsModalHost* modalHost = nullptr;
  Signal<>* settingsInvalidated = nullptr;
  settings::RuntimeActions settingsActions;

  [[nodiscard]] ShortcutServices shortcutServices() const {
    return {
        .network = network,
        .bluetooth = bluetooth,
        .nightLight = nightLight,
        .theme = theme,
        .notifications = notifications,
        .idleInhibitor = idleInhibitor,
        .audio = audio,
        .powerProfiles = powerProfiles,
        .mpris = mpris,
        .config = config,
        .dependencies = dependencies,
        .platform = platform,
        .ipc = ipc,
        .screenRecorder = screenRecorder,
        .fileWatcher = fileWatcher,
        .httpClient = httpClient,
        .clipboard = clipboard,
    };
  }
};
