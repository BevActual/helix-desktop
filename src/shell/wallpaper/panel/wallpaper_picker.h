#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"
#include "render/core/color.h"
#include "render/core/thumbnail_service.h"
#include "shell/wallpaper/daily_wallpaper.h"
#include "shell/wallpaper/panel/wallpaper_scanner.h"
#include "net/wallhaven_client.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

class AnimationManager;
class Box;
class Button;
class ConfigService;
class Flex;
class Glyph;
class HttpClient;
class Image;
class Input;
class InputArea;
class Label;
class Renderer;
class Segmented;
class Select;
class Spinner;
class VideoWallpaperService;
class VirtualGridView;
class WallpaperGridAdapter;
class WaylandConnection;

namespace noctalia::theme {
  class ThemeService;
}

class WallpaperPicker {
public:
  enum class SortMode : std::uint8_t {
    NameAsc,
    NameDesc,
    DateAsc,
    DateDesc,
    Random,
  };

  enum class BrowseSource : std::uint8_t { Library, Wallhaven, Video, Daily };

  struct Options {
    bool showTitle = true;
    bool showCloseButton = true;
  };

  WallpaperPicker(
      WaylandConnection* wayland, ConfigService* config, ThumbnailService* thumbnails, WallpaperScanner* scanner,
      noctalia::theme::ThemeService* themeService, HttpClient* httpClient, Options options,
      VideoWallpaperService* videoWallpaper = nullptr
  );
  ~WallpaperPicker();

  WallpaperPicker(const WallpaperPicker&) = delete;
  WallpaperPicker& operator=(const WallpaperPicker&) = delete;

  void setContentScale(float scale) noexcept { m_contentScale = scale; }
  void setPanelCardOpacity(float opacity);
  void setAnimationManager(AnimationManager* mgr) noexcept { m_animations = mgr; }

  [[nodiscard]] std::unique_ptr<Flex> create();
  void onOpen(std::string_view context);
  void onClose();
  void layout(Renderer& renderer, float width, float height);
  void update(Renderer& renderer);
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit);
  [[nodiscard]] InputArea* initialFocusArea() const;
  [[nodiscard]] BrowseSource browseSource() const noexcept { return m_browseSource; }

