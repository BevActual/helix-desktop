#include "shell/wallpaper/daily_wallpaper_service.h"

#include "config/config_service.h"
#include "core/log.h"
#include "net/http_client.h"
#include "shell/wallpaper/wallpaper.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <cstdlib>
#include <system_error>
#include <utility>

namespace {
  constexpr Logger kLog("daily-wp");
}

DailyWallpaperService::DailyWallpaperService(ConfigService& config, HttpClient& http, Wallpaper& wallpaper)
    : m_config(config), m_http(http), m_wallpaper(wallpaper) {
  m_wallpaper.setDailyWallpaperService(this);
}

DailyWallpaperService::~DailyWallpaperService() { m_wallpaper.setDailyWallpaperService(nullptr); }

void DailyWallpaperService::ensureTodayApplied() {
  if (!m_config.config().wallpaper.enabled || !m_config.config().wallpaper.automation.enabled) {
    return;
  }
  if (m_config.config().wallpaper.automation.source != WallpaperAutomationConfig::Source::Daily) {
    return;
  }
  if (m_inFlight) {
    return;
  }

  const std::string today = wallpaper::daily::localTodayDate();
  const WallpaperDailyProvider currentProvider = provider();
  if (m_attemptedDate == today && m_attemptedProvider == currentProvider) {
    return;
  }
  if (offline()) {
    kLog.info("skipping daily wallpaper; offline mode is on");
    return;
  }

  fetchToday();
}

void DailyWallpaperService::fetchToday() {
  const WallpaperDailyProvider currentProvider = provider();
  const std::string today = wallpaper::daily::localTodayDate();
  m_inFlight = true;
  const std::uint64_t generation = ++m_generation;

  HttpRequest req;
  req.followRedirects = true;
  req.headers.emplace_back("Accept: application/json");
  if (currentProvider == WallpaperDailyProvider::Nasa) {
    const std::string start = wallpaper::daily::addDaysToDate(today, -wallpaper::daily::kNasaLookbackDays);
    req.url = wallpaper::daily::buildNasaApodUrl(nasaApiKey(), start, today);
  } else {
    req.url = wallpaper::daily::buildBingArchiveUrl(bingMarket());
  }

  m_http.request(std::move(req), [this, generation, currentProvider, today](HttpResponse response) {
    if (generation != m_generation) {
      return;
    }
    if (!response.transportOk || response.status < 200 || response.status >= 300) {
      m_inFlight = false;
      m_attemptedDate = today;
      m_attemptedProvider = currentProvider;
      kLog.warn("daily wallpaper request failed");
      return;
    }

    auto parsed = currentProvider == WallpaperDailyProvider::Nasa
        ? wallpaper::daily::parseNasaApodBody(response.body)
        : wallpaper::daily::parseBingArchiveBody(response.body);
    if (!parsed.error.empty() || parsed.items.empty()) {
      m_inFlight = false;
      m_attemptedDate = today;
      m_attemptedProvider = currentProvider;
      kLog.warn("daily wallpaper archive was empty");
      return;
    }

    const auto* item = wallpaper::daily::itemForDate(parsed.items, today);
    if (item == nullptr) {
      m_inFlight = false;
      m_attemptedDate = today;
      m_attemptedProvider = currentProvider;
      kLog.warn("daily wallpaper had no still for today");
      return;
    }

    applyItem(*item);
  });
}

void DailyWallpaperService::applyItem(const wallpaper::daily::Item& item) {
  const auto destDir = downloadDirectory();
  std::error_code ec;
  std::filesystem::create_directories(destDir, ec);
  if (ec) {
    m_inFlight = false;
    m_attemptedDate = item.date;
    m_attemptedProvider = item.provider;
    kLog.warn("daily wallpaper download directory is unavailable");
    return;
  }

  const auto dest = wallpaper::daily::downloadPath(destDir, item);
  const std::string destText = dest.string();
  if (std::filesystem::is_regular_file(dest, ec)) {
    m_inFlight = false;
    m_attemptedDate = item.date;
    m_attemptedProvider = item.provider;
    if (FileUtils::normalizeWallpaperPath(m_config.getDefaultWallpaperPath())
        == FileUtils::normalizeWallpaperPath(destText)) {
      return;
    }
    if (!m_wallpaper.applyWallpaperImage(std::nullopt, destText)) {
      kLog.warn("failed to apply daily wallpaper");
      return;
    }
    kLog.info("applied daily wallpaper {}", destText);
    return;
  }

  auto urls = wallpaper::daily::downloadUrls(item);
  if (urls.empty()) {
    m_inFlight = false;
    m_attemptedDate = item.date;
    m_attemptedProvider = item.provider;
    kLog.warn("daily wallpaper had no image url");
    return;
  }

  const std::uint64_t generation = m_generation;
  m_http.download(std::move(urls), dest, [this, generation, destText, date = item.date, provider = item.provider](bool ok) {
    if (generation != m_generation) {
      return;
    }
    m_inFlight = false;
    m_attemptedDate = date;
    m_attemptedProvider = provider;
    if (!ok) {
      kLog.warn("daily wallpaper download failed");
      return;
    }
    if (!m_wallpaper.applyWallpaperImage(std::nullopt, destText)) {
      kLog.warn("failed to apply daily wallpaper");
      return;
    }
    kLog.info("applied daily wallpaper {}", destText);
  });
}

bool DailyWallpaperService::offline() const { return m_config.config().shell.offlineMode; }

WallpaperDailyProvider DailyWallpaperService::provider() const { return m_config.config().wallpaper.daily.provider; }

std::string DailyWallpaperService::nasaApiKey() const {
  const char* env = std::getenv("NASA_API_KEY");
  return wallpaper::daily::resolveNasaApiKey(m_config.config().wallpaper.daily.nasaApiKey, env == nullptr ? "" : env);
}

std::string DailyWallpaperService::bingMarket() const {
  return wallpaper::daily::resolveBingMarket(m_config.config().wallpaper.daily.bingMarket);
}

std::filesystem::path DailyWallpaperService::downloadDirectory() const {
  return wallpaper::daily::resolveDownloadDir(
      m_config.config().wallpaper.daily.downloadDir, m_config.config().wallpaper.directory
  );
}
