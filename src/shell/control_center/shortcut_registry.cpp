#include "shell/control_center/shortcut_registry.h"

#include "capture/screen_recorder_service.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_display.h"
#include "dbus/power/power_profiles_service.h"
#include "i18n/i18n.h"
#include "idle/idle_inhibitor.h"
#include "ipc/ipc_service.h"
#include "net/localsend_service.h"
#include "net/syncthing_service.h"
#include "net/tailscale_service.h"
#include "dbus/kdeconnect/phone_connect_service.h"
#include "dbus/firewall/firewall_service.h"
#include "dbus/cups/printer_service.h"
#include "dbus/cast/cast_service.h"
#include "notification/notification_manager.h"
#include "pipewire/pipewire_service.h"
#include "core/process/process.h"
#include "shell/control_center/shortcut_services.h"
#include "shell/keyboard_layout_label.h"
#include "shell/panel/panel_manager.h"
#include "system/ai_usage.h"
#include "system/ai_usage_service.h"
#include "system/gamma_service.h"
#include "system/weather_service.h"
#include "theme/theme_service.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <format>
#include <optional>
#include <ranges>
#include <vector>

namespace {
  void openTab(std::string_view tab) {
    PanelManager::instance().togglePanel("control-center", PanelOpenRequest{.context = tab});
  }

  // ── Toggle shortcuts ────────────────────────────────────────────────────────

  class WifiShortcut final : public Shortcut {
  public:
    explicit WifiShortcut(INetworkService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "wifi"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.wifi"); }
    std::string displayLabel() const override {
      if (m_svc != nullptr) {
        const NetworkState& state = m_svc->state();
        if (state.kind == NetworkConnectivity::Wireless && state.connected && !state.ssid.empty()) {
          return state.ssid;
        }
      }
      return defaultLabel();
    }
    std::string_view iconOn() const override { return "wifi"; }
    std::string_view iconOff() const override { return "wifi-off"; }
    std::string displayIcon() const override {
      if (m_svc == nullptr) {
        return "wifi-question";
      }
      return network_display::wifiGlyphForState(m_svc->state());
    }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && m_svc->state().wirelessEnabled; }
    void onClick() override {
      if (m_svc != nullptr) {
        m_svc->setWirelessEnabled(!m_svc->state().wirelessEnabled);
      }
    }
    void onRightClick() override { openTab("network"); }

  private:
    INetworkService* m_svc;
  };

  class BluetoothShortcut final : public Shortcut {
  public:
    explicit BluetoothShortcut(BluetoothService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "bluetooth"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.bluetooth"); }
    std::string_view iconOn() const override { return "bluetooth"; }
    std::string_view iconOff() const override { return "bluetooth-off"; }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && m_svc->state().powered; }
    void onClick() override {
      if (m_svc != nullptr) {
        m_svc->setPowered(!m_svc->state().powered);
      }
    }
    void onRightClick() override { openTab("bluetooth"); }

  private:
    BluetoothService* m_svc;
  };

  class NightlightShortcut final : public Shortcut {
  public:
    NightlightShortcut(GammaService* svc, CompositorPlatform* platform) : m_svc(svc), m_platform(platform) {}
    std::string_view id() const override { return "nightlight"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.nightlight"); }
    bool enabled() const override { return m_platform != nullptr && m_platform->hasGammaControl(); }
    std::string displayLabel() const override {
      if (m_svc == nullptr) {
        return defaultLabel();
      }
      if (m_svc->forceEnabled()) {
        return i18n::tr("control-center.shortcuts.nightlight-states.forced");
      }
      if (!m_svc->enabled()) {
        return i18n::tr("control-center.shortcuts.nightlight-states.off");
      }
      // Scheduled and currently warming the screen.
      if (m_svc->active()) {
        return i18n::tr("control-center.shortcuts.nightlight-states.scheduled-night");
      }
      // Scheduled but in the day phase: surface that the click "took" even
      // though the service is in day phase.
      return i18n::tr("control-center.shortcuts.nightlight-states.scheduled-day");
    }
    std::string_view iconOn() const override {
      return m_svc != nullptr && m_svc->forceEnabled() ? "nightlight-forced" : "nightlight-on";
    }
    std::string_view iconOff() const override { return "nightlight-off"; }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && (m_svc->forceEnabled() || m_svc->active()); }
    void onClick() override {
      if (!enabled() || m_svc == nullptr) {
        return;
      }
      m_svc->toggleEnabled();
    }
    void onRightClick() override {
      if (enabled() && m_svc != nullptr) {
        m_svc->toggleForceEnabled();
      }
    }

