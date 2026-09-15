#pragma once

#include "config/config_types.h"
#include "shell/wallpaper/daily_wallpaper.h"

#include <cstdint>
#include <filesystem>
#include <string>

class ConfigService;
class HttpClient;
class Wallpaper;

class DailyWallpaperService {
public:
  DailyWallpaperService(ConfigService& config, HttpClient& http, Wallpaper& wallpaper);
  ~DailyWallpaperService();

  DailyWallpaperService(const DailyWallpaperService&) = delete;
  DailyWallpaperService& operator=(const DailyWallpaperService&) = delete;

  void ensureTodayApplied();

private:
  void fetchToday();
  void applyItem(const wallpaper::daily::Item& item);
  [[nodiscard]] bool offline() const;
  [[nodiscard]] WallpaperDailyProvider provider() const;
  [[nodiscard]] std::string nasaApiKey() const;
  [[nodiscard]] std::string bingMarket() const;
  [[nodiscard]] std::filesystem::path downloadDirectory() const;

  ConfigService& m_config;
  HttpClient& m_http;
  Wallpaper& m_wallpaper;
  std::string m_attemptedDate;
  WallpaperDailyProvider m_attemptedProvider = WallpaperDailyProvider::Bing;
  std::uint64_t m_generation = 0;
  bool m_inFlight = false;
};
