#pragma once
#include "desktop/protocol.h"
#include "shell/wallpaper/wallhaven_client.h"
#include "config/config_service.h"
#include "net/http_client.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <unistd.h>

namespace helix {
namespace wh = wallpaper::wallhaven;
inline bool wallhavenId(std::string_view value) {
  return value.size() == 6 && std::ranges::all_of(value, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}
inline bool wallhavenImageUrl(const std::string& url, bool thumbnail) {
  auto* uri = g_uri_parse(url.c_str(), G_URI_FLAGS_NONE, nullptr);
  if (!uri) return false;
  const bool valid = g_strcmp0(g_uri_get_scheme(uri), "https") == 0 &&
    g_strcmp0(g_uri_get_host(uri), thumbnail ? "th.wallhaven.cc" : "w.wallhaven.cc") == 0 &&
    !g_uri_get_userinfo(uri) && !g_uri_get_query(uri) && !g_uri_get_fragment(uri) &&
    (g_uri_get_port(uri) == -1 || g_uri_get_port(uri) == 443);
  g_uri_unref(uri); return valid;
}
inline wh::SearchQuery wallhavenQuery(const Json& args, bool authenticated) {
  if (!args.is_object()) throw std::invalid_argument("Invalid Wallhaven search options");
  for (const char* field : {"page", "sort", "ratio", "width", "height"})
    if (args.contains(field) && (!args.at(field).is_number_integer() || args.at(field).get<double>() < 0 || args.at(field).get<double>() > 32768))
      throw std::invalid_argument("Invalid Wallhaven numeric filter");
  wh::SearchQuery query;
  query.tags = args.value("query", "");
  query.page = args.value("page", 1);
  const auto sort = args.value("sort", 2), ratio = args.value("ratio", 0);
  if (query.tags.size() > 512 || query.page < 1 || query.page > 10000 || sort < 0 || sort > 6 || ratio < 0 || std::size_t(ratio) >= wh::aspectRatioCount())
    throw std::invalid_argument("Invalid Wallhaven search options");
  query.sort = static_cast<wh::Sort>(sort); query.aspectRatioIndex = ratio;
  query.order = args.value("ascending", false) ? wh::Order::Asc : wh::Order::Desc;
  query.categoryGeneral = args.value("general", true); query.categoryAnime = args.value("anime", true); query.categoryPeople = args.value("people", true);
  query.puritySfw = args.value("sfw", true); query.puritySketchy = args.value("sketchy", false); query.purityNsfw = args.value("nsfw", false);
  if (query.purityNsfw && !authenticated) throw std::invalid_argument("A saved Wallhaven API key is required for this filter");
  const auto width = args.value("width", 0), height = args.value("height", 0);
  if (width < 0 || width > 32768 || height < 0 || height > 32768 || ((width == 0) != (height == 0)))
    throw std::invalid_argument("Specify both resolution dimensions");
  if (width) { query.resolutionWidth = std::to_string(width); query.resolutionHeight = std::to_string(height); }
  query.resolutionMode = args.value("exact", false) ? wh::ResolutionMode::Exact : wh::ResolutionMode::AtLeast;
  return query;
}

// Each client owns its jobs. Network work is bounded, cancellable and never
// exposes API keys or accepts client-supplied download URLs or output paths.
class WallhavenService {
public:
  struct Transport {
    std::function<HttpClient::StreamId(HttpRequest, HttpClient::StreamDataCallback, HttpClient::StreamCloseCallback)> start;
    std::function<void(HttpClient::StreamId)> cancel;
  };
private:
  struct Job {
    std::string owner;
    Json state = {{"busy", true}, {"error", ""}, {"items", Json::array()}};
    std::vector<wh::Item> items;
    std::set<HttpClient::StreamId> streams;
    bool cancelled = false;
    std::chrono::steady_clock::time_point touched = std::chrono::steady_clock::now();
  };
  ConfigService& config;
  HttpClient& http;
  Transport transport;
  std::function<bool()> desktop;
  std::map<std::uint64_t, std::shared_ptr<Job>> jobs;
  std::uint64_t next = 1;
  using Reply = std::function<void(bool, std::string)>;
  void request(const std::shared_ptr<Job>& job, HttpRequest request, std::size_t limit, Reply reply) {
    struct Transfer { std::string bytes; bool overflow = false; HttpClient::StreamId id = 0; };
    auto transfer = std::make_shared<Transfer>(); request.timeoutSeconds = 45;
    transfer->id = transport.start(std::move(request), [job, transfer, limit](std::string_view chunk) {
      if (job->cancelled) return;
      if (chunk.size() > limit - transfer->bytes.size()) {
        transfer->overflow = true;
        // Never erase the active curl callback from inside itself.
        return;
      }
      if (!transfer->overflow) transfer->bytes.append(chunk);
    }, [job, transfer, reply](HttpStreamResult result) {
      job->streams.erase(transfer->id);
      if (job->cancelled) return;
      reply(!transfer->overflow && result.transportOk && result.status >= 200 && result.status < 300, std::move(transfer->bytes));
    });
    if (transfer->id) job->streams.insert(transfer->id);
  }
  static bool save(const std::filesystem::path& path, const std::string& bytes, std::uint64_t id) {
    // Only raster JPEG/PNG payloads from the trusted image hosts are installed.
    if (!(bytes.starts_with("\xff\xd8\xff") || bytes.starts_with("\x89PNG\r\n\x1a\n"))) return false;
    std::error_code error; std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    auto temporary = path.string() + ".helix-" + std::to_string(id) + "-XXXXXX";
    const int fd = mkstemp(temporary.data()); if (fd < 0) return false;
    auto* file = fdopen(fd, "wb");
    if (!file) { close(fd); std::filesystem::remove(temporary, error); return false; }
    const bool written = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool closed = fclose(file) == 0;
    if (!written || !closed) { std::filesystem::remove(temporary, error); return false; }
    std::filesystem::rename(temporary, path, error);
    if (error) { std::filesystem::remove(temporary, error); return false; }
    return true;
  }
  std::shared_ptr<Job> owned(std::uint64_t id, const std::string& owner) {
    const auto found = jobs.find(id);
    if (found == jobs.end() || found->second->owner != owner) throw std::invalid_argument("Wallhaven request is no longer available");
    found->second->touched = std::chrono::steady_clock::now(); return found->second;
  }
  std::pair<std::uint64_t, std::shared_ptr<Job>> create(const std::string& owner) {
    expire();
    unsigned own = 0; for (const auto& [id, job] : jobs) if (job->owner == owner) ++own;
    if (own >= 4 || jobs.size() >= 16) throw std::runtime_error("Finish or cancel an existing Wallhaven request first");
    auto job = std::make_shared<Job>(); job->owner = owner;
    const auto id = next++; jobs.emplace(id, job); return {id, job};
  }
  std::string setting(const char* name, const std::string& configured) const {
    if (!StringUtils::trim(configured).empty()) return StringUtils::trim(configured);
    const auto plugin = config.config().plugins.pluginSettings.find(std::string(wh::kPluginId));
    if (plugin == config.config().plugins.pluginSettings.end()) return {};
    const auto value = plugin->second.find(name);
    return value == plugin->second.end() ? std::string{} : noctalia::config::widgetSettingValueAs<std::string>(value->second).value_or("");
  }
  void thumbnail(const std::shared_ptr<Job>& job, std::size_t index, std::uint64_t id) {
    if (job->cancelled || index >= job->items.size()) return;
    const auto& item = job->items[index]; const auto path = wh::thumbCachePath(item.id);
    std::string url; for (const auto& candidate : wh::thumbDownloadUrls(item)) if (wallhavenImageUrl(candidate, true)) { url = candidate; break; }
    if (url.empty()) { thumbnail(job, index + 4, id); return; }
    HttpRequest query; query.url = url;
    request(job, std::move(query), 4 * 1024 * 1024, [this, job, index, path, id](bool ok, std::string bytes) {
      if (ok && save(path, bytes, id)) job->state["items"][index]["thumbnail"] = path.string();
      thumbnail(job, index + 4, id);
    });
  }
public:
  WallhavenService(ConfigService& configuration, HttpClient& network, std::function<bool()> desktopAvailable, Transport requests = {})
    : config(configuration), http(network), transport(std::move(requests)), desktop(std::move(desktopAvailable)) {
    if (!transport.start) transport.start = [this](HttpRequest request, auto data, auto close) { return http.startStream(std::move(request), std::move(data), std::move(close)); };
    if (!transport.cancel) transport.cancel = [this](auto id) { http.cancelStream(id); };
  }
  ~WallhavenService() { for (auto& [id, job] : jobs) { job->cancelled = true; for (auto stream : job->streams) transport.cancel(stream); } }
  bool hasKey() const { return !setting("api_key", config.config().wallpaper.wallhaven.apiKey).empty(); }
  std::string downloadDirectory() const {
    const auto& wallpaper = config.config().wallpaper;
    return wh::resolveDownloadDir(setting("download_dir", wallpaper.wallhaven.downloadDir), wallpaper.directory).string();
  }
  void cancel(std::uint64_t id, const std::string& owner) {
    auto job = owned(id, owner); job->cancelled = true;
    for (auto stream : job->streams) transport.cancel(stream);
    jobs.erase(id);
  }
  void disconnect(const std::string& owner) {
    std::vector<std::uint64_t> ids; for (const auto& [id, job] : jobs) if (job->owner == owner) ids.push_back(id);
    for (auto id : ids) cancel(id, owner);
  }
  void expire() {
    std::vector<std::pair<std::uint64_t, std::string>> ids;
    for (const auto& [id, job] : jobs) if (!desktop() || config.config().shell.offlineMode || std::chrono::steady_clock::now() - job->touched > std::chrono::minutes(5)) ids.emplace_back(id, job->owner);
    for (const auto& [id, owner] : ids) cancel(id, owner);
  }
  Json result(std::uint64_t id, const std::string& owner) { return owned(id, owner)->state; }
  std::uint64_t search(const Json& args, const std::string& owner) {
    if (!desktop()) throw std::invalid_argument("Wallpaper requires a Helix desktop");
    if (config.config().shell.offlineMode) throw std::runtime_error("Wallhaven is unavailable while shell network requests are restricted");
    const auto query = wallhavenQuery(args, hasKey());
    const auto key = setting("api_key", config.config().wallpaper.wallhaven.apiKey);
    if (key.size() > 256 || key.find_first_of("\r\n") != std::string::npos) throw std::invalid_argument("Invalid saved Wallhaven API key");
    auto [id, job] = create(owner);
    HttpRequest request; request.url = wh::buildSearchUrl(query, hasKey()); request.headers = {"Accept: application/json"};
    if (!key.empty()) request.headers.push_back("X-API-Key: " + key);
    this->request(job, std::move(request), 2 * 1024 * 1024, [this, job, query, id](bool ok, std::string bytes) {
      job->state["busy"] = false;
      auto parsed = ok ? wh::parseSearchBody(bytes) : wh::SearchResponse{};
      if (!ok || !parsed.error.empty()) { job->state["error"] = "Wallhaven could not be reached. Check your connection and saved API key, then retry."; return; }
      for (auto& item : wh::filterByAspectRatio(std::move(parsed.items), query.aspectRatioIndex)) {
        if (job->items.size() == 24) break;
        if (!wallhavenId(item.id) || !wallhavenImageUrl(item.path, false) || (item.fileType != "image/png" && item.fileType != "image/jpeg")) continue;
        job->state["items"].push_back({{"id", item.id}, {"resolution", item.resolution}, {"thumbnail", ""}});
        job->items.push_back(std::move(item));
      }
      job->state["pages"] = std::clamp(wh::normalizeTotalPages(query, parsed.lastPage), 1, 10000);
      for (std::size_t i = 0; i < 4; ++i) thumbnail(job, i, id);
    });
    return id;
  }
  std::uint64_t download(std::uint64_t searchId, const std::string& imageId, const std::string& owner) {
    if (!desktop() || config.config().shell.offlineMode) throw std::invalid_argument("Wallpaper download is unavailable");
    auto found = owned(searchId, owner);
    const auto item = std::ranges::find_if(found->items, [&](const auto& candidate) { return candidate.id == imageId; });
    if (item == found->items.end()) throw std::invalid_argument("Choose an image from the current search");
    auto [id, job] = create(owner); const auto path = wh::downloadPath(downloadDirectory(), *item);
    HttpRequest request; request.url = item->path;
    this->request(job, std::move(request), 64 * 1024 * 1024, [this, job, id, path](bool ok, std::string bytes) {
      job->state["busy"] = false;
      if (!desktop() || config.config().shell.offlineMode || !ok || !save(path, bytes, id)) { job->state["error"] = "Wallpaper could not be downloaded. Try again or choose another image."; return; }
      config.setWallpaperPath(std::nullopt, path.string()); job->state["path"] = path.string();
    });
    return id;
  }
};
} // namespace helix