  private:
    GammaService* m_svc;
    CompositorPlatform* m_platform;
  };

  class NotificationShortcut final : public Shortcut {
  public:
    explicit NotificationShortcut(NotificationManager* svc) : m_svc(svc) {}
    std::string_view id() const override { return "notification"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.notification"); }
    std::string_view iconOn() const override { return "bell-off"; }
    std::string_view iconOff() const override { return "bell"; }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && m_svc->doNotDisturb(); }
    void onClick() override {
      if (m_svc != nullptr) {
        (void)m_svc->toggleDoNotDisturb();
      }
    }
    void onRightClick() override { openTab("notifications"); }

  private:
    NotificationManager* m_svc;
  };

  class DarkModeShortcut final : public Shortcut {
  public:
    explicit DarkModeShortcut(noctalia::theme::ThemeService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "dark_mode"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.dark-mode.dark"); }
    std::string displayLabel() const override {
      if (m_svc == nullptr) {
        return defaultLabel();
      }
      switch (m_svc->configuredMode()) {
      case ThemeMode::Dark:
        return i18n::tr("control-center.shortcuts.dark-mode.dark");
      case ThemeMode::Light:
        return i18n::tr("control-center.shortcuts.dark-mode.light");
      case ThemeMode::Auto:
        return i18n::tr("control-center.shortcuts.dark-mode.auto");
      }
      return defaultLabel();
    }
    std::string_view iconOn() const override { return "theme-mode"; }
    std::string_view iconOff() const override { return "theme-mode"; }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && m_svc->configuredMode() != ThemeMode::Light; }
    void onClick() override {
      if (m_svc != nullptr) {
        m_svc->cycleMode();
      }
    }

  private:
    noctalia::theme::ThemeService* m_svc;
  };

  class IdleInhibitorShortcut final : public Shortcut {
  public:
    explicit IdleInhibitorShortcut(IdleInhibitor* svc) : m_svc(svc) {}
    std::string_view id() const override { return "caffeine"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.caffeine"); }
    std::string_view iconOn() const override { return "caffeine-on"; }
    std::string_view iconOff() const override { return "caffeine-off"; }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && m_svc->enabled(); }
    void onClick() override {
      if (m_svc != nullptr) {
        m_svc->toggle();
      }
    }

  private:
    IdleInhibitor* m_svc;
  };

  class AudioShortcut final : public Shortcut {
  public:
    explicit AudioShortcut(PipeWireService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "audio"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.audio"); }
    std::string_view iconOn() const override { return "volume-x"; }
    std::string_view iconOff() const override { return "volume-high"; }
    bool isToggle() const override { return true; }
    bool active() const override {
      if (m_svc == nullptr) {
        return false;
      }
      const AudioNode* sink = m_svc->defaultSink();
      return sink != nullptr && sink->muted;
    }
    void onClick() override {
      if (m_svc != nullptr) {
        if (const AudioNode* sink = m_svc->defaultSink(); sink != nullptr) {
          m_svc->setMuted(!sink->muted);
        }
      }
    }
    void onRightClick() override { openTab("audio"); }

  private:
    PipeWireService* m_svc;
  };

  class MicMuteShortcut final : public Shortcut {
  public:
    explicit MicMuteShortcut(PipeWireService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "mic_mute"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.mic-mute"); }
    std::string_view iconOn() const override { return "microphone-mute"; }
    std::string_view iconOff() const override { return "microphone"; }
    bool isToggle() const override { return true; }
    bool active() const override {
      if (m_svc == nullptr) {
        return false;
      }
      const AudioNode* source = m_svc->defaultSource();
      return source != nullptr && source->muted;
    }
    void onClick() override {
      if (m_svc != nullptr) {
        if (const AudioNode* source = m_svc->defaultSource(); source != nullptr) {
          m_svc->setMicMuted(!source->muted);
        }
      }
    }
    void onRightClick() override { openTab("privacy"); }

  private:
    PipeWireService* m_svc;
  };

  class PowerProfileShortcut final : public Shortcut {
  public:
    explicit PowerProfileShortcut(PowerProfilesService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "power_profile"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.power-profile"); }
    std::string displayLabel() const override {
      if (m_svc != nullptr && !m_svc->activeProfile().empty()) {
        return profileLabel(m_svc->activeProfile());
      }
      return defaultLabel();
    }
    std::string_view iconOn() const override {
      return profileGlyphName(m_svc != nullptr ? m_svc->activeProfile() : "");
    }
    std::string_view iconOff() const override { return "balanced"; }
    bool isToggle() const override { return true; }
    bool active() const override {
      return m_svc != nullptr && !m_svc->activeProfile().empty() && m_svc->activeProfile() != "balanced";
    }
    void onClick() override { cycle(1); }
    void onRightClick() override { cycle(-1); }
    bool onScroll(int direction) override {
      cycle(direction);
      return true;
    }

  private:
    void cycle(int direction) {
      if (m_svc != nullptr) {
        (void)m_svc->cycleActiveProfile(direction);
      }
    }

    PowerProfilesService* m_svc;
  };

