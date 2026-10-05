#pragma once

class BluetoothService;
class CompositorPlatform;
class ConfigService;
class DependencyService;
class FileWatcher;
class IdleInhibitor;
class INetworkService;
class MprisService;
class GammaService;
class IpcService;
class NotificationManager;
class PipeWireService;
class PowerProfilesService;
class ScreenRecorderService;
class TailscaleService;
class LocalSendService;
class PhoneConnectService;
class SyncthingService;
class FirewallService;
class PrinterService;
class CastService;
class AiUsageService;
class WeatherService;
class HttpClient;
class ClipboardService;

namespace noctalia::theme {
  class ThemeService;
}

struct ShortcutServices {
  INetworkService* network = nullptr;
  BluetoothService* bluetooth = nullptr;
  GammaService* nightLight = nullptr;
  noctalia::theme::ThemeService* theme = nullptr;
  NotificationManager* notifications = nullptr;
  IdleInhibitor* idleInhibitor = nullptr;
  PipeWireService* audio = nullptr;
  PowerProfilesService* powerProfiles = nullptr;
  MprisService* mpris = nullptr;
  WeatherService* weather = nullptr;
  ConfigService* config = nullptr;
  DependencyService* dependencies = nullptr;
  CompositorPlatform* platform = nullptr;
  IpcService* ipc = nullptr;
  ScreenRecorderService* screenRecorder = nullptr;
  TailscaleService* tailscale = nullptr;
  LocalSendService* localSend = nullptr;
  PhoneConnectService* phone = nullptr;
  SyncthingService* syncthing = nullptr;
  FirewallService* firewall = nullptr;
  PrinterService* printers = nullptr;
  CastService* cast = nullptr;
  AiUsageService* aiUsage = nullptr;
  FileWatcher* fileWatcher = nullptr;
  HttpClient* httpClient = nullptr;
  ClipboardService* clipboard = nullptr;
};
