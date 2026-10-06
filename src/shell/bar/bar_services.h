#pragma once

class BluetoothService;
class BrightnessService;
class ClipboardService;
class CompositorPlatform;
class ConfigService;
class FileWatcher;
class HttpClient;
class IdleInhibitor;
class INetworkService;
class LockKeysService;
class MprisService;
class NotificationManager;
class PipeWireService;
class PipeWireSpectrum;
class PowerProfilesService;
class RenderContext;
class ScreenshotService;
class SystemMonitorService;
class TrayService;
class UPowerService;
class GammaService;

namespace noctalia::theme {
  class ThemeService;
}

struct BarServices {
  CompositorPlatform& platform;
  ConfigService& config;
  NotificationManager* notifications = nullptr;
  TrayService* tray = nullptr;
  PipeWireService* audio = nullptr;
  UPowerService* upower = nullptr;
  SystemMonitorService* sysmon = nullptr;
  PowerProfilesService* powerProfiles = nullptr;
  INetworkService* network = nullptr;
  IdleInhibitor* idleInhibitor = nullptr;
  MprisService* mpris = nullptr;
  PipeWireSpectrum* audioSpectrum = nullptr;
  HttpClient* httpClient = nullptr;
  RenderContext* renderContext = nullptr;
  GammaService* nightLight = nullptr;
  noctalia::theme::ThemeService* theme = nullptr;
  BluetoothService* bluetooth = nullptr;
  BrightnessService* brightness = nullptr;
  LockKeysService* lockKeys = nullptr;
  ClipboardService* clipboard = nullptr;
  FileWatcher* fileWatcher = nullptr;
  ScreenshotService* screenshots = nullptr;
};
