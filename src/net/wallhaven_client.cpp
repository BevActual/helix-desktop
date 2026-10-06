#include "net/wallhaven_client.h"

#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <ranges>
#include <system_error>

namespace wallpaper::wallhaven {
  namespace {

    constexpr std::string_view kAspectRatioValues[] = {
        "",     "wide",  "portrait", "16x9", "16x10", "21x9", "32x9", "48x9",
        "9x16", "10x16", "9x18",     "1x1",  "3x2",   "4x3",  "5x4",
    };

    [[nodiscard]] bool isWallhavenIdChar(char ch) {
      return std::isalnum(static_cast<unsigned char>(ch)) != 0;
    }

    [[nodiscard]] std::optional<std::string> wallpaperIdFromText(std::string_view text) {
      constexpr std::string_view kPath = "wallhaven.cc/w/";
      if (const auto pos = text.find(kPath); pos != std::string_view::npos) {
        const std::size_t start = pos + kPath.size();
        std::size_t end = start;
        while (end < text.size() && isWallhavenIdChar(text[end])) {
          ++end;
        }
        if (end - start >= 4) {
          return std::string(text.substr(start, end - start));
        }
      }

      constexpr std::string_view kFile = "wallhaven-";
      if (const auto pos = text.find(kFile); pos != std::string_view::npos) {
        const std::size_t start = pos + kFile.size();
        std::size_t end = start;
        while (end < text.size() && isWallhavenIdChar(text[end])) {
          ++end;
        }
        if (end - start >= 4) {
          return std::string(text.substr(start, end - start));
        }
      }
      return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> queryParamValue(std::string_view text, std::string_view key) {
      const std::string needleQ = "?" + std::string(key) + "=";
      const std::string needleA = "&" + std::string(key) + "=";
      std::size_t pos = text.find(needleQ);
      if (pos == std::string_view::npos) {
        pos = text.find(needleA);
      }
      if (pos == std::string_view::npos) {
        return std::nullopt;
      }
      const std::size_t start = pos + key.size() + 2;
      const std::size_t end = std::min(text.find_first_of("&#", start), text.size());
      const std::string formEncoded = StringUtils::replaceAll(text.substr(start, end - start), "+", " ");
      return StringUtils::urlDecode(formEncoded);
    }

    [[nodiscard]] std::string collapseSearchTokens(std::string_view text) {
      const std::string commasAsSpaces = StringUtils::replaceAll(text, ",", " ");
      const auto parts = StringUtils::splitWhitespace(commasAsSpaces);
      if (parts.empty()) {
        return {};
      }
      std::string out = parts.front();
      for (std::size_t i = 1; i < parts.size(); ++i) {
        out += ' ';
        out += parts[i];
      }
      return out;
    }

    [[nodiscard]] std::optional<int> parsePositiveInt(std::string_view text) {
      const std::string trimmed = StringUtils::trim(text);
      if (trimmed.empty()) {
        return std::nullopt;
      }
      int value = 0;
      for (char ch : trimmed) {
        if (ch < '0' || ch > '9') {
          return std::nullopt;
        }
        const int digit = ch - '0';
        if (value > (std::numeric_limits<int>::max() - digit) / 10) {
          return std::nullopt;
        }
        value = value * 10 + digit;
      }
      if (value < 1) {
        return std::nullopt;
      }
      return value;
    }

    [[nodiscard]] std::optional<std::pair<int, int>> parseResolution(std::string_view text) {
      const auto x = text.find('x');
      if (x == std::string_view::npos) {
        return std::nullopt;
      }
      const auto width = parsePositiveInt(text.substr(0, x));
      const auto height = parsePositiveInt(text.substr(x + 1));
      if (!width.has_value() || !height.has_value()) {
        return std::nullopt;
      }
      return std::pair{*width, *height};
    }

    [[nodiscard]] bool relevanceWithoutQuery(const SearchQuery& query) {
      return query.sort == Sort::Relevance && normalizeSearchQuery(query.tags).empty();
    }

    [[nodiscard]] std::string jsonString(const nlohmann::json& obj, std::string_view key) {
      const auto it = obj.find(key);
      if (it == obj.end() || !it->is_string()) {
        return {};
      }
      return it->get<std::string>();
    }

  } // namespace

