#include "net/wallhaven_client.h"

#include <print>
#include <string>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "wallhaven_client_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

} // namespace

int main() {
  using namespace wallpaper::wallhaven;
  bool ok = true;

  SearchQuery query;
  ok = expect(categoriesParam(query) == "111", "default categories are all on") && ok;
  query.categoryGeneral = false;
  query.categoryAnime = false;
  query.categoryPeople = false;
  ok = expect(categoriesParam(query) == "100", "categories never emit 000") && ok;

  query = {};
  query.puritySfw = false;
  query.puritySketchy = false;
  query.purityNsfw = true;
  ok = expect(purityParam(query, false) == "100", "nsfw is ignored without an API key") && ok;
  ok = expect(purityParam(query, true) == "001", "nsfw is included with an API key") && ok;

  query = {};
  query.sort = Sort::Relevance;
  query.page = 1;
  ok = expect(apiPage(query) == 2, "relevance without tags skips empty first page") && ok;
  ok = expect(normalizeTotalPages(query, 4) == 3, "relevance without tags shrinks last_page") && ok;
  query.tags = " mountains ";
  ok = expect(apiPage(query) == 1, "relevance with tags uses the requested page") && ok;

  ok = expect(normalizeSearchQuery(" nature,  city night ") == "nature city night", "commas become tag separators")
      && ok;
  ok = expect(normalizeSearchQuery("+nature -city") == "+nature -city", "keeps include/exclude prefixes") && ok;
  ok = expect(normalizeSearchQuery("https://wallhaven.cc/w/l81zwp") == "id:l81zwp", "wallpaper URLs become id queries")
      && ok;
  ok = expect(
           normalizeSearchQuery("https://www.wallhaven.cc/w/abc12/?ref=1") == "id:abc12",
           "strips URL extras from wallpaper ids"
       )
      && ok;
  ok = expect(
           normalizeSearchQuery("https://wallhaven.cc/search?q=nature+city&categories=111") == "nature city",
           "search URLs keep the q parameter"
       )
      && ok;
  ok =
      expect(normalizeSearchQuery("wallhaven-l81zwp.jpg") == "id:l81zwp", "download filenames become id queries") && ok;

  query = {};
  query.tags = "city, night";
  query.resolutionWidth = "2560";
  query.resolutionHeight = "1440";
  query.aspectRatioIndex = 3;
  const std::string url = buildSearchUrl(query, false);
  ok = expect(url.find("https://wallhaven.cc/api/v1/search?") == 0, "search URL uses the wallhaven API") && ok;
  ok = expect(url.find("q=city%20night") != std::string::npos, "encodes the tag query") && ok;
  ok = expect(url.find("atleast=2560x1440") != std::string::npos, "emits atleast resolution") && ok;
  ok = expect(url.find("ratios=16x9") != std::string::npos, "emits exact aspect ratio") && ok;
  ok = expect(aspectRatioParam(1).value_or("") == "landscape", "wide maps to landscape") && ok;

  const auto parsed = parseSearchBody(R"({
    "data": [
      {
        "id": "abc12",
        "path": "https://w.wallhaven.cc/full/abc12.jpg",
        "file_type": "image/png",
        "resolution": "1920x1080",
        "purity": "sfw",
        "thumbs": { "large": "https://th.wallhaven.cc/lg/abc12.jpg" }
      },
      {
        "id": "port1",
        "path": "https://w.wallhaven.cc/full/port1.jpg",
        "file_type": "image/jpeg",
        "resolution": "1080x1920",
        "purity": "sfw",
        "thumbs": { "large": "https://th.wallhaven.cc/lg/port1.jpg" }
      }
    ],
    "meta": { "last_page": 7 }
  })");
  ok = expect(parsed.error.empty(), "parses a search payload") && ok;
  ok = expect(parsed.lastPage == 7, "reads last_page") && ok;
  ok = expect(parsed.items.size() == 2, "reads wallpaper rows") && ok;
  ok = expect(parsed.items[0].thumbLarge.find("abc12.jpg") != std::string::npos, "reads large thumb") && ok;
  ok = expect(thumbDownloadUrls(parsed.items[0]).size() == 1, "thumb urls include the large preview") && ok;
  ok = expect(fileExtension(parsed.items[0]) == ".png", "png file type uses .png") && ok;
  ok = expect(downloadPath("/tmp", parsed.items[0]).filename() == "wallhaven-abc12.png", "download name includes id")
      && ok;

  const auto wideOnly = filterByAspectRatio(parsed.items, 1);
  ok = expect(wideOnly.size() == 1 && wideOnly[0].id == "abc12", "wide filter keeps landscape") && ok;
  const auto portraitOnly = filterByAspectRatio(parsed.items, 2);
  ok = expect(portraitOnly.size() == 1 && portraitOnly[0].id == "port1", "portrait filter keeps portrait") && ok;

  const auto failed = parseSearchBody(R"({"error":"Unauthorized"})");
  ok = expect(failed.error == "Unauthorized" && failed.items.empty(), "reads API error bodies") && ok;

  return ok ? 0 : 1;
}
