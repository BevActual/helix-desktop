#include "theme/theme_service.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/scoped_timer.h"
#include "desktop_api/client.h"
#include "ipc/ipc_service.h"
#include "net/http_client.h"
#include "system/day_night_schedule.h"
#include "theme/builtin_palettes.h"
#include "theme/community_palettes.h"
#include "theme/custom_palettes.h"
#include "theme/fixed_palette.h"
#include "theme/image_loader.h"
#include "theme/palette_generator.h"
#include "theme/palette_transform.h"
#include "theme/runtime_palette.h"
#include "theme/scheme.h"
#include "util/checksum.h"
#include "util/string_utils.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace helix::theme {

  namespace {

    constexpr auto kLog = Logger("theme");

    constexpr float kTransitionDurationMs = 400.0F;
    constexpr std::chrono::milliseconds kTransitionTick{8};

    struct ResolvedTheme {
      GeneratedPalette generated;
      Palette palette;
      // Resolved [theme].mode (apps) and the mode Helix itself runs in.
      std::string mode;
      std::string shellMode;
    };

    ResolvedTheme resolveBuiltin(const ThemeConfig& cfg, std::string_view mode, std::string_view shellMode) {
      const auto* palette = findBuiltinPalette(cfg.builtinPalette);
      if (palette == nullptr) {
        kLog.warn("unknown builtin palette '{}', falling back to Helix", cfg.builtinPalette);
        palette = findBuiltinPalette("Helix");
      }
      const GeneratedPalette generated = expandBuiltinPalette(*palette);
      return {
          .generated = generated,
          .palette = mapGeneratedPaletteMode(shellMode == "light" ? generated.light : generated.dark),
          .mode = std::string(mode),
          .shellMode = std::string(shellMode),
      };
    }

    // Reads a color key from a JSON object, looking first for the `m`-prefixed form
    // (e.g. `mPrimary`) and falling back to the unprefixed name. Returns fallback
    // (transparent black) if the key is missing or the value is not a hex string.
    Color readColorField(const nlohmann::json& obj, std::string_view camelField) {
      std::string prefixed = "m";
      prefixed.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(camelField[0]))));
      prefixed.append(camelField.substr(1));
      auto tryRead = [&](const std::string& key) -> std::optional<Color> {
        auto it = obj.find(key);
        if (it == obj.end() || !it->is_string()) {
          return std::nullopt;
        }
        try {
          return hex(it->get<std::string>());
        } catch (const std::exception&) {
          return std::nullopt;
        }
      };
      if (auto c = tryRead(prefixed))
        return *c;
      if (auto c = tryRead(std::string(camelField)))
        return *c;
      return Color{};
    }

    Palette readPaletteJson(const nlohmann::json& obj) {
      return Palette{
          .primary = readColorField(obj, "primary"),
          .onPrimary = readColorField(obj, "onPrimary"),
          .secondary = readColorField(obj, "secondary"),
          .onSecondary = readColorField(obj, "onSecondary"),
          .tertiary = readColorField(obj, "tertiary"),
          .onTertiary = readColorField(obj, "onTertiary"),
          .error = readColorField(obj, "error"),
          .onError = readColorField(obj, "onError"),
          .surface = readColorField(obj, "surface"),
          .onSurface = readColorField(obj, "onSurface"),
          .surfaceVariant = readColorField(obj, "surfaceVariant"),
          .onSurfaceVariant = readColorField(obj, "onSurfaceVariant"),
          .outline = readColorField(obj, "outline"),
          .shadow = readColorField(obj, "shadow"),
          .hover = readColorField(obj, "hover"),
          .onHover = readColorField(obj, "onHover"),
      };
    }

    TerminalAnsiColors readAnsiJson(const nlohmann::json& obj) {
      return TerminalAnsiColors{
          .black = readColorField(obj, kTerminalBlackJsonKey),
          .red = readColorField(obj, kTerminalRedJsonKey),
          .green = readColorField(obj, kTerminalGreenJsonKey),
          .yellow = readColorField(obj, kTerminalYellowJsonKey),
          .blue = readColorField(obj, kTerminalBlueJsonKey),
          .magenta = readColorField(obj, kTerminalMagentaJsonKey),
          .cyan = readColorField(obj, kTerminalCyanJsonKey),
          .white = readColorField(obj, kTerminalWhiteJsonKey),
      };
    }

    TerminalPalette readTerminalJson(const nlohmann::json& obj) {
      TerminalPalette tp{};
      if (auto it = obj.find(kTerminalNormalJsonKey); it != obj.end() && it->is_object()) {
        tp.normal = readAnsiJson(*it);
      }
      if (auto it = obj.find(kTerminalBrightJsonKey); it != obj.end() && it->is_object()) {
        tp.bright = readAnsiJson(*it);
      }
      tp.foreground = readColorField(obj, kTerminalForegroundJsonKey);
      tp.background = readColorField(obj, kTerminalBackgroundJsonKey);
      tp.selectionFg = readColorField(obj, kTerminalSelectionFgJsonKey);
      tp.selectionBg = readColorField(obj, kTerminalSelectionBgJsonKey);
      tp.cursorText = readColorField(obj, kTerminalCursorTextJsonKey);
      tp.cursor = readColorField(obj, kTerminalCursorJsonKey);
      return tp;
    }

    std::optional<TerminalPalette> readModeTerminalJson(const nlohmann::json& obj) {
      auto it = obj.find(kTerminalJsonKey);
      if (it == obj.end() || !it->is_object())
        return std::nullopt;
      return readTerminalJson(*it);
    }

    struct ParsedCommunityPalette {
      FixedPaletteMode dark;
      FixedPaletteMode light;
    };

    std::optional<ParsedCommunityPalette> parseCommunityPaletteJson(const std::filesystem::path& path) {
      try {
        std::ifstream in(path);
        if (!in)
          return std::nullopt;
        std::stringstream buf;
        buf << in.rdbuf();
        auto root = nlohmann::json::parse(buf.str());
        if (!root.is_object())
          return std::nullopt;
        ParsedCommunityPalette out{};
        if (auto it = root.find("dark"); it != root.end() && it->is_object()) {
          out.dark.palette = readPaletteJson(*it);
          if (auto terminal = readModeTerminalJson(*it)) {
            out.dark.terminal = *terminal;
          } else {
            return std::nullopt;
          }
        } else {
          return std::nullopt;
        }
        if (auto it = root.find("light"); it != root.end() && it->is_object()) {
          out.light.palette = readPaletteJson(*it);
          if (auto terminal = readModeTerminalJson(*it)) {
            out.light.terminal = *terminal;
          } else {
            return std::nullopt;
          }
        } else {
          out.light = out.dark;
        }
        return out;
      } catch (const std::exception& e) {
        kLog.warn("failed to parse community palette '{}': {}", path.string(), e.what());
        return std::nullopt;
      }
    }

    ResolvedTheme
    makeResolvedFromParsed(const ParsedCommunityPalette& parsed, std::string_view mode, std::string_view shellMode) {
      BuiltinPalette bp{
          .name = "community",
          .dark = parsed.dark,
          .light = parsed.light,
      };
      const GeneratedPalette generated = expandBuiltinPalette(bp);
      return {
          .generated = generated,
          .palette = mapGeneratedPaletteMode(shellMode == "light" ? generated.light : generated.dark),
          .mode = std::string(mode),
          .shellMode = std::string(shellMode),
      };
    }

    std::optional<ResolvedTheme> resolveFromSource(
        const ThemeConfig& cfg, std::string_view mode, std::string_view shellMode,
        const std::optional<GeneratedPalette>& wallpaperGenerated
    ) {
      if (cfg.source == PaletteSource::Custom && !cfg.customPalette.empty()) {
        const auto path = customPalettePath(cfg.customPalette);
        if (std::filesystem::exists(path)) {
          if (auto parsed = parseCommunityPaletteJson(path)) {
            return makeResolvedFromParsed(*parsed, mode, shellMode);
          }
        }
        kLog.warn("custom palette '{}' not found or invalid; falling back to builtin", cfg.customPalette);
      } else if (cfg.source == PaletteSource::Wallpaper) {
        if (wallpaperGenerated.has_value()) {
          return ResolvedTheme{
              .generated = *wallpaperGenerated,
              .palette =
                  mapGeneratedPaletteMode(shellMode == "light" ? wallpaperGenerated->light : wallpaperGenerated->dark),
              .mode = std::string(mode),
              .shellMode = std::string(shellMode),
          };
        }
      } else if (cfg.source == PaletteSource::Community && !cfg.communityPalette.empty()) {
        const auto cachePath = communityPaletteCachePath(cfg.communityPalette);
        if (std::filesystem::exists(cachePath)) {
          if (auto parsed = parseCommunityPaletteJson(cachePath)) {
            return makeResolvedFromParsed(*parsed, mode, shellMode);
          }
          std::error_code rmEc;
          std::filesystem::remove(cachePath, rmEc);
        }
      } else if (cfg.source == PaletteSource::Builtin) {
        return resolveBuiltin(cfg, mode, shellMode);
      }
      return std::nullopt;
    }

    std::string colorSchemeValue(const ThemeConfig& theme) {
      switch (theme.source) {
      case PaletteSource::Builtin:
        return theme.builtinPalette;
      case PaletteSource::Wallpaper:
        return theme.wallpaperScheme;
      case PaletteSource::Community:
        return theme.communityPalette;
      case PaletteSource::Custom:
        return theme.customPalette;
      }
      return theme.builtinPalette;
    }

    std::string formatColorSchemeLine(const ThemeConfig& theme) {
      std::string out(enumToKey(kPaletteSources, theme.source));
      out.push_back(' ');
      out += colorSchemeValue(theme);
      out.push_back('\n');
      return out;
    }

    bool parseColorSchemeSetArgs(
        std::string_view args, PaletteSource& sourceOut, std::string& valueOut, std::string& errorOut
    ) {
      const auto tokens = StringUtils::splitWhitespace(args);
      if (tokens.size() < 2) {
        errorOut = "error: expected <builtin|wallpaper|community|custom> <name-or-scheme>\n";
        return false;
      }

      const auto source = enumFromKey(kPaletteSources, tokens.front());
      if (!source.has_value()) {
        errorOut = "error: unknown palette source (expected builtin, wallpaper, community, or custom)\n";
        return false;
      }

      std::string value;
      for (std::size_t i = 1; i < tokens.size(); ++i) {
        if (i > 1) {
          value.push_back(' ');
        }
        value += tokens[i];
      }
      value = StringUtils::trim(value);
      if (value.empty()) {
        errorOut = "error: palette or scheme name required\n";
        return false;
      }

      sourceOut = *source;
      valueOut = std::move(value);
      return true;
    }

  } // namespace

  ThemeService::ThemeService(ConfigService& config, HttpClient& httpClient)
      : m_config(config), m_httpClient(httpClient) {}

  void ThemeService::apply() { resolveAndSet(/*animate=*/false); }

  void ThemeService::onConfigReload() { resolveAndSet(/*animate=*/true); }

  void ThemeService::onWallpaperChange() {
    if (m_config.config().theme.source == PaletteSource::Wallpaper) {
      resolveAndSet(/*animate=*/true);
    }
  }

  void ThemeService::onAutoSchemeChanged() {
    if (hasAutoMode()) {
      resolveAndSet(/*animate=*/true);
    }
  }

  void ThemeService::setAutoCoordinates(std::optional<double> latitude, std::optional<double> longitude) {
    if (latitude.has_value() && !std::isfinite(*latitude)) {
      latitude.reset();
    }
    if (longitude.has_value() && !std::isfinite(*longitude)) {
      longitude.reset();
    }
    if (m_autoLatitude == latitude && m_autoLongitude == longitude) {
      return;
    }
    m_autoLatitude = latitude;
    m_autoLongitude = longitude;
    if (hasAutoMode()) {
      resolveAndSet(/*animate=*/true);
    }
  }

  void ThemeService::toggleLightDark() {
    const auto next = m_isLightMode ? ThemeMode::Dark : ThemeMode::Light;
    // Persist via ConfigService → StateService. The resulting overrides-change
    // callback rebuilds the Config and fires the reload callbacks, which call
    // ThemeService::onConfigReload() to transition to the new palette.
    m_config.setThemeMode(next);
  }

  void ThemeService::cycleMode() {
    ThemeMode next = ThemeMode::Dark;
    switch (configuredMode()) {
    case ThemeMode::Dark:
      next = ThemeMode::Light;
      break;
    case ThemeMode::Light:
      next = ThemeMode::Auto;
      break;
    case ThemeMode::Auto:
      next = ThemeMode::Dark;
      break;
    }
    m_config.setThemeMode(next);
  }

  ThemeMode ThemeService::configuredMode() const noexcept { return m_config.config().theme.mode; }

  bool ThemeService::isLightMode() const noexcept { return m_isShellLightMode; }

  std::string_view ThemeService::resolvedShellMode() const noexcept { return m_isShellLightMode ? "light" : "dark"; }

  std::string_view ThemeService::resolvedMode() const noexcept { return m_isLightMode ? "light" : "dark"; }

  ThemeService::AutoSchedule ThemeService::autoSchedule() const {
    const auto& loc = m_config.config().location;
    if (!day_night_schedule::isManualMode(loc)) {
      const auto coords = day_night_schedule::resolveCoordinates(loc, m_autoLatitude, m_autoLongitude);
      if (!coords.latitude.has_value() || !coords.longitude.has_value()) {
        return {};
      }
    }
    const auto eval = day_night_schedule::evaluate(loc, m_autoLatitude, m_autoLongitude);
    return AutoSchedule{
        .night = eval.night,
        .ready = true,
        .nextBoundary = std::chrono::system_clock::now() + eval.untilBoundary,
    };
  }

  void ThemeService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

  void ThemeService::setResolvedCallback(ResolvedCallback callback) { m_resolvedCallback = std::move(callback); }

  void ThemeService::queueResolvedCallback(const GeneratedPalette& generated, std::string_view mode) {
    if (!m_resolvedCallback) {
      return;
    }
    m_pendingResolvedPalette = generated;
    m_pendingResolvedMode = std::string(mode);
    ++m_resolvedCallbackGeneration;
  }

  void ThemeService::flushResolvedCallback(bool defer) {
    if (!m_resolvedCallback || !m_pendingResolvedPalette.has_value()) {
      return;
    }

    GeneratedPalette generated = std::move(*m_pendingResolvedPalette);
    std::string mode = std::move(m_pendingResolvedMode);
    const std::uint64_t generation = m_resolvedCallbackGeneration;
    m_pendingResolvedPalette.reset();
    m_pendingResolvedMode.clear();

    if (defer) {
      DeferredCall::callLater([this, generation, generated = std::move(generated), mode = std::move(mode)]() mutable {
        if (generation == m_resolvedCallbackGeneration && m_resolvedCallback) {
          m_resolvedCallback(generated, mode);
        }
      });
      return;
    }

    m_resolvedCallback(generated, mode);
  }

  void ThemeService::startCommunityDownload(const std::string& name) {
    if (m_inflightCommunityName == name) {
      return;
    }
    m_inflightCommunityName = name;
    const auto cachePath = communityPaletteCachePath(name);
    std::error_code ec;
    std::filesystem::create_directories(cachePath.parent_path(), ec);
    const std::string url = communityPaletteDownloadUrl(name);
    if (url.empty()) {
      kLog.warn(
          "community palette '{}' requested but HELIX_COMMUNITY_PALETTE_URL_BASE is unset; using local cache only", name
      );
      m_inflightCommunityName.clear();
      if (parseCommunityPaletteJson(cachePath).has_value()) {
        resolveAndSet(/*animate=*/true);
      }
      return;
    }
    kLog.info("fetching community palette '{}' from {}", name, url);
    m_httpClient.download(url, cachePath, [this, name, cachePath](bool success) {
      if (m_inflightCommunityName == name) {
        m_inflightCommunityName.clear();
      }
      if (!success) {
        kLog.warn("community palette download failed for '{}'", name);
        return;
      }
      // Validate the just-downloaded file. If it parses, trigger a re-resolve.
      if (!parseCommunityPaletteJson(cachePath).has_value()) {
        kLog.warn("community palette '{}' downloaded but failed to parse; removing cache", name);
        std::error_code rmEc;
        std::filesystem::remove(cachePath, rmEc);
        return;
      }
      resolveAndSet(/*animate=*/true);
    });
  }

  std::optional<GeneratedPalette>
  ThemeService::resolveWallpaperGenerated(const ThemeConfig& cfg, const std::string& wallpaperPath) {
    if (wallpaperPath.empty()) {
      kLog.warn("wallpaper theme requested but no wallpaper path set");
      return std::nullopt;
    }
    auto scheme = schemeFromString(cfg.wallpaperScheme);
    if (!scheme.has_value()) {
      kLog.warn("unknown wallpaper scheme '{}', falling back to m3-content", cfg.wallpaperScheme);
      scheme = Scheme::Content;
    }

    // mtime drives cache invalidation: an edited wallpaper at the same path
    // re-decodes. A failed stat (mtime 0) disables the cache rather than risk a
    // stale palette.
    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(wallpaperPath, ec);
    const std::int64_t mtimeNs = ec ? 0 : writeTime.time_since_epoch().count();

    if (mtimeNs != 0
        && m_wallpaperCacheGenerated.has_value()
        && m_wallpaperCachePath == wallpaperPath
        && m_wallpaperCacheScheme == cfg.wallpaperScheme
        && m_wallpaperCacheMtimeNs == mtimeNs) {
      return m_wallpaperCacheGenerated;
    }

    profiling::StopWatch loadWatch;
    auto image = loadAndResize(wallpaperPath, *scheme);
    if (profiling::enabled()) {
      kLog.info("theme: wallpaper load+resize: {:.1F} ms", loadWatch.elapsedMs());
    }
    if (!image) {
      kLog.warn("failed to load wallpaper '{}': {}", wallpaperPath, image.error());
      return std::nullopt;
    }
    profiling::StopWatch genWatch;
    auto generated = generate(image->rgb, *scheme);
    if (profiling::enabled()) {
      kLog.info("theme: wallpaper palette generate: {:.1F} ms", genWatch.elapsedMs());
    }
    if (!generated) {
      kLog.warn("failed to generate palette from wallpaper: {}", generated.error());
      return std::nullopt;
    }

    if (mtimeNs != 0) {
      m_wallpaperCacheGenerated = *generated;
      m_wallpaperCachePath = wallpaperPath;
      m_wallpaperCacheScheme = cfg.wallpaperScheme;
      m_wallpaperCacheMtimeNs = mtimeNs;
    }
    return *generated;
  }

  void ThemeService::resolveAndSet(bool animate) {
    if (m_remoteSource) {
      try {
        const auto snapshot = helix::desktopClient().call("appearance", "GetSnapshot");
        if (!snapshot.value("available", false))
          return;
        GeneratedPalette generated;
        generated.dark = snapshot.at("dark").get<decltype(generated.dark)>();
        generated.light = snapshot.at("light").get<decltype(generated.light)>();
        m_isLightMode = snapshot.value("mode", "dark") == "light";
        m_isShellLightMode = snapshot.value("shell-mode", "dark") == "light";
        setResolvedThemeLight(m_isShellLightMode);
        const auto target = mapGeneratedPaletteMode(m_isShellLightMode ? generated.light : generated.dark);
        queueResolvedCallback(generated, m_isLightMode ? "light" : "dark");
        if (animate)
          startTransition(target);
        else {
          setPalette(target);
          if (m_changeCallback)
            m_changeCallback();
          flushResolvedCallback(false);
        }
      } catch (...) {
      }
      return;
    }
    profiling::ScopedTimer t(kLog, "theme: resolveAndSet");
    const auto& cfg = m_config.config().theme;
    // [theme].mode is the app-facing mode; the shell either follows it or runs its own.
    // Both resolve `auto` against one schedule evaluation.
    std::optional<bool> scheduleNight;
    const auto modeName = [&](ThemeMode themeMode) -> std::string {
      if (themeMode != ThemeMode::Auto) {
        return themeMode == ThemeMode::Light ? "light" : "dark";
      }
      if (!scheduleNight.has_value()) {
        scheduleNight = day_night_schedule::evaluate(m_config.config().location, m_autoLatitude, m_autoLongitude).night;
      }
      return *scheduleNight ? "dark" : "light";
    };
    const std::string mode = modeName(cfg.mode);
    const std::string shellMode = modeName(shellThemeMode(cfg));

    std::optional<GeneratedPalette> wallpaperGenerated;
    if (cfg.source == PaletteSource::Wallpaper) {
      wallpaperGenerated = resolveWallpaperGenerated(cfg, m_config.getPaletteWallpaperPath());
    }
    std::optional<ResolvedTheme> resolved = resolveFromSource(cfg, mode, shellMode, wallpaperGenerated);
    if (cfg.source == PaletteSource::Community && !cfg.communityPalette.empty()) {
      const auto cachePath = communityPaletteCachePath(cfg.communityPalette);
      bool stale = true;
      if (std::filesystem::exists(cachePath)) {
        // Re-fetch when the catalog advertises a different checksum than the
        // cached copy. An empty catalog md5 means "freshness unknown": keep
        // the cached palette rather than re-downloading on every resolve.
        const std::string expectedMd5 = communityPaletteCatalogMd5(cfg.communityPalette);
        stale = !expectedMd5.empty() && util::fileMd5Hex(cachePath) != StringUtils::toLower(expectedMd5);
      }
      // A stale-but-valid cache still resolves above, so the fresh copy fades in
      // via the download callback instead of flashing the builtin palette.
      if (stale) {
        startCommunityDownload(cfg.communityPalette);
      }
    }
    if (!resolved.has_value()) {
      resolved = resolveBuiltin(cfg, mode, shellMode);
    }

    // Every source funnels through here, so the transform covers wallpaper-generated,
    // builtin, community and custom palettes alike. It runs after the wallpaper cache,
    // which keeps storing the untransformed palette, so toggling this cannot serve a stale one.
    if (cfg.pureBlackDark) {
      applyPureBlackDark(resolved->generated);
    }
    if (m_config.config().accessibility.highContrast) {
      applyHighContrast(resolved->generated);
    }

    if (cfg.pureBlackDark || m_config.config().accessibility.highContrast) {
      if (resolved->shellMode != "light") {
        resolved->palette = mapGeneratedPaletteMode(resolved->generated.dark);
      } else {
        resolved->palette = mapGeneratedPaletteMode(resolved->generated.light);
      }
    }

    queueResolvedCallback(resolved->generated, resolved->mode);
    m_isLightMode = resolved->mode == "light";
    m_isShellLightMode = resolved->shellMode == "light";

    if (animate) {
      setResolvedThemeLight(m_isShellLightMode);
      startTransition(resolved->palette);
    } else {
      if (palette == resolved->palette) {
        flushResolvedCallback(/*defer=*/false);
        rescheduleAutoTimer();
        return;
      }
      m_transitionTimer.stop();
      setResolvedThemeLight(m_isShellLightMode);
      setPalette(resolved->palette);
      if (m_changeCallback) {
        m_changeCallback();
      }
      flushResolvedCallback(/*defer=*/false);
    }
    rescheduleAutoTimer();
  }

  void ThemeService::rescheduleAutoTimer() {
    m_autoTimer.stop();
    if (!hasAutoMode()) {
      return;
    }
    constexpr auto kAutoRecheckInterval = std::chrono::minutes(1);
    const auto nextBoundary =
        day_night_schedule::evaluate(m_config.config().location, m_autoLatitude, m_autoLongitude).untilBoundary;
    const auto delay =
        std::min(nextBoundary, std::chrono::duration_cast<std::chrono::milliseconds>(kAutoRecheckInterval));
    m_autoTimer.start(delay, [this]() { onAutoSchemeChanged(); });
  }

  bool ThemeService::hasAutoMode() const noexcept {
    const auto& theme = m_config.config().theme;
    return theme.mode == ThemeMode::Auto || shellThemeMode(theme) == ThemeMode::Auto;
  }

  void ThemeService::startTransition(const Palette& target) {
    // Palette cross-fade lived in the display layer; platform ThemeService snaps.
    m_targetPalette = target;
    m_transitionTimer.stop();
    setPalette(target);
    if (m_changeCallback) {
      m_changeCallback();
    }
    flushResolvedCallback(/*defer=*/false);
  }

  void ThemeService::finishTransition(bool deferResolvedCallback) {
    m_transitionTimer.stop();
    setPalette(m_targetPalette);
    if (m_changeCallback) {
      m_changeCallback();
    }
    flushResolvedCallback(deferResolvedCallback);
  }

  void ThemeService::tickTransition() { m_transitionTimer.stop(); }

  bool ThemeService::saveWallpaperPaletteAsCustom(std::string* paletteNameOut, std::string* errorOut) {
    if (m_remoteSource) {
      try {
        const auto name = helix::desktopClient().call("appearance", "SavePalette", g_variant_new("(s)", "wallpaper"));
        if (paletteNameOut)
          *paletteNameOut = name.get<std::string>();
        return true;
      } catch (...) {
        if (errorOut)
          *errorOut = "Palette could not be saved";
        return false;
      }
    }

    auto fail = [errorOut](std::string message) {
      if (errorOut != nullptr) {
        *errorOut = std::move(message);
      }
      return false;
    };

    const auto& cfg = m_config.config().theme;
    if (cfg.source != PaletteSource::Wallpaper) {
      return fail("palette source is not wallpaper");
    }

    const std::string wallpaperPath = m_config.getPaletteWallpaperPath();
    const auto generated = resolveWallpaperGenerated(cfg, wallpaperPath);
    if (!generated) {
      return fail("failed to generate palette from wallpaper");
    }

    const std::string name = allocateCustomPaletteName(wallpaperPath, cfg.wallpaperScheme);
    if (name.empty()) {
      return fail("failed to allocate custom palette name");
    }

    if (!saveCustomPaletteFromGenerated(name, *generated, errorOut)) {
      return false;
    }

    if (!m_config.setThemeColorScheme(PaletteSource::Custom, name)) {
      return fail("failed to switch palette source to custom");
    }

    if (paletteNameOut != nullptr) {
      *paletteNameOut = name;
    }
    return true;
  }

  bool ThemeService::saveCurrentPaletteAsCustom(std::string* paletteNameOut, std::string* errorOut) {
    if (m_remoteSource) {
      try {
        const auto name = helix::desktopClient().call("appearance", "SavePalette", g_variant_new("(s)", "current"));
        if (paletteNameOut)
          *paletteNameOut = name.get<std::string>();
        return true;
      } catch (...) {
        if (errorOut)
          *errorOut = "Palette could not be saved";
        return false;
      }
    }

    auto fail = [errorOut](std::string message) {
      if (errorOut != nullptr) {
        *errorOut = std::move(message);
      }
      return false;
    };

    const auto& cfg = m_config.config().theme;
    if (cfg.source == PaletteSource::Wallpaper) {
      return saveWallpaperPaletteAsCustom(paletteNameOut, errorOut);
    }

    const std::string mode{resolvedMode()};
    const std::string shellMode{resolvedShellMode()};
    std::optional<ResolvedTheme> resolved = resolveFromSource(cfg, mode, shellMode, std::nullopt);
    if (!resolved.has_value()) {
      resolved = resolveBuiltin(cfg, mode, shellMode);
    }

    const std::string name = allocateCustomPaletteName("custom");
    if (name.empty()) {
      return fail("failed to allocate custom palette name");
    }

    if (!saveCustomPaletteFromGenerated(name, resolved->generated, errorOut)) {
      return false;
    }

    if (!m_config.setThemeColorScheme(PaletteSource::Custom, name)) {
      return fail("failed to switch palette source to custom");
    }

    if (paletteNameOut != nullptr) {
      *paletteNameOut = name;
    }
    return true;
  }

  bool ThemeService::saveRoleColorAsCustom(
      ColorRole role, const ::Color& color, std::string* paletteNameOut, std::string* errorOut
  ) {
    if (m_remoteSource) {
      try {
        const auto rgb = std::format(
            "#{:02x}{:02x}{:02x}", unsigned(std::clamp(color.r, 0.0F, 1.0F) * 255),
            unsigned(std::clamp(color.g, 0.0F, 1.0F) * 255), unsigned(std::clamp(color.b, 0.0F, 1.0F) * 255)
        );
        const auto name =
            helix::desktopClient().call("appearance", "SetRole", g_variant_new("(us)", unsigned(role), rgb.c_str()));
        if (paletteNameOut)
          *paletteNameOut = name.get<std::string>();
        return true;
      } catch (...) {
        if (errorOut)
          *errorOut = "Palette could not be saved";
        return false;
      }
    }

    auto fail = [errorOut](std::string message) {
      if (errorOut != nullptr) {
        *errorOut = std::move(message);
      }
      return false;
    };

    ::Color rgb = color;
    rgb.a = 1.0F;

    const auto& cfg = m_config.config().theme;
    const std::string mode{resolvedMode()};
    const std::string shellMode{resolvedShellMode()};
    std::optional<GeneratedPalette> wallpaperGenerated;
    if (cfg.source == PaletteSource::Wallpaper) {
      wallpaperGenerated = resolveWallpaperGenerated(cfg, m_config.getPaletteWallpaperPath());
    }
    std::optional<ResolvedTheme> resolved = resolveFromSource(cfg, mode, shellMode, wallpaperGenerated);
    if (!resolved.has_value()) {
      resolved = resolveBuiltin(cfg, mode, shellMode);
    }

    const bool light = resolved->shellMode == "light";
    ::Palette palette = mapGeneratedPaletteMode(light ? resolved->generated.light : resolved->generated.dark);
    if (!applyEditablePaletteRole(palette, role, rgb)) {
      return fail("unsupported palette role");
    }
    if (light) {
      resolved->generated.light = expandFixedPaletteMode(palette, false);
    } else {
      resolved->generated.dark = expandFixedPaletteMode(palette, true);
    }

    std::string name;
    if (cfg.source == PaletteSource::Custom && !cfg.customPalette.empty()) {
      name = cfg.customPalette;
    } else {
      name = allocateCustomPaletteName("custom");
    }
    if (name.empty()) {
      return fail("failed to allocate custom palette name");
    }

    if (!saveCustomPaletteFromGenerated(name, resolved->generated, errorOut)) {
      return false;
    }

    if (!m_config.setThemeColorScheme(PaletteSource::Custom, name)) {
      return fail("failed to switch palette source to custom");
    }

    if (paletteNameOut != nullptr) {
      *paletteNameOut = name;
    }
    return true;
  }

  void ThemeService::registerIpc(IpcService& ipc) {
    ipc.bind(helix::cli::msg::themeModeToggle, [this](const std::string&) -> std::string {
      toggleLightDark();
      return "ok\n";
    });
    ipc.bind(
        helix::cli::msg::themeModeGet,
        [this](const std::string&) -> std::string {
          std::string out(resolvedMode());
          out.push_back('\n');
          return out;
        },
        IpcService::HandlerOptions{.actionEditorVisibility = IpcService::ActionEditorVisibility::Hidden}
    );
    ipc.bind(helix::cli::msg::themeModeSet, [this](const std::string& args) -> std::string {
      const std::string token = StringUtils::trim(args);
      const auto mode = enumFromKey(kThemeModes, token);
      if (!mode.has_value()) {
        return "error: expected dark, light, or auto\n";
      }
      m_config.setThemeMode(*mode);
      return "ok\n";
    });
    ipc.bind(
        helix::cli::msg::colorSchemeGet,
        [this](const std::string&) -> std::string { return formatColorSchemeLine(m_config.config().theme); },
        IpcService::HandlerOptions{.actionEditorVisibility = IpcService::ActionEditorVisibility::Hidden}
    );
    ipc.bind(helix::cli::msg::colorSchemeSet, [this](const std::string& args) -> std::string {
      PaletteSource source = PaletteSource::Builtin;
      std::string value;
      std::string error;
      if (!parseColorSchemeSetArgs(args, source, value, error)) {
        return error;
      }
      if (!m_config.setThemeColorScheme(source, value)) {
        return "error: unknown scheme/palette or settings not writable\n";
      }
      return "ok\n";
    });
  }

} // namespace helix::theme