  std::string normalizeSearchQuery(std::string_view raw) {
    std::string text = StringUtils::trim(raw);
    if (text.empty()) {
      return {};
    }

    const bool looksLikeLink = text.find("://") != std::string::npos || text.find("wallhaven.cc") != std::string::npos;
    if (looksLikeLink) {
      if (const auto id = wallpaperIdFromText(text)) {
        return "id:" + *id;
      }
      if (const auto query = queryParamValue(text, "q")) {
        text = StringUtils::trim(*query);
      }
    } else if (text.starts_with("wallhaven-") || text.find("/wallhaven-") != std::string::npos) {
      if (const auto id = wallpaperIdFromText(text)) {
        return "id:" + *id;
      }
    }

    return collapseSearchTokens(text);
  }

  std::string categoriesParam(const SearchQuery& query) {
    std::string value;
    value += query.categoryGeneral ? '1' : '0';
    value += query.categoryAnime ? '1' : '0';
    value += query.categoryPeople ? '1' : '0';
    if (value == "000") {
      return "100";
    }
    return value;
  }

  std::string purityParam(const SearchQuery& query, bool nsfwAllowed) {
    std::string value;
    value += query.puritySfw ? '1' : '0';
    value += query.puritySketchy ? '1' : '0';
    value += (query.purityNsfw && nsfwAllowed) ? '1' : '0';
    if (value == "000") {
      return "100";
    }
    return value;
  }

  std::string_view sortParam(Sort sort) {
    switch (sort) {
    case Sort::Relevance:
      return "relevance";
    case Sort::Random:
      return "random";
    case Sort::DateAdded:
      return "date_added";
    case Sort::Views:
      return "views";
    case Sort::Favorites:
      return "favorites";
    case Sort::Toplist:
      return "toplist";
    case Sort::Hot:
      return "hot";
    }
    return "date_added";
  }

  std::string_view orderParam(Order order) { return order == Order::Asc ? "asc" : "desc"; }

  std::optional<std::pair<std::string, std::string>> resolutionParam(const SearchQuery& query) {
    const auto width = parsePositiveInt(query.resolutionWidth);
    const auto height = parsePositiveInt(query.resolutionHeight);
    if (!width.has_value() || !height.has_value()) {
      return std::nullopt;
    }
    const std::string value = std::to_string(*width) + 'x' + std::to_string(*height);
    if (query.resolutionMode == ResolutionMode::Exact) {
      return std::pair<std::string, std::string>{"resolutions", value};
    }
    return std::pair<std::string, std::string>{"atleast", value};
  }

  std::optional<std::string> aspectRatioParam(std::size_t index) {
    if (index >= std::size(kAspectRatioValues)) {
      return std::nullopt;
    }
    const std::string_view value = kAspectRatioValues[index];
    if (value.empty()) {
      return std::nullopt;
    }
    if (value == "wide") {
      return "landscape";
    }
    if (value == "portrait") {
      return "portrait";
    }
    return std::string(value);
  }

  int apiPage(const SearchQuery& query) {
    const int page = std::max(1, query.page);
    if (relevanceWithoutQuery(query)) {
      return page + 1;
    }
    return page;
  }

  int normalizeTotalPages(const SearchQuery& query, int lastPage) {
    const int pages = std::max(1, lastPage);
    if (relevanceWithoutQuery(query)) {
      return std::max(1, pages - 1);
    }
    return pages;
  }

  std::string buildSearchUrl(const SearchQuery& query, bool nsfwAllowed) {
    std::string url;
    url += kApiSearchUrl;
    url += "?categories=";
    url += categoriesParam(query);
    url += "&purity=";
    url += purityParam(query, nsfwAllowed);
    url += "&page=";
    url += std::to_string(apiPage(query));
    url += "&sorting=";
    url += sortParam(query.sort);
    url += "&order=";
    url += orderParam(query.order);

    const std::string tags = normalizeSearchQuery(query.tags);
    if (!tags.empty()) {
      url += "&q=";
      url += StringUtils::urlEncode(tags);
    }
    if (const auto resolution = resolutionParam(query)) {
      url += '&';
      url += resolution->first;
      url += '=';
      url += resolution->second;
    }
    if (const auto ratio = aspectRatioParam(query.aspectRatioIndex)) {
      url += "&ratios=";
      url += *ratio;
    }
    return url;
  }

