#include "application.h"
#include "desktop/client.h"

#include "app/main_loop.h"
#include "ui/controls/select_dropdown_popup.h"
#include "compositors/compositor_detect.h"
#include "config/config_types.h"
#include "core/build_info.h"
#include "core/deferred_call.h"
#include "core/files/resource_paths.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/process/process.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dbus/accounts/accounts_service.h"
#include "dbus/bluetooth/bluetooth_agent.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/idle/screensaver_poll_source.h"
#include "dbus/idle/screensaver_service.h"
#include "dbus/logind/logind_service.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/iwd_secret_agent.h"
#include "dbus/network/network_manager_service.h"
#include "dbus/network/network_secret_agent.h"
#include "dbus/network/wpa_supplicant_service.h"
#include "dbus/notification/notification_dbus_host.h"
#include "dbus/notification/notification_service.h"
#include "dbus/polkit/polkit_agent.h"
#include "dbus/polkit/polkit_poll_source.h"
#include "dbus/polkit/polkit_session_support.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/session_bus.h"
#include "dbus/session_bus_poll_source.h"
#include "dbus/system_bus.h"
#include "dbus/system_bus_poll_source.h"
#include "dbus/tray/tray_service.h"
#include "dbus/upower/upower_service.h"
#include "debug/debug_service.h"
#include "i18n/i18n.h"
#include "i18n/i18n_service.h"
#include "ipc/ipc_arg_parse.h"
#include "launcher/app_provider.h"
#include "launcher/dmenu_provider.h"
#include "launcher/emoji_provider.h"
#include "launcher/math_provider.h"
#include "launcher/session_provider.h"
#include "launcher/wallpaper_provider.h"
#include "launcher/window_provider.h"
#include "notification/notifications.h"
#include "pipewire/pipewire_poll_source.h"
#include "pipewire/pipewire_service.h"
#include "pipewire/pipewire_spectrum.h"
#include "pipewire/pipewire_spectrum_poll_source.h"
#include "pipewire/sound_player.h"
#include "pipewire/wireplumber_mixer.h"
#include "render/animation/motion_service.h"
#include "render/backend/render_backend.h"
#include "render/core/texture_manager.h"
#include "render/text/font_weight_catalog.h"
#include "shell/greeter/greeter_appearance_sync.h"
#include "shell/launcher/launcher_panel.h"
#include "shell/polkit/polkit_panel.h"
#include "shell/session/session_ipc.h"
#include "shell/session/session_panel.h"
#include "shell/setup_wizard/setup_wizard_panel.h"
#include "shell/test/test_panel.h"
#include "shell/tooltip/tooltip_manager.h"
#include "shell/tray/tray_drawer_panel.h"
#include "shell/wallpaper/panel/wallpaper_panel.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "system/brightness_poll_source.h"
#include "system/brightness_service.h"
#include "system/display_config_service.h"
#include "system/distro_info.h"
#include "system/keyboard_backlight_service.h"
#include "system/system_monitor_service.h"
#include "ui/app_icon_colorization.h"
#include "ui/controls/input.h"
#include "ui/dialogs/color_picker_dialog.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/glyph_picker_dialog.h"
#include "ui/style.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <malloc.h>
#ifdef NOCTALIA_USE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#ifdef NOCTALIA_USE_JEMALLOC
#define NOCTALIA_STRINGIFY_HELPER(x) #x
#define NOCTALIA_STRINGIFY(x) NOCTALIA_STRINGIFY_HELPER(x)
#endif

std::atomic<bool> Application::s_shutdownRequested{false};

namespace {
  constexpr Logger kLog("app");

