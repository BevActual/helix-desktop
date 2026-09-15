#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wallpaper::wallhaven {

  inline constexpr std::string_view kApiSearchUrl = "https://wallhaven.cc/api/v1/search";
  inline constexpr std::string_view kPluginId = "noctalia/wallhaven";

  enum class Sort : std::uint8_t {
    Relevance,
    Random,
    DateAdded,
    Views,
    Favorites,
    Toplist,
    Hot,
  };

  enum class Order : std::uint8_t { Desc, Asc };

  enum class ResolutionMode : std::uint8_t { AtLeast, Exact };

  struct SearchQuery {
    std::string tags;
    bool categoryGeneral = true;
    bool categoryAnime = true;
    bool categoryPeople = true;
    bool puritySfw = true;
    bool puritySketchy = false;
    bool purityNsfw = false;
    Sort sort = Sort::DateAdded;
    Order order = Order::Desc;
    ResolutionMode resolutionMode = ResolutionMode::AtLeast;
    std::string resolutionWidth;
    std::string resolutionHeight;
    std::size_t aspectRatioIndex = 0;
    int page = 1;
  };

  struct Item {
    std::string id;
    std::string path;
    std::string fileType;
    std::string resolution;
    std::string purity;
    std::string thumbLarge;
    std::string thumbOriginal;
    std::string thumbSmall;
  };

  struct SearchResponse {
    std::vector<Item> items;
    int lastPage = 1;
    std::string error;
  };

  [[nodiscard]] std::string normalizeSearchQuery(std::string_view raw);
  [[nodiscard]] std::string categoriesParam(const SearchQuery& query);
  [[nodiscard]] std::string purityParam(const SearchQuery& query, bool nsfwAllowed);
  [[nodiscard]] std::string_view sortParam(Sort sort);
  [[nodiscard]] std::string_view orderParam(Order order);
  [[nodiscard]] std::optional<std::pair<std::string, std::string>> resolutionParam(const SearchQuery& query);
  [[nodiscard]] std::optional<std::string> aspectRatioParam(std::size_t index);
  [[nodiscard]] int apiPage(const SearchQuery& query);
  [[nodiscard]] int normalizeTotalPages(const SearchQuery& query, int lastPage);
  [[nodiscard]] std::string buildSearchUrl(const SearchQuery& query, bool nsfwAllowed);
  [[nodiscard]] SearchResponse parseSearchBody(std::string_view body);
  [[nodiscard]] std::vector<Item> filterByAspectRatio(std::vector<Item> items, std::size_t aspectRatioIndex);
  [[nodiscard]] std::vector<std::string> thumbDownloadUrls(const Item& item);
  [[nodiscard]] std::string fileExtension(const Item& item);
  [[nodiscard]] std::filesystem::path thumbCacheDir();
  [[nodiscard]] std::filesystem::path thumbCachePath(std::string_view id);
  [[nodiscard]] std::filesystem::path downloadPath(const std::filesystem::path& dir, const Item& item);
  [[nodiscard]] std::filesystem::path resolveDownloadDir(
      std::string_view configured, std::string_view wallpaperDirectory
  );
  [[nodiscard]] std::size_t aspectRatioCount();
  [[nodiscard]] std::string_view aspectRatioValue(std::size_t index);

} // namespace wallpaper::wallhaven
