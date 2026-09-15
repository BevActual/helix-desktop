#pragma once

#include "config/config_types.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wallpaper::daily {

  inline constexpr std::string_view kBingHost = "https://www.bing.com";
  inline constexpr std::string_view kBingArchivePath = "/HPImageArchive.aspx";
  inline constexpr std::string_view kNasaApodUrl = "https://api.nasa.gov/planetary/apod";
  inline constexpr std::string_view kDefaultBingMarket = "en-US";
  inline constexpr std::string_view kDefaultNasaApiKey = "DEMO_KEY";
  inline constexpr int kBingArchiveCount = 8;
  inline constexpr int kNasaLookbackDays = 14;
  inline constexpr std::size_t kMaxGridItems = 8;

  struct Item {
    WallpaperDailyProvider provider = WallpaperDailyProvider::Bing;
    std::string id;
    std::string date; // YYYY-MM-DD
    std::string title;
    std::string copyright;
    std::string thumbUrl;
    std::string imageUrl;
    std::string uhdUrl;
  };

  struct ArchiveResponse {
    std::vector<Item> items;
    std::string error;
  };

  [[nodiscard]] std::string normalizeDate(std::string_view text);
  [[nodiscard]] std::string compactDate(std::string_view ymd);
  [[nodiscard]] std::string localTodayDate();
  [[nodiscard]] std::string addDaysToDate(std::string_view ymd, int days);
  [[nodiscard]] std::string resolveBingMarket(std::string_view configured);
  [[nodiscard]] std::string resolveNasaApiKey(std::string_view configured, std::string_view envValue);
  [[nodiscard]] std::string buildBingArchiveUrl(std::string_view market, int count = kBingArchiveCount);
  [[nodiscard]] std::string buildNasaApodUrl(std::string_view apiKey, std::string_view startDate, std::string_view endDate);
  [[nodiscard]] std::string absoluteBingUrl(std::string_view pathOrUrl);
  [[nodiscard]] std::string bingUhdUrl(std::string_view urlbase);
  [[nodiscard]] std::string bingThumbUrl(std::string_view urlbase);
  [[nodiscard]] std::string fileExtensionFromUrl(std::string_view url);
  [[nodiscard]] std::string slugFromTitle(std::string_view title);
  [[nodiscard]] std::string ohrNameFromUrlbase(std::string_view urlbase);
  [[nodiscard]] ArchiveResponse parseBingArchiveBody(std::string_view body);
  [[nodiscard]] ArchiveResponse parseNasaApodBody(std::string_view body);
  [[nodiscard]] std::vector<Item> limitGridItems(std::vector<Item> items);
  [[nodiscard]] const Item* itemById(const std::vector<Item>& items, std::string_view id);
  [[nodiscard]] const Item* itemForDate(const std::vector<Item>& items, std::string_view ymd);
  [[nodiscard]] std::vector<std::string> downloadUrls(const Item& item);
  [[nodiscard]] std::filesystem::path thumbCacheDir();
  [[nodiscard]] std::filesystem::path thumbCachePath(std::string_view id);
  [[nodiscard]] std::filesystem::path downloadPath(const std::filesystem::path& dir, const Item& item);
  [[nodiscard]] std::filesystem::path resolveDownloadDir(
      std::string_view configured, std::string_view wallpaperDirectory
  );

} // namespace wallpaper::daily
