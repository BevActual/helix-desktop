#include "shell/wallpaper/daily_wallpaper.h"

#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace wallpaper::daily {
  namespace {

    [[nodiscard]] std::string jsonString(const nlohmann::json& obj, std::string_view key) {
      const auto it = obj.find(key);
      if (it == obj.end() || !it->is_string()) {
        return {};
      }
      return it->get<std::string>();
    }

    [[nodiscard]] bool isDigits(std::string_view text) {
      if (text.empty()) {
        return false;
      }
      for (char ch : text) {
        if (ch < '0' || ch > '9') {
          return false;
        }
      }
      return true;
    }

    [[nodiscard]] std::optional<std::chrono::year_month_day> parseYmd(std::string_view text) {
      const std::string normalized = normalizeDate(text);
      if (normalized.size() != 10 || normalized[4] != '-' || normalized[7] != '-') {
        return std::nullopt;
      }
      const auto yearPart = normalized.substr(0, 4);
      const auto monthPart = normalized.substr(5, 2);
      const auto dayPart = normalized.substr(8, 2);
      if (!isDigits(yearPart) || !isDigits(monthPart) || !isDigits(dayPart)) {
        return std::nullopt;
      }
      const int year = std::stoi(yearPart);
      const unsigned month = static_cast<unsigned>(std::stoi(monthPart));
      const unsigned day = static_cast<unsigned>(std::stoi(dayPart));
      const std::chrono::year_month_day ymd{
          std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}
      };
      if (!ymd.ok()) {
        return std::nullopt;
      }
      return ymd;
    }

    [[nodiscard]] std::string formatYmd(const std::chrono::year_month_day& ymd) {
      const int year = static_cast<int>(ymd.year());
      const unsigned month = static_cast<unsigned>(ymd.month());
      const unsigned day = static_cast<unsigned>(ymd.day());
      std::string out;
      out.reserve(10);
      out += std::to_string(year);
      out += '-';
      if (month < 10) {
        out += '0';
      }
      out += std::to_string(month);
      out += '-';
      if (day < 10) {
        out += '0';
      }
      out += std::to_string(day);
      return out;
    }

    [[nodiscard]] std::string stripQuery(std::string_view url) {
      const auto pos = url.find_first_of("?#");
      if (pos == std::string_view::npos) {
        return std::string(url);
      }
      return std::string(url.substr(0, pos));
    }

    void sortNewestFirst(std::vector<Item>& items) {
      std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.date > b.date; });
    }

  } // namespace

  std::string normalizeDate(std::string_view text) {
    const std::string trimmed = StringUtils::trim(text);
    if (trimmed.size() == 8 && isDigits(trimmed)) {
      return trimmed.substr(0, 4) + "-" + trimmed.substr(4, 2) + "-" + trimmed.substr(6, 2);
    }
    if (trimmed.size() == 10 && trimmed[4] == '-' && trimmed[7] == '-') {
      return trimmed;
    }
    return {};
  }

  std::string compactDate(std::string_view ymd) {
    const std::string normalized = normalizeDate(ymd);
    if (normalized.size() != 10) {
      return {};
    }
    std::string out;
    out.reserve(8);
    out += normalized.substr(0, 4);
    out += normalized.substr(5, 2);
    out += normalized.substr(8, 2);
    return out;
  }

  std::string localTodayDate() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const std::chrono::year_month_day ymd{
        std::chrono::year{local.tm_year + 1900},
        std::chrono::month{static_cast<unsigned>(local.tm_mon + 1)},
        std::chrono::day{static_cast<unsigned>(local.tm_mday)},
    };
    return formatYmd(ymd);
  }

  std::string addDaysToDate(std::string_view ymd, int days) {
    const auto parsed = parseYmd(ymd);
    if (!parsed.has_value()) {
      return {};
    }
    const std::chrono::sys_days shifted = std::chrono::sys_days{*parsed} + std::chrono::days{days};
    return formatYmd(std::chrono::year_month_day{shifted});
  }

  std::string resolveBingMarket(std::string_view configured) {
    const std::string trimmed = StringUtils::trim(configured);
    return trimmed.empty() ? std::string(kDefaultBingMarket) : trimmed;
  }

  std::string resolveNasaApiKey(std::string_view configured, std::string_view envValue) {
    const std::string fromConfig = StringUtils::trim(configured);
    if (!fromConfig.empty()) {
      return fromConfig;
    }
    const std::string fromEnv = StringUtils::trim(envValue);
    if (!fromEnv.empty()) {
      return fromEnv;
    }
    return std::string(kDefaultNasaApiKey);
  }

  std::string buildBingArchiveUrl(std::string_view market, int count) {
    const int n = std::clamp(count, 1, kBingArchiveCount);
    std::string url;
    url += kBingHost;
    url += kBingArchivePath;
    url += "?format=js&idx=0&n=";
    url += std::to_string(n);
    url += "&mkt=";
    url += StringUtils::urlEncode(resolveBingMarket(market));
    return url;
  }

  std::string buildNasaApodUrl(std::string_view apiKey, std::string_view startDate, std::string_view endDate) {
    std::string url;
    url += kNasaApodUrl;
    url += "?api_key=";
    url += StringUtils::urlEncode(std::string(apiKey));
    url += "&start_date=";
    url += StringUtils::urlEncode(std::string(startDate));
    url += "&end_date=";
    url += StringUtils::urlEncode(std::string(endDate));
    return url;
  }

  std::string absoluteBingUrl(std::string_view pathOrUrl) {
    const std::string trimmed = StringUtils::trim(pathOrUrl);
    if (trimmed.empty()) {
      return {};
    }
    if (trimmed.starts_with("https://") || trimmed.starts_with("http://")) {
      return trimmed;
    }
    if (trimmed.starts_with("//")) {
      return "https:" + trimmed;
    }
    if (trimmed.front() == '/') {
      return std::string(kBingHost) + trimmed;
    }
    return std::string(kBingHost) + "/" + trimmed;
  }

  std::string bingUhdUrl(std::string_view urlbase) {
    const std::string base = absoluteBingUrl(urlbase);
    if (base.empty()) {
      return {};
    }
    return base + "_UHD.jpg";
  }

  std::string bingThumbUrl(std::string_view urlbase) {
    const std::string base = absoluteBingUrl(urlbase);
    if (base.empty()) {
      return {};
    }
    return base + "_640x360.jpg";
  }

  std::string fileExtensionFromUrl(std::string_view url) {
    const std::string path = stripQuery(url);
    const auto slash = path.find_last_of('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) {
      return ".jpg";
    }
    std::string ext = name.substr(dot);
    for (char& ch : ext) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (ext == ".jpeg") {
      return ".jpg";
    }
    if (ext == ".jpg" || ext == ".png" || ext == ".webp") {
      return ext;
    }
    return ".jpg";
  }

  std::string slugFromTitle(std::string_view title) {
    std::string slug;
    slug.reserve(title.size());
    bool dash = false;
    for (char ch : title) {
      const unsigned char uch = static_cast<unsigned char>(ch);
      if (std::isalnum(uch)) {
        slug.push_back(static_cast<char>(std::tolower(uch)));
        dash = false;
        continue;
      }
      if (!slug.empty() && !dash) {
        slug.push_back('-');
        dash = true;
      }
    }
    while (!slug.empty() && slug.back() == '-') {
      slug.pop_back();
    }
    constexpr std::size_t kMaxSlug = 40;
    if (slug.size() > kMaxSlug) {
      slug.resize(kMaxSlug);
      while (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
      }
    }
    return slug;
  }

  std::string ohrNameFromUrlbase(std::string_view urlbase) {
    const std::string text = std::string(urlbase);
    const auto pos = text.find("OHR.");
    if (pos == std::string::npos) {
      return {};
    }
    std::string name = text.substr(pos + 4);
    const auto cut = name.find_first_of("_&?");
    if (cut != std::string::npos) {
      name.resize(cut);
    }
    return slugFromTitle(name);
  }

  ArchiveResponse parseBingArchiveBody(std::string_view body) {
    ArchiveResponse out;
    nlohmann::json decoded;
    try {
      decoded = nlohmann::json::parse(body.begin(), body.end());
    } catch (const nlohmann::json::exception&) {
      out.error = "parse";
      return out;
    }
    if (!decoded.is_object()) {
      out.error = "parse";
      return out;
    }
    const auto imagesIt = decoded.find("images");
    if (imagesIt == decoded.end() || !imagesIt->is_array()) {
      out.error = "error";
      return out;
    }
    for (const auto& entry : *imagesIt) {
      if (!entry.is_object()) {
        continue;
      }
      Item item;
      item.provider = WallpaperDailyProvider::Bing;
      item.date = normalizeDate(jsonString(entry, "startdate"));
      item.title = jsonString(entry, "title");
      item.copyright = jsonString(entry, "copyright");
      const std::string urlbase = jsonString(entry, "urlbase");
      item.imageUrl = absoluteBingUrl(jsonString(entry, "url"));
      item.uhdUrl = bingUhdUrl(urlbase);
      item.thumbUrl = bingThumbUrl(urlbase);
      if (item.thumbUrl.empty()) {
        item.thumbUrl = item.imageUrl;
      }
      if (item.date.empty() || item.imageUrl.empty()) {
        continue;
      }
      item.id = "bing:" + item.date;
      if (item.title.empty()) {
        item.title = ohrNameFromUrlbase(urlbase);
      }
      out.items.push_back(std::move(item));
    }
    sortNewestFirst(out.items);
    return out;
  }

  ArchiveResponse parseNasaApodBody(std::string_view body) {
    ArchiveResponse out;
    nlohmann::json decoded;
    try {
      decoded = nlohmann::json::parse(body.begin(), body.end());
    } catch (const nlohmann::json::exception&) {
      out.error = "parse";
      return out;
    }
    std::vector<nlohmann::json> rows;
    if (decoded.is_array()) {
      rows.reserve(decoded.size());
      for (const auto& entry : decoded) {
        rows.push_back(entry);
      }
    } else if (decoded.is_object()) {
      rows.push_back(std::move(decoded));
    } else {
      out.error = "parse";
      return out;
    }

    for (const auto& entry : rows) {
      if (!entry.is_object()) {
        continue;
      }
      if (jsonString(entry, "media_type") != "image") {
        continue;
      }
      Item item;
      item.provider = WallpaperDailyProvider::Nasa;
      item.date = normalizeDate(jsonString(entry, "date"));
      item.title = jsonString(entry, "title");
      item.copyright = jsonString(entry, "copyright");
      const std::string hdurl = jsonString(entry, "hdurl");
      const std::string url = jsonString(entry, "url");
      item.imageUrl = hdurl.empty() ? url : hdurl;
      item.uhdUrl = hdurl;
      item.thumbUrl = url.empty() ? item.imageUrl : url;
      if (item.date.empty() || item.imageUrl.empty()) {
        continue;
      }
      item.id = "nasa:" + item.date;
      out.items.push_back(std::move(item));
    }
    sortNewestFirst(out.items);
    return out;
  }

  std::vector<Item> limitGridItems(std::vector<Item> items) {
    if (items.size() > kMaxGridItems) {
      items.resize(kMaxGridItems);
    }
    return items;
  }

  const Item* itemById(const std::vector<Item>& items, std::string_view id) {
    for (const auto& item : items) {
      if (item.id == id) {
        return &item;
      }
    }
    return nullptr;
  }

  const Item* itemForDate(const std::vector<Item>& items, std::string_view ymd) {
    const std::string date = normalizeDate(ymd);
    if (date.empty()) {
      return items.empty() ? nullptr : &items.front();
    }
    for (const auto& item : items) {
      if (item.date == date) {
        return &item;
      }
    }
    return items.empty() ? nullptr : &items.front();
  }

  std::vector<std::string> downloadUrls(const Item& item) {
    std::vector<std::string> urls;
    if (!item.uhdUrl.empty() && item.uhdUrl != item.imageUrl) {
      urls.push_back(item.uhdUrl);
    }
    if (!item.imageUrl.empty()) {
      urls.push_back(item.imageUrl);
    }
    return urls;
  }

  std::filesystem::path thumbCacheDir() {
    const std::string state = FileUtils::stateDir();
    if (state.empty()) {
      return {};
    }
    return std::filesystem::path(state) / "daily-wallpaper" / "thumbs";
  }

  std::filesystem::path thumbCachePath(std::string_view id) {
    if (id.empty()) {
      return {};
    }
    std::string name = slugFromTitle(id);
    if (name.empty()) {
      name = "item";
    }
    return thumbCacheDir() / (name + ".jpg");
  }

  std::filesystem::path downloadPath(const std::filesystem::path& dir, const Item& item) {
    const std::string prefix = item.provider == WallpaperDailyProvider::Nasa ? "nasa" : "bing";
    std::string slug = slugFromTitle(item.title);
    if (slug.empty()) {
      slug = ohrNameFromUrlbase(item.uhdUrl);
    }
    if (slug.empty()) {
      slug = "wallpaper";
    }
    const std::string ext = fileExtensionFromUrl(item.imageUrl.empty() ? item.uhdUrl : item.imageUrl);
    std::string name = prefix + "-" + compactDate(item.date) + "-" + slug + ext;
    return dir / name;
  }

  std::filesystem::path resolveDownloadDir(std::string_view configured, std::string_view wallpaperDirectory) {
    if (!StringUtils::trim(configured).empty()) {
      return FileUtils::expandUserPath(std::string(configured));
    }
    if (!StringUtils::trim(wallpaperDirectory).empty()) {
      return FileUtils::expandUserPath(std::string(wallpaperDirectory));
    }
    return FileUtils::defaultPicturesDirectory();
  }

} // namespace wallpaper::daily