  class WeatherShortcut final : public Shortcut {
  public:
    explicit WeatherShortcut(WeatherService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "weather"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.weather"); }
    std::string displayLabel() const override {
      if (m_svc != nullptr && m_svc->enabled() && m_svc->hasData()) {
        const auto& snapshot = m_svc->snapshot();
        const int temp = static_cast<int>(std::lround(m_svc->displayTemperature(snapshot.current.temperatureC)));
        return std::format("{}{}", temp, m_svc->displayTemperatureUnit());
      }
      return defaultLabel();
    }
    std::string displayIcon() const override {
      if (m_svc == nullptr || !m_svc->enabled()) {
        return "weather-cloud-off";
      }
      if (m_svc->hasData()) {
        const auto& snapshot = m_svc->snapshot();
        return WeatherService::glyphForCode(snapshot.current.weatherCode, snapshot.current.isDay);
      }
      return "weather-cloud";
    }
    std::string_view iconOn() const override { return "weather-cloud-sun"; }
    std::string_view iconOff() const override { return "weather-cloud-sun"; }
    void onClick() override { openTab("weather"); }
    void onRightClick() override { openTab("weather"); }

  private:
    WeatherService* m_svc;
  };

  class KeyboardLayoutShortcut final : public Shortcut {
  public:
    KeyboardLayoutShortcut(CompositorPlatform* platform, ConfigService* config)
        : m_platform(platform), m_config(config) {}
    std::string_view id() const override { return "keyboard_layout"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.keyboard-layout"); }
    std::string displayLabel() const override {
      const std::string layoutName = resolvedLayoutName();
      if (m_config == nullptr) {
        return formatKeyboardLayoutLabel(layoutName, KeyboardLayoutDisplayMode::Short);
      }
      return resolveKeyboardLayoutLabel(
          layoutName, KeyboardLayoutDisplayMode::Short, m_config->config().shell.keyboardLayout.customLabels
      );
    }
    std::string_view iconOn() const override { return "keyboard"; }
    std::string_view iconOff() const override { return "keyboard"; }
    void onClick() override {
      if (m_platform != nullptr) {
        (void)m_platform->cycleKeyboardLayout();
      }
      PanelManager::instance().refresh();
    }

  private:
    [[nodiscard]] std::string resolvedLayoutName() const {
      const auto state = m_platform != nullptr ? m_platform->keyboardLayoutState() : std::nullopt;
      if (state.has_value()
          && state->currentIndex >= 0
          && state->currentIndex < static_cast<int>(state->names.size())) {
        return state->names[static_cast<std::size_t>(state->currentIndex)];
      }

      if (m_platform != nullptr) {
        return m_platform->currentKeyboardLayoutName();
      }

      return {};
    }

    CompositorPlatform* m_platform = nullptr;
    ConfigService* m_config = nullptr;
  };

  // ── Action-only shortcuts ────��──────────────────────────────────────────────

  class MediaShortcut final : public Shortcut {
  public:
    explicit MediaShortcut(MprisService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "media"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.media"); }
    std::string_view iconOn() const override { return "media-pause"; }
    std::string_view iconOff() const override { return "media-play"; }
    bool isToggle() const override { return true; }
    bool active() const override {
      if (m_svc == nullptr) {
        return false;
      }
      const auto active = m_svc->activePlayer();
      return active.has_value() && active->playbackStatus == "Playing";
    }
    void onClick() override {
      if (m_svc != nullptr) {
        (void)m_svc->playPauseActive();
      }
    }
    void onRightClick() override { openTab("media"); }

  private:
    MprisService* m_svc;
  };

  class SystemShortcut final : public Shortcut {
  public:
    std::string_view id() const override { return "system"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.system"); }
    std::string_view iconOn() const override { return "activity"; }
    std::string_view iconOff() const override { return "activity"; }
    void onClick() override { openTab("system"); }
    void onRightClick() override { openTab("system"); }
  };

