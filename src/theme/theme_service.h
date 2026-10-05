#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "theme/palette.h"
#include "ui/palette.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

class ConfigService;
class HttpClient;
class IpcService;

namespace noctalia::theme {

  class ThemeService {
  public:
    using ChangeCallback = std::function<void()>;
    // Carries the resolved [theme].mode: the app-facing mode templates and the GTK color
    // scheme run in. Shell-facing consumers read resolvedShellMode()/isLightMode().
    using ResolvedCallback = std::function<void(const GeneratedPalette&, std::string_view)>;

    ThemeService(ConfigService& config, HttpClient& httpClient);

    // Snaps the palette to the resolved theme (no fade). Used at startup.
    void apply();
    void setRemoteSource(bool remote) { m_remoteSource = remote; }

    // Resolves the target theme and cross-fades to it.
    void onConfigReload();
    void onWallpaperChange();
    void onAutoSchemeChanged();
    void setAutoCoordinates(std::optional<double> latitude, std::optional<double> longitude);
    void toggleLightDark();
    void cycleMode();
    [[nodiscard]] ThemeMode configuredMode() const noexcept;
    // Noctalia's own resolved mode ([theme].shell_mode, or [theme].mode when it follows).
    [[nodiscard]] bool isLightMode() const noexcept;
    [[nodiscard]] std::string_view resolvedShellMode() const noexcept;
    // The resolved [theme].mode, which drives apps.
    [[nodiscard]] std::string_view resolvedMode() const noexcept;

    // Schedule used to resolve Auto theme/shell mode. `ready` is false until
    // location (or a custom sunrise/sunset) is available.
    struct AutoSchedule {
      bool night = false;
      bool ready = false;
      std::chrono::system_clock::time_point nextBoundary{};
    };
    [[nodiscard]] AutoSchedule autoSchedule() const;

    void setChangeCallback(ChangeCallback callback);
    void setResolvedCallback(ResolvedCallback callback);

    void registerIpc(IpcService& ipc);

    // Writes the current wallpaper-generated palette to ~/.config/noctalia/palettes/
    // and switches palette source to custom.
    [[nodiscard]] bool saveWallpaperPaletteAsCustom(std::string* paletteNameOut, std::string* errorOut = nullptr);
    // Forks the active palette into a new custom file (wallpaper palettes keep a
    // wallpaper-based name) and switches palette source to custom.
    [[nodiscard]] bool saveCurrentPaletteAsCustom(std::string* paletteNameOut, std::string* errorOut = nullptr);
    // Forks the active palette into a custom file (or overwrites the current custom
    // palette), writes `role` for the shell's current mode, and switches source to custom.
    [[nodiscard]] bool saveRoleColorAsCustom(
        ColorRole role, const ::Color& color, std::string* paletteNameOut, std::string* errorOut = nullptr
    );

  private:
    bool m_remoteSource = false;
    void resolveAndSet(bool animate);
    // Decodes + generates the wallpaper palette, memoized on (path, mtime, scheme)
    // so repeated resolves for an unchanged wallpaper skip the ~100ms image decode.
    std::optional<GeneratedPalette> resolveWallpaperGenerated(const ThemeConfig& cfg, const std::string& wallpaperPath);
    void queueResolvedCallback(const GeneratedPalette& generated, std::string_view mode);
    void flushResolvedCallback(bool defer);
    void startTransition(const Palette& target);
    void finishTransition(bool deferResolvedCallback);
    void tickTransition();
    void startCommunityDownload(const std::string& name);
    void rescheduleAutoTimer();
    [[nodiscard]] bool hasAutoMode() const noexcept;

    ConfigService& m_config;
    HttpClient& m_httpClient;
    std::string m_inflightCommunityName;

    // Memoized wallpaper palette (see resolveWallpaperGenerated). Keyed on the
    // wallpaper path, its mtime, and the active scheme; any mismatch re-decodes.
    std::optional<GeneratedPalette> m_wallpaperCacheGenerated;
    std::string m_wallpaperCachePath;
    std::string m_wallpaperCacheScheme;
    std::int64_t m_wallpaperCacheMtimeNs = 0;

    ChangeCallback m_changeCallback;
    ResolvedCallback m_resolvedCallback;
    // External template/hooks callbacks are delayed until the shell palette is
    // applied, and deferred callbacks must be able to drop stale resolves.
    std::optional<GeneratedPalette> m_pendingResolvedPalette;
    std::string m_pendingResolvedMode;
    std::uint64_t m_resolvedCallbackGeneration = 0;

    AnimationManager m_animations;
    Timer m_transitionTimer;
    Palette m_fromPalette{};
    Palette m_targetPalette{};
    AnimationManager::Id m_transitionAnimId = 0;
    bool m_transitionResolvedCallbackFlushed = false;
    bool m_isShellLightMode = false;
    bool m_isLightMode = false;
    std::optional<double> m_autoLatitude;
    std::optional<double> m_autoLongitude;
    Timer m_autoTimer;
  };

} // namespace noctalia::theme
