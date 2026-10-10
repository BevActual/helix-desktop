#include "desktop/wallhaven.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

int main() {
  using namespace helix;
  assert(wallhavenId("abc123") && !wallhavenId("../abc") && !wallhavenId("ABC123"));
  assert(wallhavenImageUrl("https://w.wallhaven.cc/full/ab/wallhaven-abc123.jpg", false));
  for (const auto& url : {"http://w.wallhaven.cc/a", "https://evil.test/a", "https://w.wallhaven.cc.evil.test/a", "https://x@w.wallhaven.cc/a", "file:///a", "https://w.wallhaven.cc:444/a"}) assert(!wallhavenImageUrl(url, false));
  assert(!wallhavenImageUrl("https://w.wallhaven.cc/a", true));
  auto invalid = [](const std::function<void()>& action) { try { action(); return false; } catch (...) { return true; } };
  assert(invalid([] { wallhavenQuery({{"sort", 8}}, false); }));
  assert(invalid([] { wallhavenQuery({{"width", 100}}, false); }));
  assert(invalid([] { wallhavenQuery({{"page", 1.5}}, false); }));
  assert(invalid([] { wallhavenQuery({{"nsfw", true}}, false); }));
  auto query = wallhavenQuery({{"query", "mountains"}, {"width", 1920}, {"height", 1080}, {"ratio", 3}}, false);
  assert(wh::buildSearchUrl(query, false).find("atleast=1920x1080") != std::string::npos);
  char pattern[] = "/tmp/helix-wallhaven-XXXXXX"; const auto* directory = mkdtemp(pattern); assert(directory);
  const std::filesystem::path root(directory);
  for (const auto* kind : {"CONFIG", "STATE", "DATA", "CACHE"}) {
    setenv((std::string("XDG_") + kind + "_HOME").c_str(), (root / kind).c_str(), 1);
    if (std::string(kind) != "CACHE") setenv((std::string("HELIX_") + kind + "_HOME").c_str(), (root / kind).c_str(), 1);
  }
  std::filesystem::create_directories(root / "CONFIG/helix");
  std::ofstream(root / "CONFIG/helix/config.toml") << "[wallpaper.wallhaven]\napi_key = \"test-secret\"\ndownload_dir = \"" << (root / "downloads").string() << "\"\n";
  ConfigService config; HttpClient http; bool desktop = true;
  struct Request { HttpRequest request; HttpClient::StreamDataCallback data; HttpClient::StreamCloseCallback close; };
  std::map<std::uint64_t, Request> requests; std::uint64_t next = 1;
  WallhavenService service(config, http, [&] { return desktop; }, {
    [&](HttpRequest request, auto data, auto close) { const auto id = next++; requests.emplace(id, Request{std::move(request), std::move(data), std::move(close)}); return id; },
    [&](auto id) { requests.erase(id); }
  });
  auto deliver = [&](std::string body, bool ok = true) { assert(!requests.empty()); auto request = std::move(requests.begin()->second); requests.erase(requests.begin()); request.data(body); request.close({ok, ok ? 200L : 500L}); };
  assert(service.hasKey());
  const auto job = service.search(Json::object(), "client-a");
  assert(requests.begin()->second.request.headers.back() == "X-API-Key: test-secret");
  assert(requests.begin()->second.request.url.find("test-secret") == std::string::npos);
  assert(!requests.begin()->second.request.followRedirects);
  assert(invalid([&] { service.result(job, "client-b"); }));
  assert(invalid([&] { service.cancel(job, "client-b"); }));
  deliver(R"({"data":[{"id":"abc123","path":"https://w.wallhaven.cc/full/ab/wallhaven-abc123.png","file_type":"image/png","resolution":"1920x1080","thumbs":{"large":"https://th.wallhaven.cc/lg/ab/abc123.jpg"}},{"id":"bad123","path":"https://evil.test/image.png","file_type":"image/png"}],"meta":{"last_page":2}})");
  auto result = service.result(job, "client-a"); assert(result.at("items").size() == 1 && result.at("pages") == 2);
  assert(result.dump().find("test-secret") == std::string::npos);
  const std::string png = "\x89PNG\r\n\x1a\nfixture";
  deliver(png);
  assert(!service.result(job, "client-a").at("items")[0].at("thumbnail").get<std::string>().empty());
  assert(invalid([&] { service.download(job, "bad123", "client-a"); }));
  auto download = service.download(job, "abc123", "client-a");
  service.cancel(download, "client-a"); assert(requests.empty());
  assert(!std::filesystem::exists(root / "downloads/wallhaven-abc123.png"));
  download = service.download(job, "abc123", "client-a"); deliver("not an image");
  assert(!service.result(download, "client-a").at("error").get<std::string>().empty()); service.cancel(download, "client-a");
  download = service.download(job, "abc123", "client-a"); deliver(png);
  assert(config.getPaletteWallpaperPath() == (root / "downloads/wallhaven-abc123.png").string());
  assert(service.result(download, "client-a").at("path") == config.getPaletteWallpaperPath()); service.cancel(download, "client-a");
  service.cancel(job, "client-a");
  const auto oversized = service.search(Json::object(), "client-a"); deliver(std::string(2 * 1024 * 1024 + 1, 'x'));
  assert(!service.result(oversized, "client-a").at("error").get<std::string>().empty()); service.cancel(oversized, "client-a");
  service.search(Json::object(), "client-a"); service.disconnect("client-a"); assert(requests.empty());
  service.search(Json::object(), "client-a"); desktop = false; service.expire(); assert(requests.empty());
  assert(invalid([&] { service.search(Json::object(), "client-a"); }));
  std::filesystem::remove_all(root);
}