  class ScreenTimeShortcut final : public Shortcut {
  public:
    std::string_view id() const override { return "screen_time"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.screen-time"); }
    std::string_view iconOn() const override { return "hourglass"; }
    std::string_view iconOff() const override { return "hourglass"; }
    void onClick() override { openTab("screen-time"); }
    void onRightClick() override { openTab("screen-time"); }
  };

  class AiShortcut final : public Shortcut {
  public:
    explicit AiShortcut(AiUsageService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "ai"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.ai"); }
    std::string displayLabel() const override {
      if (m_svc == nullptr) {
        return defaultLabel();
      }
      const auto& snapshot = m_svc->snapshot();
      if (noctalia::ai_usage::activeOllamaPull(snapshot) != nullptr) {
        return noctalia::ai_usage::snapshotGlanceLabel(snapshot);
      }
      if (const auto hit = noctalia::ai_usage::hottestQuota(snapshot)) {
        return std::format("{:.0f}%", static_cast<double>(hit->window.percent));
      }
      if (noctalia::ai_usage::glanceableOllama(snapshot)) {
        return noctalia::ai_usage::ollamaGlanceLabel(snapshot.ollama);
      }
      if (const auto* runtime = noctalia::ai_usage::glanceableLocalRuntime(snapshot)) {
        return noctalia::ai_usage::localRuntimeLabel(runtime->kind);
      }
      return defaultLabel();
    }
    std::string displayIcon() const override {
      if (m_svc == nullptr) {
        return "sparkles";
      }
      const auto& snapshot = m_svc->snapshot();
      if (noctalia::ai_usage::activeOllamaPull(snapshot) != nullptr) {
        return "server";
      }
      if (const auto hit = noctalia::ai_usage::hottestQuota(snapshot)) {
        return std::string(noctalia::ai_usage::providerGlyph(hit->provider));
      }
      if (noctalia::ai_usage::glanceableOllama(snapshot)) {
        return "server";
      }
      if (const auto* runtime = noctalia::ai_usage::glanceableLocalRuntime(snapshot)) {
        return std::string(noctalia::ai_usage::localRuntimeGlyph(runtime->kind));
      }
      return "sparkles";
    }
    std::string_view iconOn() const override { return "sparkles"; }
    std::string_view iconOff() const override { return "sparkles"; }
    bool active() const override {
      if (m_svc == nullptr) {
        return false;
      }
      const auto hit = noctalia::ai_usage::hottestQuota(m_svc->snapshot());
      if (!hit.has_value()) {
        return false;
      }
      return hit->window.percent + 0.5F >= static_cast<float>(m_svc->attentionPercent());
    }
    void onClick() override { openTab("ai"); }
    void onRightClick() override { openTab("ai"); }

  private:
    AiUsageService* m_svc = nullptr;
  };

  class WallpaperShortcut final : public Shortcut {
  public:
    std::string_view id() const override { return "wallpaper"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.wallpaper"); }
    std::string_view iconOn() const override { return "wallpaper-selector"; }
    std::string_view iconOff() const override { return "wallpaper-selector"; }
    void onClick() override { PanelManager::instance().togglePanel("wallpaper"); }
  };

  class SessionShortcut final : public Shortcut {
  public:
    std::string_view id() const override { return "session"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.session"); }
    std::string_view iconOn() const override { return "shutdown"; }
    std::string_view iconOff() const override { return "shutdown"; }
    void onClick() override { openTab("power"); }
  };

  class ClipboardShortcut final : public Shortcut {
  public:
    std::string_view id() const override { return "clipboard"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.clipboard"); }
    std::string_view iconOn() const override { return "clipboard"; }
    std::string_view iconOff() const override { return "clipboard"; }
    void onClick() override { openTab("clipboard"); }
    void onRightClick() override { openTab("clipboard"); }
  };

  void runCapture(IpcService* ipc, const char* command) {
    PanelManager::instance().closePanel();
    if (ipc != nullptr) {
      (void)ipc->execute(command);
    }
  }

  class ScreenshotShortcut final : public Shortcut {
  public:
    explicit ScreenshotShortcut(IpcService* ipc) : m_ipc(ipc) {}
    std::string_view id() const override { return "screenshot"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.screenshot"); }
    std::string_view iconOn() const override { return "crop"; }
    std::string_view iconOff() const override { return "crop"; }
    void onClick() override { runCapture(m_ipc, "screenshot-region"); }
    void onRightClick() override { openTab("screenshot"); }

  private:
    IpcService* m_ipc = nullptr;
  };