  SearchResponse parseSearchBody(std::string_view body) {
    SearchResponse out;
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
    const auto dataIt = decoded.find("data");
    if (dataIt == decoded.end() || !dataIt->is_array()) {
      const auto errIt = decoded.find("error");
      if (errIt != decoded.end() && errIt->is_string()) {
        out.error = errIt->get<std::string>();
      } else {
        out.error = "error";
      }
      return out;
    }

    const auto metaIt = decoded.find("meta");
    if (metaIt != decoded.end() && metaIt->is_object()) {
      const auto lastIt = metaIt->find("last_page");
      if (lastIt != metaIt->end() && lastIt->is_number()) {
        try {
          out.lastPage = std::max(1, lastIt->get<int>());
        } catch (const nlohmann::json::exception&) {
        }
      }
    }

    for (const auto& entry : *dataIt) {
      if (!entry.is_object()) {
        continue;
      }
      Item item;
      item.id = jsonString(entry, "id");
      item.path = jsonString(entry, "path");
      item.fileType = jsonString(entry, "file_type");
      item.resolution = jsonString(entry, "resolution");
      item.purity = jsonString(entry, "purity");
      const auto thumbsIt = entry.find("thumbs");
      if (thumbsIt != entry.end() && thumbsIt->is_object()) {
        item.thumbLarge = jsonString(*thumbsIt, "large");
        item.thumbOriginal = jsonString(*thumbsIt, "original");
        item.thumbSmall = jsonString(*thumbsIt, "small");
      }
      if (item.id.empty() || item.path.empty()) {
        continue;
      }
      out.items.push_back(std::move(item));
    }
    return out;
  }

  std::vector<Item> filterByAspectRatio(std::vector<Item> items, std::size_t aspectRatioIndex) {
    if (aspectRatioIndex == 0 || aspectRatioIndex >= std::size(kAspectRatioValues)) {
      return items;
    }
    const std::string_view selected = kAspectRatioValues[aspectRatioIndex];
    std::vector<Item> filtered;
    filtered.reserve(items.size());
    for (auto& item : items) {
      const auto parsed = parseResolution(item.resolution);
      if (!parsed.has_value()) {
        continue;
      }
      const double ratio = static_cast<double>(parsed->first) / static_cast<double>(parsed->second);
      bool include = true;
      if (selected == "wide") {
        include = ratio > 1.0;
      } else if (selected == "portrait") {
        include = ratio < 1.0;
      } else if (selected.find('x') != std::string_view::npos) {
        const auto target = parseResolution(selected);
        if (target.has_value()) {
          const double targetRatio = static_cast<double>(target->first) / static_cast<double>(target->second);
          include = std::abs(ratio - targetRatio) <= 0.05;
        }
      }
      if (include) {
        filtered.push_back(std::move(item));
      }
    }
    return filtered;
  }

  std::vector<std::string> thumbDownloadUrls(const Item& item) {
    std::vector<std::string> urls;
    const auto add = [&](const std::string& url) {
      if (url.empty()) {
        return;
      }
      if (std::ranges::find(urls, url) != urls.end()) {
        return;
      }
      urls.push_back(url);
    };
    add(item.thumbLarge);
    add(item.thumbOriginal);
    add(item.thumbSmall);
    return urls;
  }

  std::string fileExtension(const Item& item) {
    if (item.fileType == "image/png") {
      return ".png";
    }
    return ".jpg";
  }

  std::filesystem::path thumbCacheDir() {
    const std::string state = FileUtils::stateDir();
    if (state.empty()) {
      return {};
    }
    return std::filesystem::path(state) / "wallhaven" / "thumbs";
  }

  std::filesystem::path thumbCachePath(std::string_view id) {
    if (id.empty()) {
      return {};
    }
    return thumbCacheDir() / (std::string(id) + ".jpg");
  }

  std::filesystem::path downloadPath(const std::filesystem::path& dir, const Item& item) {
    return dir / ("wallhaven-" + item.id + fileExtension(item));
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

  std::size_t aspectRatioCount() { return std::size(kAspectRatioValues); }

  std::string_view aspectRatioValue(std::size_t index) {
    if (index >= std::size(kAspectRatioValues)) {
      return {};
    }
    return kAspectRatioValues[index];
  }

} // namespace wallpaper::wallhaven