  float elapsedSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
  }

  template <typename Fn> void runStartupPhase(std::string_view label, Fn&& fn) {
    constexpr float kSlowStartupPhaseDebugMs = 50.0F;
    constexpr float kSlowStartupPhaseWarnMs = 1000.0F;

    const auto start = std::chrono::steady_clock::now();
    try {
      fn();
    } catch (...) {
      kLog.warn("startup phase {} failed after {:.1F}ms", label, elapsedSince(start));
      throw;
    }

    const float ms = elapsedSince(start);
    if (ms >= kSlowStartupPhaseWarnMs) {
      kLog.warn("startup phase {} took {:.1F}ms", label, ms);
    } else if (ms >= kSlowStartupPhaseDebugMs) {
      kLog.debug("startup phase {} took {:.1F}ms", label, ms);
    }
  }
} // namespace

Application::Application()
    : m_lockKeysService(m_wayland), m_gammaService(m_wayland) {
  m_notificationManager.loadPersistedHistory();
  notify::setInstance(&m_notificationManager);

  m_notificationManager.addEventCallback([this](const Notification& n, NotificationEvent event) {
    const char* kind = "updated";
    if (event == NotificationEvent::Added) {
      kind = "added";
    } else if (event == NotificationEvent::Closed) {
      kind = "closed";
    }
    const char* origin = (n.origin == NotificationOrigin::Internal) ? "internal" : "external";
    kLog.debug("notification {} id={} origin={}", kind, n.id, origin);

    if (event == NotificationEvent::Added && m_panelManager.isActivePanelContext("notifications")) {
      m_notificationManager.markNotificationHistorySeen();
    }

    // Keep bar widgets in sync with notification state changes.
    scheduleNotificationShellRefresh();
  });

  m_notificationManager.setStateCallback([this]() {
    if (m_notificationManager.doNotDisturb()) {
      m_notificationToast.hideDndSuppressed();
    } else {
      m_notificationToast.replayDeferredForDnd();
    }
    scheduleNotificationShellRefresh();
  });
}

Application::~Application() {
  m_desktopServiceTimer.stop();
  helix::desktopClient().unobserve(m_desktopServiceObserver);
  ColorPickerDialog::setPresenter(nullptr);
  GlyphPickerDialog::setPresenter(nullptr);
  FileDialog::setPresenter(nullptr);
  TooltipManager::instance().shutdown();
  m_notificationManager.flushPersistedHistory();
  m_wayland.setClipboardService(nullptr);
  m_wayland.setTextInputService(nullptr);
  m_wayland.setVirtualKeyboardService(nullptr);
  notify::setInstance(nullptr);
}

void Application::run(std::function<void()> startupReadyCallback) {
  initLogFile();
  initLogLevelFromEnvironment();
  kLog.info("Helix Shell {}", noctalia::build_info::displayVersion());
  runStartupPhase("initServices", [this]() { initServices(); });
  runStartupPhase("initUi", [this]() { initUi(); });
  runStartupPhase("initIpc", [this]() { initIpc(); });
  runStartupPhase("buildPollSources", [this]() { (void)buildPollSources(); });

  runStartupPhase("startup hooks", [this]() {
    m_hookManager.reload(m_configService.config().hooks);
    m_hookManager.fire(HookKind::Started);
  });

#ifdef __GLIBC__
  runStartupPhase("allocator_trim", []() {
#ifdef NOCTALIA_USE_JEMALLOC
    // jemalloc exports no malloc_trim; purge unused pages in every arena.
    const int purgeResult =
        mallctl("arena." NOCTALIA_STRINGIFY(MALLCTL_ARENAS_ALL) ".purge", nullptr, nullptr, nullptr, 0);
    if (purgeResult != 0) {
      kLog.warn("failed to purge jemalloc arenas: {}", std::strerror(purgeResult));
    }
#else
    malloc_trim(0);
#endif
  });
#endif

  m_trayInitTimer.start(std::chrono::milliseconds(500), [this]() { startTrayService(); });
  m_polkitInitTimer.start(std::chrono::milliseconds(1000), [this]() { syncPolkitAgent(); });

  m_mainLoop = std::make_unique<MainLoop>(m_wayland, m_bar, [this]() { return currentPollSources(); });
  if (startupReadyCallback) {
    startupReadyCallback();
  }
  m_mainLoop->run();
  kLog.info("shutdown");
}