  class ColorPickShortcut final : public Shortcut {
  public:
    explicit ColorPickShortcut(IpcService* ipc) : m_ipc(ipc) {}
    std::string_view id() const override { return "pick_color"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.pick-color"); }
    std::string_view iconOn() const override { return "color-picker"; }
    std::string_view iconOff() const override { return "color-picker"; }
    void onClick() override { runCapture(m_ipc, "screenshot-pick-color"); }
    void onRightClick() override { openTab("screenshot"); }

  private:
    IpcService* m_ipc = nullptr;
  };

  class OcrShortcut final : public Shortcut {
  public:
    explicit OcrShortcut(IpcService* ipc) : m_ipc(ipc) {}
    std::string_view id() const override { return "ocr"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.ocr"); }
    std::string_view iconOn() const override { return "text-recognition"; }
    std::string_view iconOff() const override { return "text-recognition"; }
    bool enabled() const override { return process::commandExists("tesseract"); }
    void onClick() override { runCapture(m_ipc, "screenshot-ocr"); }
    void onRightClick() override { openTab("screenshot"); }

  private:
    IpcService* m_ipc = nullptr;
  };

  class QrScanShortcut final : public Shortcut {
  public:
    explicit QrScanShortcut(IpcService* ipc) : m_ipc(ipc) {}
    std::string_view id() const override { return "scan_qr"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.scan-qr"); }
    std::string_view iconOn() const override { return "qrcode"; }
    std::string_view iconOff() const override { return "qrcode"; }
    bool enabled() const override { return process::commandExists("zbarimg"); }
    void onClick() override { runCapture(m_ipc, "screenshot-scan-qr"); }
    void onRightClick() override { openTab("screenshot"); }

  private:
    IpcService* m_ipc = nullptr;
  };

  class ScreenRecordShortcut final : public Shortcut {
  public:
    explicit ScreenRecordShortcut(ScreenRecorderService* recorder) : m_recorder(recorder) {}
    std::string_view id() const override { return "screen_recorder"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.screen-recorder"); }
    std::string displayLabel() const override {
      if (m_recorder != nullptr && m_recorder->isRecording()) {
        return m_recorder->elapsedLabel();
      }
      return defaultLabel();
    }
    std::string_view iconOn() const override { return "player-stop"; }
    std::string_view iconOff() const override { return "video"; }
    bool isToggle() const override { return true; }
    bool enabled() const override {
      return m_recorder != nullptr
          && (m_recorder->isRecording() || (m_recorder->available() && !m_recorder->isReplaying()));
    }
    bool active() const override { return m_recorder != nullptr && m_recorder->isRecording(); }
    void onClick() override {
      PanelManager::instance().closePanel();
      if (m_recorder != nullptr) {
        (void)m_recorder->toggle(ScreenRecorderService::Source::Focused);
      }
    }
    void onRightClick() override { openTab("screen-record"); }

  private:
    ScreenRecorderService* m_recorder = nullptr;
  };

  class ScreenReplayShortcut final : public Shortcut {
  public:
    explicit ScreenReplayShortcut(ScreenRecorderService* recorder) : m_recorder(recorder) {}
    std::string_view id() const override { return "screen_replay"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.screen-replay"); }
    std::string displayLabel() const override {
      if (m_recorder != nullptr && m_recorder->isReplaying()) {
        return m_recorder->elapsedLabel();
      }
      return defaultLabel();
    }
    std::string_view iconOn() const override { return "player-stop"; }
    std::string_view iconOff() const override { return "history"; }
    bool isToggle() const override { return true; }
    bool enabled() const override {
      return m_recorder != nullptr
          && (m_recorder->isReplaying()
              || (m_recorder->hasGpuScreenRecorder() && !m_recorder->isRecording()));
    }
    bool active() const override { return m_recorder != nullptr && m_recorder->isReplaying(); }
    void onClick() override {
      PanelManager::instance().closePanel();
      if (m_recorder != nullptr) {
        (void)m_recorder->toggleReplay(ScreenRecorderService::Source::Focused);
      }
    }
    void onRightClick() override {
      if (m_recorder != nullptr && m_recorder->isReplaying() && !m_recorder->isReplayStopping()) {
        (void)m_recorder->saveReplay();
        return;
      }
      openTab("screen-record");
    }

  private:
    ScreenRecorderService* m_recorder = nullptr;
  };

  class TailscaleShortcut final : public Shortcut {
  public:
    explicit TailscaleShortcut(TailscaleService* service) : m_service(service) {}
    std::string_view id() const override { return "tailscale"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.tailscale"); }
    std::string_view iconOn() const override { return "circles"; }
    std::string_view iconOff() const override { return "circles"; }
    bool isToggle() const override { return true; }
    bool enabled() const override {
      return m_service != nullptr && (m_service->installed() || m_service->running());
    }
    bool active() const override { return m_service != nullptr && m_service->running(); }
    void onClick() override {
      if (m_service != nullptr) {
        m_service->toggle();
      }
    }
    void onRightClick() override { openTab("tailscale"); }

  private:
    TailscaleService* m_service = nullptr;
  };

