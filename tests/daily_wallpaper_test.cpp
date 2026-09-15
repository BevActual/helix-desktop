#include "shell/wallpaper/daily_wallpaper.h"

#include <print>
#include <string>
#include <vector>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "daily_wallpaper_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

} // namespace

int main() {
  using namespace wallpaper::daily;
  bool ok = true;

  ok = expect(normalizeDate("20260914") == "2026-09-14", "bing compact date normalizes") && ok;
  ok = expect(normalizeDate("2026-09-14") == "2026-09-14", "iso date stays iso") && ok;
  ok = expect(compactDate("2026-09-14") == "20260914", "iso date compacts") && ok;
  ok = expect(addDaysToDate("2026-09-14", -1) == "2026-09-13", "subtracts one day") && ok;
  ok = expect(addDaysToDate("2026-03-01", -1) == "2026-02-28", "crosses month boundary") && ok;

  ok = expect(resolveBingMarket("") == "en-US", "empty market defaults") && ok;
  ok = expect(resolveBingMarket("  ja-JP ") == "ja-JP", "trims market") && ok;
  ok = expect(resolveNasaApiKey("", "") == "DEMO_KEY", "demo key fallback") && ok;
  ok = expect(resolveNasaApiKey("cfg", "env") == "cfg", "config key wins") && ok;
  ok = expect(resolveNasaApiKey("", "env-key") == "env-key", "env key used when config empty") && ok;

  const std::string bingUrl = buildBingArchiveUrl("en-GB", 8);
  ok = expect(bingUrl.find("https://www.bing.com/HPImageArchive.aspx?") == 0, "bing archive host") && ok;
  ok = expect(bingUrl.find("format=js") != std::string::npos, "bing json format") && ok;
  ok = expect(bingUrl.find("n=8") != std::string::npos, "bing count") && ok;
  ok = expect(bingUrl.find("mkt=en-GB") != std::string::npos, "bing market") && ok;

  const std::string nasaUrl = buildNasaApodUrl("DEMO_KEY", "2026-09-01", "2026-09-14");
  ok = expect(nasaUrl.find("https://api.nasa.gov/planetary/apod?") == 0, "nasa apod host") && ok;
  ok = expect(nasaUrl.find("api_key=DEMO_KEY") != std::string::npos, "nasa demo key") && ok;
  ok = expect(nasaUrl.find("start_date=2026-09-01") != std::string::npos, "nasa start") && ok;
  ok = expect(nasaUrl.find("end_date=2026-09-14") != std::string::npos, "nasa end") && ok;

  ok = expect(absoluteBingUrl("/th?id=OHR.Foo").find("https://www.bing.com/th?id=OHR.Foo") == 0, "bing relative url")
      && ok;
  ok = expect(absoluteBingUrl("https://cdn.example/a.jpg") == "https://cdn.example/a.jpg", "absolute url passthrough")
      && ok;
  ok = expect(bingUhdUrl("/th?id=OHR.Foo_EN-US1") == "https://www.bing.com/th?id=OHR.Foo_EN-US1_UHD.jpg", "uhd url")
      && ok;
  ok = expect(ohrNameFromUrlbase("/th?id=OHR.GoldenGate_EN-US1") == "goldengate", "ohr slug") && ok;
  ok = expect(fileExtensionFromUrl("https://apod.nasa.gov/a/foo.PNG?x=1") == ".png", "ext from url") && ok;
  ok = expect(slugFromTitle("Hello, Hubble!") == "hello-hubble", "title slug") && ok;

  const auto bing = parseBingArchiveBody(R"json({
    "images": [
      {
        "startdate": "20260914",
        "url": "/th?id=OHR.River_EN-US123_1920x1080.jpg&rf=LaDigue_1920x1080.jpg&pid=hp",
        "urlbase": "/th?id=OHR.River_EN-US123",
        "copyright": "A river, Photographer",
        "title": "River Light"
      },
      {
        "startdate": "20260913",
        "url": "/th?id=OHR.Forest_EN-US456_1920x1080.jpg",
        "urlbase": "/th?id=OHR.Forest_EN-US456",
        "title": "Forest"
      }
    ]
  })json");
  ok = expect(bing.error.empty(), "parses bing archive") && ok;
  ok = expect(bing.items.size() == 2, "reads bing rows") && ok;
  ok = expect(bing.items[0].id == "bing:2026-09-14", "newest bing first") && ok;
  ok = expect(bing.items[0].imageUrl.find("https://www.bing.com/th?id=OHR.River") == 0, "bing image url") && ok;
  ok = expect(bing.items[0].uhdUrl.find("_UHD.jpg") != std::string::npos, "bing uhd url") && ok;
  ok = expect(downloadUrls(bing.items[0]).size() == 2, "bing tries uhd then standard") && ok;
  ok = expect(
      downloadPath("/tmp", bing.items[0]).filename() == "bing-20260914-river-light.jpg", "bing download name"
  ) && ok;

  const auto nasa = parseNasaApodBody(R"json([
    {
      "date": "2026-09-13",
      "media_type": "video",
      "title": "A clip",
      "url": "https://www.youtube.com/embed/abc"
    },
    {
      "date": "2026-09-14",
      "media_type": "image",
      "title": "Nebula",
      "hdurl": "https://apod.nasa.gov/apod/image/2609/nebula_hd.jpg",
      "url": "https://apod.nasa.gov/apod/image/2609/nebula.jpg"
    },
    {
      "date": "2026-09-12",
      "media_type": "image",
      "title": "Moon",
      "url": "https://apod.nasa.gov/apod/image/2609/moon.jpg"
    }
  ])json");
  ok = expect(nasa.error.empty(), "parses nasa archive") && ok;
  ok = expect(nasa.items.size() == 2, "skips nasa video days") && ok;
  ok = expect(nasa.items[0].id == "nasa:2026-09-14", "newest nasa still first") && ok;
  ok = expect(nasa.items[0].imageUrl.find("nebula_hd.jpg") != std::string::npos, "prefers hdurl") && ok;
  ok = expect(itemForDate(nasa.items, "2026-09-14") == &nasa.items[0], "finds today's nasa still") && ok;
  ok = expect(
      downloadPath("/tmp", nasa.items[0]).filename() == "nasa-20260914-nebula.jpg", "nasa download name"
  ) && ok;

  const auto single = parseNasaApodBody(R"json({
    "date": "2026-09-14",
    "media_type": "image",
    "title": "Solo",
    "url": "https://apod.nasa.gov/apod/image/2609/solo.jpg"
  })json");
  ok = expect(single.items.size() == 1 && single.items[0].id == "nasa:2026-09-14", "parses single nasa object") && ok;

  const auto limited = limitGridItems(std::vector<Item>(12));
  ok = expect(limited.size() == kMaxGridItems, "caps grid at eight") && ok;

  if (!ok) {
    return 1;
  }
  return 0;
}
