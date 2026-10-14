// Records net/wallhaven_client.cpp behaviour for services/helix-desktop/src/wallhaven.rs parity.
#include "net/wallhaven_client.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace wallpaper::wallhaven;

int main() {
  const std::vector<std::string> tags = {
      "",
      "forest",
      "  forest,  night sky ",
      "https://wallhaven.cc/w/abc123",
      "https://wallhaven.cc/search?q=cats+dogs%21&page=2",
      "wallhaven-xyz789.jpg",
      "/home/u/Pictures/wallhaven-q1w2e3.png",
      "https://example.com/other",
      "+tag -other \"a phrase\"",
      "ñandú ü",
  };
  for (const auto& t : tags)
    std::printf("norm\t%s\t%s\n", t.c_str(), normalizeSearchQuery(t).c_str());

  int n = 0;
  for (const auto& t : {std::string(""), std::string("forest night")})
    for (int sort = 0; sort <= 6; ++sort)
      for (bool asc : {false, true})
        for (int ratio : {0, 1, 2, 3, 14})
          for (int res : {0, 1, 2}) {
            SearchQuery q;
            q.tags = t;
            q.sort = static_cast<Sort>(sort);
            q.order = asc ? Order::Asc : Order::Desc;
            q.aspectRatioIndex = ratio;
            q.page = 1 + (n % 3);
            q.categoryGeneral = (n % 4) != 0;
            q.categoryAnime = (n % 5) != 0;
            q.categoryPeople = (n % 7) != 0;
            q.puritySfw = (n % 6) != 0;
            q.puritySketchy = (n % 2) == 0;
            q.purityNsfw = (n % 3) == 0;
            if (res == 1) {
              q.resolutionWidth = "1920";
              q.resolutionHeight = "1080";
            } else if (res == 2) {
              q.resolutionWidth = "3840";
              q.resolutionHeight = "2160";
              q.resolutionMode = ResolutionMode::Exact;
            }
            for (bool nsfwAllowed : {false, true})
              std::printf("url\t%s\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%d\t%d\t%d\t%s\n",
                          q.tags.c_str(), sort, asc ? "asc" : "desc", ratio, res, q.page, q.categoryGeneral,
                          q.categoryAnime, q.categoryPeople, q.puritySfw, q.puritySketchy, q.resolutionWidth.c_str(),
                          q.resolutionHeight.c_str(), q.purityNsfw, nsfwAllowed, q.resolutionMode == ResolutionMode::Exact,
                          buildSearchUrl(q, nsfwAllowed).c_str());
            for (int last : {1, 2, 9})
              std::printf("pages\t%s\t%d\t%d\t%d\n", q.tags.c_str(), sort, last, normalizeTotalPages(q, last));
            ++n;
          }

  // Aspect-ratio filtering over a spread of resolutions.
  std::vector<Item> items;
  for (const char* res : {"1920x1080", "1080x1920", "3440x1440", "1000x1000", "2560x1600", "1600x1200", "bogus", "0x5"}) {
    Item item;
    item.id = res;
    item.path = "p";
    item.resolution = res;
    items.push_back(item);
  }
  for (std::size_t ratio = 0; ratio < aspectRatioCount(); ++ratio) {
    std::printf("ratio\t%zu", ratio);
    for (const auto& kept : filterByAspectRatio(items, ratio))
      std::printf("\t%s", kept.id.c_str());
    std::putchar('\n');
  }
}