  class LocalSendShortcut final : public Shortcut {
  public:
    LocalSendShortcut(LocalSendService* service, ConfigService* config) : m_service(service), m_config(config) {}
    std::string_view id() const override { return "localsend"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.localsend"); }
    std::string_view iconOn() const override { return "share"; }
    std::string_view iconOff() const override { return "share"; }
    bool isToggle() const override { return true; }
    bool enabled() const override { return m_service != nullptr; }
    bool active() const override { return m_service != nullptr && m_service->enabled(); }
    void onClick() override {
      if (m_service == nullptr) {
        return;
      }
      const bool next = !m_service->enabled();
      if (m_config != nullptr) {
        (void)m_config->setOverride({"localsend", "enabled"}, next);
      } else {
        m_service->setEnabled(next);
      }
    }
    void onRightClick() override { openTab("localsend"); }

  private:
    LocalSendService* m_service = nullptr;
    ConfigService* m_config = nullptr;
  };

  class PhoneShortcut final : public Shortcut {
  public:
    explicit PhoneShortcut(PhoneConnectService* service) : m_service(service) {}
    std::string_view id() const override { return "phone"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.phone"); }
    std::string_view iconOn() const override { return "device-mobile"; }
    std::string_view iconOff() const override { return "device-mobile"; }
    bool isToggle() const override { return false; }
    bool enabled() const override { return true; }
    bool active() const override {
      if (m_service == nullptr) {
        return false;
      }
      const auto& snap = m_service->snapshot();
      return std::ranges::any_of(snap.devices, [](const PhoneDevice& device) {
        return device.paired && device.reachable;
      });
    }
    void onClick() override { openTab("phone"); }
    void onRightClick() override { openTab("phone"); }

  private:
    PhoneConnectService* m_service = nullptr;
  };

  class SyncthingShortcut final : public Shortcut {
  public:
    explicit SyncthingShortcut(SyncthingService* service) : m_service(service) {}
    std::string_view id() const override { return "syncthing"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.syncthing"); }
    std::string_view iconOn() const override { return "folder"; }
    std::string_view iconOff() const override { return "folder"; }
    bool isToggle() const override { return false; }
    bool enabled() const override { return true; }
    bool active() const override { return m_service != nullptr && m_service->running(); }
    void onClick() override { openTab("syncthing"); }
    void onRightClick() override { openTab("syncthing"); }

  private:
    SyncthingService* m_service = nullptr;
  };

  class FirewallShortcut final : public Shortcut {
  public:
    explicit FirewallShortcut(FirewallService* service) : m_service(service) {}
    std::string_view id() const override { return "firewall"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.firewall"); }
    std::string_view iconOn() const override { return "shield-lock"; }
    std::string_view iconOff() const override { return "shield-lock"; }
    bool isToggle() const override { return false; }
    bool enabled() const override { return true; }
    bool active() const override { return m_service != nullptr && m_service->snapshot().panic; }
    void onClick() override { openTab("firewall"); }
    void onRightClick() override { openTab("firewall"); }

  private:
    FirewallService* m_service = nullptr;
  };

  class PrintersShortcut final : public Shortcut {
  public:
    explicit PrintersShortcut(PrinterService* service) : m_service(service) {}
    std::string_view id() const override { return "printers"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.printers"); }
    std::string_view iconOn() const override { return "printer"; }
    std::string_view iconOff() const override { return "printer"; }
    bool isToggle() const override { return false; }
    bool enabled() const override { return true; }
    bool active() const override {
      if (m_service == nullptr) {
        return false;
      }
      return std::ranges::any_of(m_service->snapshot().printers, [](const PrinterInfo& printer) {
        return printer.state == "printing" || printer.jobCount > 0;
      });
    }
    void onClick() override { openTab("printers"); }
    void onRightClick() override { openTab("printers"); }

  private:
    PrinterService* m_service = nullptr;
  };

  class CastShortcut final : public Shortcut {
  public:
    explicit CastShortcut(CastService* service) : m_service(service) {}
    std::string_view id() const override { return "cast"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.cast"); }
    std::string_view iconOn() const override { return "screen-share"; }
    std::string_view iconOff() const override { return "screen-share"; }
    bool isToggle() const override { return false; }
    bool enabled() const override { return true; }
    bool active() const override { return m_service != nullptr && m_service->sharing(); }
    void onClick() override { openTab("cast"); }
    void onRightClick() override { openTab("cast"); }

  private:
    CastService* m_service = nullptr;
  };

