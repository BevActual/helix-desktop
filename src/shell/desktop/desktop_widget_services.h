#pragma once

class ClipboardService;
class ConfigService;
class FileWatcher;
class HttpClient;
class MprisService;
class PipeWireService;
class PipeWireSpectrum;
class RenderContext;
class SharedTextureCache;
class SystemMonitorService;
class WaylandConnection;

// Shared dependencies several desktop widgets read directly.
struct DesktopWidgetScriptDeps {
  FileWatcher* fileWatcher = nullptr;
  ClipboardService* clipboard = nullptr;
  ConfigService* configService = nullptr;
};

struct DesktopWidgetRuntimeServices {
  PipeWireService* pipewire = nullptr;
  PipeWireSpectrum* pipewireSpectrum = nullptr;
  MprisService* mpris = nullptr;
  HttpClient* httpClient = nullptr;
  SystemMonitorService* sysmon = nullptr;
  DesktopWidgetScriptDeps scriptDeps;
};

struct DesktopWidgetServices {
  WaylandConnection& wayland;
  ConfigService* config = nullptr;
  RenderContext* renderContext = nullptr;
  DesktopWidgetRuntimeServices runtime;
  SharedTextureCache* textureCache = nullptr;
};