private:
  [[nodiscard]] float contentScale() const noexcept { return m_contentScale; }
  [[nodiscard]] float panelCardOpacity() const noexcept { return m_panelCardOpacity; }
  [[nodiscard]] float scaled(float value) const noexcept { return value * m_contentScale; }

  struct MonitorChoice {
    std::string connector;
    std::string label;
  };

  void populateMonitorChoices();
  void refreshVisibleEntries();
  void refreshScan();
  void onScanComplete();
  void syncLoadingState();
  void applyFilter();
  void syncBrowseChrome();
  void syncBackButton();
  void navigateInto(const std::filesystem::path& dir);
  void navigateUp();
  void applyWallpaperFromEntry(const WallpaperEntry& entry);
  void applyWallpaperPath(const std::string& path, const WallpaperFavorite* applyTheme);
  [[nodiscard]] const WallpaperFavorite* favoriteThemeToApply(std::string_view path) const;
  [[nodiscard]] WallpaperFavorite themeFromControls() const;
  [[nodiscard]] WallpaperFavorite activeThemeSettings() const;
  void applyThemeFromControls();
  void toggleFavoriteForPath(const std::string& path);
  void syncThemeControls();
  void rebuildFavoritePaletteDetailSelect(const WallpaperFavorite* favorite);
  [[nodiscard]] std::optional<PaletteSource> paletteSourceForSegment(std::size_t index) const;
  [[nodiscard]] std::size_t segmentForPaletteSource(PaletteSource source) const;
  [[nodiscard]] std::string selectedWallpaperPath() const;
  void appendFilteredFavoriteEntries(
      std::vector<WallpaperEntry>& out, std::unordered_set<std::string>& favoritePaths,
      const std::filesystem::path& activeDir, const std::filesystem::path& rootDir
  ) const;
  void sortVisibleEntries();
  void reseedRandomSort();
  void syncSortButtonGlyph();
  void syncFlattenButton();
  void syncVideoMuteButton();
  void cycleLibrarySortOrder();
  void setLibrarySortField(std::size_t fieldIndex);
  void setSortMode(SortMode mode);
  [[nodiscard]] static SortMode sortModeFromState(std::string_view value);
  [[nodiscard]] static std::string_view sortModeStateValue(SortMode mode);
  [[nodiscard]] static std::size_t librarySortFieldIndex(SortMode mode);
  [[nodiscard]] static bool librarySortDescending(SortMode mode);
  void applyColorWallpaper();
  void pickWallpaperFile();
  void rebindGrid(bool resetScroll = false);
  void resetSelection();
  [[nodiscard]] bool hasVisibleSelection() const;
  void selectVisibleIndex(std::size_t index);
  void activateSelectedEntry();
  [[nodiscard]] bool handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers);
  [[nodiscard]] std::filesystem::path activeDirectoryForSelection() const;
  [[nodiscard]] std::filesystem::path rootDirectoryForSelection() const;
  [[nodiscard]] std::string currentWallpaperPathForSelection() const;
  [[nodiscard]] std::vector<std::string> allMonitorConnectors() const;
  [[nodiscard]] std::optional<Color> selectedFillColor() const;
  [[nodiscard]] static std::string displayNameForWallpaperPath(std::string_view path);

  void setBrowseSource(BrowseSource source, bool fetchIfNeeded);
  void syncSourceChrome();
  void syncCurrentStrip();
  void layoutCurrentThumb(Renderer& renderer);
  void bindCurrentThumbnail(Renderer& renderer);
  void releaseCurrentThumbnail();
  void toggleLockscreenWallpaper();
  [[nodiscard]] bool currentIsFavorite() const;
  [[nodiscard]] bool lockscreenUsesCurrent() const;
  [[nodiscard]] std::string themeFavoritePath() const;
  void refreshVideoEntries();
  void applyVideoPath(const std::string& path);
  void clearVideoWallpaper();
  void syncVideoStatus();
  void syncLibraryStatus();
  [[nodiscard]] bool videoPlayingOnSelection() const;
  [[nodiscard]] bool mpvpaperBinaryAvailable() const;
  [[nodiscard]] std::filesystem::path videoDirectory() const;
  [[nodiscard]] static std::size_t browseSourceIndex(BrowseSource source);
  [[nodiscard]] static BrowseSource browseSourceFromIndex(std::size_t index);
  [[nodiscard]] static std::string_view browseSourceStateValue(BrowseSource source);
  [[nodiscard]] static BrowseSource browseSourceFromState(std::string_view value);
  void fetchWallhaven(bool resetPage);
  void commitWallhavenSearch();
  void syncWallhavenSearchSort();
  void applyMonitorResolutionFilter();
  [[nodiscard]] std::optional<std::pair<int, int>> selectedMonitorPixelSize() const;
  void applyWallhavenItem(std::string_view id);
  void queueWallhavenThumbs();
  void pumpWallhavenThumbs();
  void bindWallhavenGrid();
  void syncWallhavenFilters();
  void syncWallhavenPager();
  void toggleWallhavenFlag(bool& flag, bool otherA, bool otherB);
  [[nodiscard]] bool wallhavenNsfwAllowed() const;
  [[nodiscard]] std::string wallhavenApiKey() const;
  [[nodiscard]] std::filesystem::path wallhavenDownloadDirectory() const;
  [[nodiscard]] const wallpaper::wallhaven::Item* wallhavenItemById(std::string_view id) const;

  void fetchDaily(bool force);
  void applyDailyItem(std::string_view id);
  void queueDailyThumbs();
  void pumpDailyThumbs();
  void bindDailyGrid();
  void syncDailyChrome();
  void persistDailyProvider(WallpaperDailyProvider provider);
  [[nodiscard]] WallpaperDailyProvider dailyProvider() const;
  [[nodiscard]] std::string dailyNasaApiKey() const;
  [[nodiscard]] std::string dailyBingMarket() const;
  [[nodiscard]] std::filesystem::path dailyDownloadDirectory() const;
  [[nodiscard]] const wallpaper::daily::Item* dailyItemById(std::string_view id) const;
  [[nodiscard]] const wallpaper::daily::Item* dailyItemByThumbPath(const std::filesystem::path& path) const;

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  ThumbnailService* m_thumbnails = nullptr;
  WallpaperScanner* m_scanner = nullptr;
  noctalia::theme::ThemeService* m_themeService = nullptr;
  HttpClient* m_http = nullptr;
  VideoWallpaperService* m_videoWallpaper = nullptr;
  AnimationManager* m_animations = nullptr;
  float m_contentScale = 1.0F;
  float m_panelCardOpacity = 1.0F;
  bool m_showTitle = true;
  bool m_showCloseButton = true;

  Flex* m_rootLayout = nullptr;
  Flex* m_toolbar = nullptr;
  Flex* m_currentStrip = nullptr;
  Box* m_currentThumbHost = nullptr;
  Image* m_currentThumb = nullptr;
  Glyph* m_currentThumbPlaceholder = nullptr;
  Label* m_currentCaption = nullptr;
  Label* m_currentName = nullptr;
  Label* m_folderName = nullptr;
  Button* m_lockscreenButton = nullptr;
  Flex* m_favoritesOptionsColumn = nullptr;
  Flex* m_favoritePaletteDetailRow = nullptr;
  Label* m_favoritePaletteDetailTitle = nullptr;
  Label* m_title = nullptr;
  Segmented* m_sourceSegmented = nullptr;
  Button* m_backButton = nullptr;
  Select* m_favoriteThemeMode = nullptr;
  Select* m_favoritePaletteSource = nullptr;
  Select* m_favoritePaletteDetailSelect = nullptr;
  Select* m_monitorSelect = nullptr;
  Input* m_filterInput = nullptr;
  Button* m_favoriteCurrentButton = nullptr;
  Button* m_tuneButton = nullptr;
  Flex* m_extrasRow = nullptr;
  Flex* m_toolsRow = nullptr;
  Button* m_flattenButton = nullptr;
  Button* m_sortButton = nullptr;
  Button* m_refreshButton = nullptr;
  Button* m_fileButton = nullptr;
  Button* m_colorButton = nullptr;
  Button* m_videoMute = nullptr;
  Button* m_videoClearButton = nullptr;
  Button* m_closeButton = nullptr;
  Select* m_librarySortSelect = nullptr;
  Select* m_dailyProvider = nullptr;
  Flex* m_wallhavenFilters = nullptr;
  Button* m_wallhavenCatGeneral = nullptr;
  Button* m_wallhavenCatAnime = nullptr;
  Button* m_wallhavenCatPeople = nullptr;
  Button* m_wallhavenPuritySfw = nullptr;
  Button* m_wallhavenPuritySketchy = nullptr;
  Button* m_wallhavenPurityNsfw = nullptr;
  Select* m_wallhavenSortSelect = nullptr;
  Button* m_wallhavenSortOrder = nullptr;
  Select* m_wallhavenAspectSelect = nullptr;
  Select* m_wallhavenResolutionMode = nullptr;
  Input* m_wallhavenResWidth = nullptr;
  Input* m_wallhavenResHeight = nullptr;
  Button* m_wallhavenClearRes = nullptr;
  Button* m_wallhavenResFromMonitor = nullptr;
  Button* m_wallhavenPrev = nullptr;
  Button* m_wallhavenNext = nullptr;
  Label* m_wallhavenPageLabel = nullptr;
  VirtualGridView* m_grid = nullptr;
  Flex* m_loadingBox = nullptr;
  Spinner* m_spinner = nullptr;
  Flex* m_videoStatusBox = nullptr;
  Label* m_videoStatusLabel = nullptr;
  std::unique_ptr<WallpaperGridAdapter> m_adapter;

  std::vector<MonitorChoice> m_monitorChoices;
  std::size_t m_selectedMonitorIndex = 0;
  std::vector<std::filesystem::path> m_navStack;
  std::vector<WallpaperEntry> m_visibleEntries;
  std::string m_filterQuery;
  std::string m_pendingFilterQuery;
  Timer m_filterDebounceTimer;
  bool m_flatten = false;
  bool m_extrasOpen = false;
  bool m_scanPending = false;
  SortMode m_sortMode = SortMode::NameAsc;
  std::uint64_t m_randomSeed = 0;
  std::size_t m_pinnedFavoriteCount = 0;
  bool m_syncingFavoriteControls = false;
  bool m_syncingGridSelectionVisual = false;
  bool m_gridKeyboardActive = false;
  std::vector<std::string> m_favoritePaletteDetailValues;
  std::vector<PaletteSource> m_paletteSourceOrder;
  static constexpr std::size_t kNoVisibleSelection = std::numeric_limits<std::size_t>::max();
  std::size_t m_selectedVisibleIndex = kNoVisibleSelection;
  BrowseSource m_browseSource = BrowseSource::Library;
  wallpaper::wallhaven::SearchQuery m_wallhavenQuery;
  std::vector<wallpaper::wallhaven::Item> m_wallhavenItems;
  int m_wallhavenTotalPages = 1;
  std::uint64_t m_wallhavenSearchGeneration = 0;
  std::uint64_t m_wallhavenApplyGeneration = 0;
  bool m_wallhavenLoading = false;
  bool m_wallhavenApplying = false;
  std::string m_wallhavenError;
  std::string m_wallhavenSelectedId;
  int m_wallhavenThumbActive = 0;
  std::vector<std::string> m_wallhavenThumbQueue;
  bool m_wallhavenSortManual = false;
  std::vector<wallpaper::daily::Item> m_dailyItems;
  std::uint64_t m_dailySearchGeneration = 0;
  std::uint64_t m_dailyApplyGeneration = 0;
  bool m_dailyLoading = false;
  bool m_dailyApplying = false;
  std::string m_dailyError;
  std::string m_dailySelectedId;
  int m_dailyThumbActive = 0;
  std::vector<std::string> m_dailyThumbQueue;
  float m_lastWidth = 0.0F;
  float m_lastHeight = 0.0F;
  bool m_dirty = false;
  bool m_thumbnailRefreshPending = false;
  ThumbnailService::Subscription m_thumbnailPendingSub;
  std::string m_currentThumbPath;
  int m_currentThumbTargetPx = 0;
};