  class PrivacyShortcut final : public Shortcut {
  public:
    explicit PrivacyShortcut(PipeWireService* audio) : m_audio(audio) {}
    std::string_view id() const override { return "privacy"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.privacy"); }
    std::string_view iconOn() const override { return "eye-off"; }
    std::string_view iconOff() const override { return "eye-off"; }
    bool isToggle() const override { return false; }
    bool enabled() const override { return true; }
    bool active() const override { return m_audio != nullptr && !m_audio->privacyState().captures.empty(); }
    void onClick() override { openTab("privacy"); }
    void onRightClick() override { openTab("privacy"); }

  private:
    PipeWireService* m_audio = nullptr;
  };

  using ShortcutCreator = std::unique_ptr<Shortcut> (*)(const ShortcutServices& services);
  using ShortcutAvailability = bool (*)(const Config& config);

  struct BuiltinShortcutDescriptor {
    std::string_view type;
    std::string_view labelKey;
    ShortcutAvailability isAvailable = nullptr;
    ShortcutCreator create = nullptr;
  };

  template <typename T, auto... ServiceMembers>
  constexpr BuiltinShortcutDescriptor builtinShortcut(BuiltinShortcutDescriptor descriptor) {
    descriptor.create = [](const ShortcutServices& services) -> std::unique_ptr<Shortcut> {
      return std::make_unique<T>((services.*ServiceMembers)...);
    };
    return descriptor;
  }

