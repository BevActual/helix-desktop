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
class CalendarService;
class ClipboardService;
class CompositorPlatform;
class ConfigService;
class DependencyService;
class EasyEffectsService;
class ExternalIpService;
class FileWatcher;
class HttpClient;
class IdleInhibitor;
class INetworkService;
class IpcService;
class ModemManagerService;
class MprisService;
class NetworkSecretAgent;
class NotificationManager;
class PipeWireService;
class PipeWireSpectrum;
class PowerProfilesService;
class ScreenRecorderService;
class ScreenTimeService;
class SessionActionRunner;
class SystemMonitorService;
class TailscaleService;
class ThumbnailService;
class UPowerService;
class Wallpaper;
class WeatherService;
class GammaService;
class WallpaperScanner;

namespace noctalia::theme {
  class ThemeService;
  class TemplateApplyService;
}
namespace scripting {
  class ScriptApiContext;
  class PluginManager;
}
namespace settings {
  class HostedEditors;
  class SettingsModalHost;
}

struct ControlCenterServices {
  NotificationManager* notifications = nullptr;
  PipeWireService* audio = nullptr;
  EasyEffectsService* easyEffects = nullptr;
  MprisService* mpris = nullptr;
  ConfigService* config = nullptr;
  HttpClient* httpClient = nullptr;
  WeatherService* weather = nullptr;
  PipeWireSpectrum* spectrum = nullptr;
  UPowerService* upower = nullptr;
  PowerProfilesService* powerProfiles = nullptr;
  INetworkService* network = nullptr;
  ModemManagerService* modem = nullptr;
  NetworkSecretAgent* networkSecrets = nullptr;
  ExternalIpService* externalIp = nullptr;
  BluetoothService* bluetooth = nullptr;
  BluetoothAgent* bluetoothAgent = nullptr;
  BrightnessService* brightness = nullptr;
  DisplayConfigService* displayConfig = nullptr;
  SystemMonitorService* sysmon = nullptr;
  ScreenTimeService* screenTime = nullptr;
  GammaService* nightLight = nullptr;
  noctalia::theme::ThemeService* theme = nullptr;
  noctalia::theme::TemplateApplyService* templateApply = nullptr;
  IdleInhibitor* idleInhibitor = nullptr;
  SessionActionRunner* session = nullptr;
  DependencyService* dependencies = nullptr;
  CompositorPlatform* platform = nullptr;
  IpcService* ipc = nullptr;
  Wallpaper* wallpaper = nullptr;
  CalendarService* calendar = nullptr;
  scripting::ScriptApiContext* scriptApi = nullptr;
  FileWatcher* fileWatcher = nullptr;
  ClipboardService* clipboard = nullptr;
  std::function<void(bool isImage)> scheduleClipboardAutoPaste;
  AccountsService* accounts = nullptr;
  ThumbnailService* thumbnails = nullptr;
  WallpaperScanner* wallpaperScanner = nullptr;
  AsyncTextureCache* asyncTextures = nullptr;
  scripting::PluginManager* pluginManager = nullptr;
  ScreenRecorderService* screenRecorder = nullptr;
  TailscaleService* tailscale = nullptr;
  settings::HostedEditors* hostedEditors = nullptr;
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
        .weather = weather,
        .config = config,
        .dependencies = dependencies,
        .platform = platform,
        .ipc = ipc,
        .screenRecorder = screenRecorder,
        .tailscale = tailscale,
        .scriptApi = scriptApi,
        .fileWatcher = fileWatcher,
        .httpClient = httpClient,
        .clipboard = clipboard,
    };
  }
};