  constexpr auto kBuiltinShortcuts = std::to_array<BuiltinShortcutDescriptor>({
      builtinShortcut<WifiShortcut, &ShortcutServices::network>({
          .type = "wifi",
          .labelKey = "control-center.shortcuts.wifi",
      }),
      builtinShortcut<BluetoothShortcut, &ShortcutServices::bluetooth>({
          .type = "bluetooth",
          .labelKey = "control-center.shortcuts.bluetooth",
      }),
      builtinShortcut<NightlightShortcut, &ShortcutServices::nightLight, &ShortcutServices::platform>({
          .type = "nightlight",
          .labelKey = "control-center.shortcuts.nightlight",
      }),
      builtinShortcut<NotificationShortcut, &ShortcutServices::notifications>({
          .type = "notification",
          .labelKey = "control-center.shortcuts.notification",
      }),
      builtinShortcut<DarkModeShortcut, &ShortcutServices::theme>({
          .type = "dark_mode",
          .labelKey = "control-center.shortcuts.dark-mode.dark",
      }),
      builtinShortcut<IdleInhibitorShortcut, &ShortcutServices::idleInhibitor>({
          .type = "caffeine",
          .labelKey = "control-center.shortcuts.caffeine",
      }),
      builtinShortcut<AudioShortcut, &ShortcutServices::audio>({
          .type = "audio",
          .labelKey = "control-center.shortcuts.audio",
      }),
      builtinShortcut<MicMuteShortcut, &ShortcutServices::audio>({
          .type = "mic_mute",
          .labelKey = "control-center.shortcuts.mic-mute",
      }),
      builtinShortcut<PowerProfileShortcut, &ShortcutServices::powerProfiles>({
          .type = "power_profile",
          .labelKey = "control-center.shortcuts.power-profile",
      }),
      builtinShortcut<MediaShortcut, &ShortcutServices::mpris>({
          .type = "media",
          .labelKey = "control-center.shortcuts.media",
      }),
      builtinShortcut<WeatherShortcut, &ShortcutServices::weather>({
          .type = "weather",
          .labelKey = "control-center.shortcuts.weather",
          .isAvailable = [](const Config& config) { return config.weather.enabled; },
      }),
      builtinShortcut<SystemShortcut>({
          .type = "system",
          .labelKey = "control-center.shortcuts.system",
          .isAvailable = [](const Config& config) { return config.system.monitor.enabled; },
      }),
      builtinShortcut<ScreenTimeShortcut>({
          .type = "screen_time",
          .labelKey = "control-center.shortcuts.screen-time",
          .isAvailable = [](const Config& config) { return config.shell.screenTimeEnabled; },
      }),
      builtinShortcut<AiShortcut, &ShortcutServices::aiUsage>({
          .type = "ai",
          .labelKey = "control-center.shortcuts.ai",
      }),
      builtinShortcut<KeyboardLayoutShortcut, &ShortcutServices::platform, &ShortcutServices::config>({
          .type = "keyboard_layout",
          .labelKey = "control-center.shortcuts.keyboard-layout",
      }),
      builtinShortcut<WallpaperShortcut>({
          .type = "wallpaper",
          .labelKey = "control-center.shortcuts.wallpaper",
      }),
      builtinShortcut<SessionShortcut>({
          .type = "session",
          .labelKey = "control-center.shortcuts.session",
      }),
      builtinShortcut<ClipboardShortcut>({
          .type = "clipboard",
          .labelKey = "control-center.shortcuts.clipboard",
          .isAvailable = [](const Config& config) { return config.shell.clipboardEnabled; },
      }),
      builtinShortcut<ScreenshotShortcut, &ShortcutServices::ipc>({
          .type = "screenshot",
          .labelKey = "control-center.shortcuts.screenshot",
      }),
      builtinShortcut<ColorPickShortcut, &ShortcutServices::ipc>({
          .type = "pick_color",
          .labelKey = "control-center.shortcuts.pick-color",
      }),
      builtinShortcut<OcrShortcut, &ShortcutServices::ipc>({
          .type = "ocr",
          .labelKey = "control-center.shortcuts.ocr",
      }),
      builtinShortcut<QrScanShortcut, &ShortcutServices::ipc>({
          .type = "scan_qr",
          .labelKey = "control-center.shortcuts.scan-qr",
      }),
      builtinShortcut<ScreenRecordShortcut, &ShortcutServices::screenRecorder>({
          .type = "screen_recorder",
          .labelKey = "control-center.shortcuts.screen-recorder",
      }),
      builtinShortcut<ScreenReplayShortcut, &ShortcutServices::screenRecorder>({
          .type = "screen_replay",
          .labelKey = "control-center.shortcuts.screen-replay",
      }),
      builtinShortcut<TailscaleShortcut, &ShortcutServices::tailscale>({
          .type = "tailscale",
          .labelKey = "control-center.shortcuts.tailscale",
      }),
      builtinShortcut<LocalSendShortcut, &ShortcutServices::localSend, &ShortcutServices::config>({
          .type = "localsend",
          .labelKey = "control-center.shortcuts.localsend",
      }),
      builtinShortcut<PhoneShortcut, &ShortcutServices::phone>({
          .type = "phone",
          .labelKey = "control-center.shortcuts.phone",
      }),
      builtinShortcut<SyncthingShortcut, &ShortcutServices::syncthing>({
          .type = "syncthing",
          .labelKey = "control-center.shortcuts.syncthing",
      }),
      builtinShortcut<FirewallShortcut, &ShortcutServices::firewall>({
          .type = "firewall",
          .labelKey = "control-center.shortcuts.firewall",
      }),
      builtinShortcut<PrintersShortcut, &ShortcutServices::printers>({
          .type = "printers",
          .labelKey = "control-center.shortcuts.printers",
      }),
      builtinShortcut<CastShortcut, &ShortcutServices::cast>({
          .type = "cast",
          .labelKey = "control-center.shortcuts.cast",
      }),
      builtinShortcut<PrivacyShortcut, &ShortcutServices::audio>({
          .type = "privacy",
          .labelKey = "control-center.shortcuts.privacy",
      }),
  });

  const BuiltinShortcutDescriptor* findBuiltinShortcut(std::string_view type) {
    for (const auto& shortcut : kBuiltinShortcuts) {
      if (shortcut.type == type) {
        return &shortcut;
      }
    }
    return nullptr;
  }

} // namespace

std::span<const ShortcutRegistry::CatalogEntry> ShortcutRegistry::catalog() {
  static const std::vector<CatalogEntry> combined = [] {
    auto result = kBuiltinShortcuts
        | std::views::transform([](const BuiltinShortcutDescriptor& shortcut) {
                    return CatalogEntry{.type = shortcut.type, .labelKey = shortcut.labelKey};
                  })
        | std::ranges::to<std::vector>();
    return result;
  }();
  return combined;
}

bool ShortcutRegistry::isAvailable(std::string_view type, const Config& config) {
  const auto* shortcut = findBuiltinShortcut(type);
  return shortcut == nullptr || shortcut->isAvailable == nullptr || shortcut->isAvailable(config);
}

std::unique_ptr<Shortcut> ShortcutRegistry::create(std::string_view type, const ShortcutServices& s) {
  const auto* shortcut = findBuiltinShortcut(type);
  if (shortcut == nullptr || (s.config != nullptr && !isAvailable(type, s.config->config()))) {
    return nullptr;
  }
  return shortcut->create(s);
}
