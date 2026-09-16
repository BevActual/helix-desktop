#include "net/syncthing_service.h"

#include "cli/schema_msg.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "net/http_client.h"
#include "net/url_open.h"
#include "util/string_utils.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <utility>

namespace {

  constexpr Logger kLog("syncthing");
  constexpr auto kRequestTimeout = 6L;
  constexpr const char* kDefaultOrigin = "http://127.0.0.1:8384";

  [[nodiscard]] std::string jsonString(const nlohmann::json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_string()) {
      return {};
    }
    return object[key].get<std::string>();
  }

  [[nodiscard]] bool jsonBool(const nlohmann::json& object, const char* key, bool fallback = false) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_boolean()) {
      return fallback;
    }
    return object[key].get<bool>();
  }

  [[nodiscard]] double jsonNumber(const nlohmann::json& object, const char* key, double fallback = 0.0) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_number()) {
      return fallback;
    }
    return object[key].get<double>();
  }

  [[nodiscard]] std::string xmlText(std::string_view xml, std::string_view tag) {
    const std::string open = "<" + std::string(tag);
    const auto start = xml.find(open);
    if (start == std::string_view::npos) {
      return {};
    }
    const auto gt = xml.find('>', start);
    if (gt == std::string_view::npos) {
      return {};
    }
    const std::string close = "</" + std::string(tag) + ">";
    const auto end = xml.find(close, gt + 1);
    if (end == std::string_view::npos) {
      return {};
    }
    return StringUtils::trim(std::string(xml.substr(gt + 1, end - (gt + 1))));
  }

  [[nodiscard]] bool xmlOpeningHasTrue(std::string_view xml, std::string_view tag, std::string_view attr) {
    const std::string open = "<" + std::string(tag);
    const auto start = xml.find(open);
    if (start == std::string_view::npos) {
      return false;
    }
    const auto gt = xml.find('>', start);
    if (gt == std::string_view::npos) {
      return false;
    }
    const std::string_view opening = xml.substr(start, gt - start);
    const std::string needle = std::string(attr) + "=\"true\"";
    return opening.find(needle) != std::string_view::npos;
  }

  [[nodiscard]] std::filesystem::path configXmlPath() {
    if (const char* confDir = std::getenv("STCONFDIR"); confDir != nullptr && confDir[0] != '\0') {
      return std::filesystem::path(confDir) / "config.xml";
    }
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
      return std::filesystem::path(xdg) / "syncthing" / "config.xml";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      return std::filesystem::path(home) / ".config" / "syncthing" / "config.xml";
    }
    return {};
  }

  [[nodiscard]] std::string originFromAddress(std::string address, bool tls) {
    address = StringUtils::trim(address);
    if (address.empty() || address.starts_with("unix:") || address.starts_with("unixs:")) {
      return {};
    }
    if (address.starts_with("http://") || address.starts_with("https://")) {
      while (!address.empty() && address.back() == '/') {
        address.pop_back();
      }
      return address;
    }
    if (address.starts_with(":")) {
      address = "127.0.0.1" + address;
    } else if (address.starts_with("0.0.0.0:")) {
      address.replace(0, 7, "127.0.0.1");
    } else if (address.starts_with("[::]:")) {
      address = "127.0.0.1" + address.substr(4);
    }
    return std::string(tls ? "https://" : "http://") + address;
  }

  [[nodiscard]] std::optional<nlohmann::json> parseJson(std::string_view body) {
    try {
      auto parsed = nlohmann::json::parse(body, nullptr, true, true);
      if (!parsed.is_object() && !parsed.is_array()) {
        return std::nullopt;
      }
      return parsed;
    } catch (const nlohmann::json::exception&) {
      return std::nullopt;
    }
  }

  [[nodiscard]] std::string urlEncode(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
      if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
        out.push_back(static_cast<char>(ch));
      } else {
        out += std::format("%{:02X}", ch);
      }
    }
    return out;
  }

} // namespace

SyncthingService::SyncthingService(HttpClient& http) : m_http(http) {}

SyncthingService::~SyncthingService() {
  m_pollTimer.stop();
  m_retryTimer.stop();
  m_lifetime.reset();
}

void SyncthingService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void SyncthingService::start() {
  refresh();
  schedulePoll();
}

void SyncthingService::setWantFastPoll(bool enabled) {
  if (m_fastPoll == enabled) {
    return;
  }
  m_fastPoll = enabled;
  schedulePoll();
  if (enabled) {
    refresh();
  }
}

void SyncthingService::schedulePoll() {
  m_pollTimer.stop();
  const auto seconds = m_fastPoll && m_snapshot.running ? 3 : (m_snapshot.running ? 8 : 12);
  m_pollTimer.start(std::chrono::seconds(seconds), [this]() { refresh(); });
}

void SyncthingService::notifyChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void SyncthingService::loadGuiAuth() {
  m_auth = {};
  if (const char* gui = std::getenv("STGUIADDRESS"); gui != nullptr && gui[0] != '\0') {
    m_auth.tls = std::string_view(gui).starts_with("https://") || std::string_view(gui).starts_with("unixs:");
    m_auth.origin = originFromAddress(gui, m_auth.tls);
  }
  const auto path = configXmlPath();
  if (path.empty()) {
    if (m_auth.origin.empty()) {
      m_auth.origin = kDefaultOrigin;
    }
    return;
  }
  std::ifstream file(path);
  if (!file) {
    if (m_auth.origin.empty()) {
      m_auth.origin = kDefaultOrigin;
    }
    return;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  const std::string xml = buffer.str();
  m_auth.apiKey = xmlText(xml, "apikey");
  m_auth.tls = xmlOpeningHasTrue(xml, "gui", "tls") || m_auth.tls;
  if (m_auth.origin.empty()) {
    m_auth.origin = originFromAddress(xmlText(xml, "address"), m_auth.tls);
  }
  if (m_auth.origin.empty()) {
    m_auth.origin = kDefaultOrigin;
  }
}

std::string SyncthingService::restUrl(std::string_view path) const {
  std::string origin = m_auth.origin.empty() ? kDefaultOrigin : m_auth.origin;
  while (!origin.empty() && origin.back() == '/') {
    origin.pop_back();
  }
  if (path.empty() || path.front() != '/') {
    return origin + "/" + std::string(path);
  }
  return origin + std::string(path);
}

void SyncthingService::requestJson(
    std::string_view method, std::string path, std::string body, std::function<void(bool ok, std::string body)> onDone
) {
  HttpRequest req;
  req.method = std::string(method);
  req.url = restUrl(path);
  req.body = std::move(body);
  req.allowWhenOffline = true;
  req.allowInsecureTls = m_auth.tls;
  req.timeoutSeconds = kRequestTimeout;
  if (!m_auth.apiKey.empty()) {
    req.headers.push_back("X-API-Key: " + m_auth.apiKey);
  }
  if (!req.body.empty()) {
    req.headers.emplace_back("Content-Type: application/json");
  }
  const std::weak_ptr<void> lifetime = m_lifetime;
  m_http.request(std::move(req), [lifetime, onDone = std::move(onDone)](HttpResponse res) {
    if (lifetime.expired()) {
      return;
    }
    const bool ok = res.transportOk && res.status >= 200 && res.status < 300;
    onDone(ok, std::move(res.body));
  });
}

void SyncthingService::finishRefresh(SyncthingSnapshot next) {
  m_refreshInFlight = false;
  next.folderCount = static_cast<int>(next.folders.size());
  next.connectedCount = 0;
  for (const auto& remote : next.remotes) {
    if (remote.connected) {
      ++next.connectedCount;
    }
  }
  const bool pollChanged = next.running != m_snapshot.running;
  if (next == m_snapshot) {
    if (pollChanged) {
      schedulePoll();
    }
    return;
  }
  m_snapshot = std::move(next);
  if (pollChanged) {
    schedulePoll();
  }
  notifyChanged();
}

void SyncthingService::applyStatusJson(SyncthingSnapshot& next, std::string_view body) {
  if (const auto parsed = parseJson(body)) {
    next.myId = jsonString(*parsed, "myID");
    next.myName = jsonString(*parsed, "myName");
  }
}

void SyncthingService::applyConfigJson(SyncthingSnapshot& next, std::string_view body) {
  const auto parsed = parseJson(body);
  if (!parsed) {
    return;
  }
  if (parsed->contains("folders") && (*parsed)["folders"].is_array()) {
    for (const auto& folder : (*parsed)["folders"]) {
      if (!folder.is_object()) {
        continue;
      }
      SyncthingFolder item;
      item.id = jsonString(folder, "id");
      item.label = jsonString(folder, "label");
      item.path = jsonString(folder, "path");
      item.type = jsonString(folder, "type");
      item.paused = jsonBool(folder, "paused");
      item.state = item.paused ? "paused" : "idle";
      if (!item.id.empty()) {
        next.folders.push_back(std::move(item));
      }
    }
  }
  if (parsed->contains("devices") && (*parsed)["devices"].is_array()) {
    for (const auto& device : (*parsed)["devices"]) {
      if (!device.is_object()) {
        continue;
      }
      SyncthingRemote remote;
      remote.id = jsonString(device, "deviceID");
      if (remote.id.empty()) {
        remote.id = jsonString(device, "deviceId");
      }
      remote.name = jsonString(device, "name");
      remote.paused = jsonBool(device, "paused");
      if (remote.id.empty() || remote.id == next.myId) {
        continue;
      }
      next.remotes.push_back(std::move(remote));
    }
  }
}

void SyncthingService::applyConnectionsJson(SyncthingSnapshot& next, std::string_view body) {
  const auto parsed = parseJson(body);
  if (!parsed || !parsed->contains("connections") || !(*parsed)["connections"].is_object()) {
    return;
  }
  const auto& connections = (*parsed)["connections"];
  for (auto& remote : next.remotes) {
    if (!connections.contains(remote.id) || !connections[remote.id].is_object()) {
      continue;
    }
    remote.connected = jsonBool(connections[remote.id], "connected");
    remote.paused = jsonBool(connections[remote.id], "paused", remote.paused);
  }
}

void SyncthingService::applyFolderStatusJson(SyncthingFolder& folder, std::string_view body) {
  const auto parsed = parseJson(body);
  if (!parsed) {
    return;
  }
  folder.state = jsonString(*parsed, "state");
  folder.needBytes = static_cast<std::int64_t>(jsonNumber(*parsed, "needBytes"));
  const double global = jsonNumber(*parsed, "globalBytes");
  if (global > 0.0) {
    folder.completion = 100.0 * (1.0 - (static_cast<double>(folder.needBytes) / global));
  } else {
    folder.completion = jsonNumber(*parsed, "completion", 100.0);
  }
}

void SyncthingService::fetchFolderStatuses(SyncthingSnapshot next, std::uint64_t generation) {
  const std::weak_ptr<void> lifetime = m_lifetime;
  std::size_t pending = 0;
  for (const auto& folder : next.folders) {
    if (!folder.paused) {
      ++pending;
    }
  }
  if (pending == 0) {
    finishRefresh(std::move(next));
    return;
  }
  auto remaining = std::make_shared<std::size_t>(pending);
  auto shared = std::make_shared<SyncthingSnapshot>(std::move(next));
  for (std::size_t index = 0; index < shared->folders.size(); ++index) {
    if (shared->folders[index].paused) {
      continue;
    }
    const std::string path = "/rest/db/status?folder=" + urlEncode(shared->folders[index].id);
    requestJson("GET", path, {}, [this, lifetime, generation, remaining, shared, index](bool folderOk, std::string folderBody) {
      if (lifetime.expired() || generation != m_generation) {
        return;
      }
      if (folderOk) {
        applyFolderStatusJson(shared->folders[index], folderBody);
      }
      if (--(*remaining) == 0) {
        finishRefresh(std::move(*shared));
      }
    });
  }
}

void SyncthingService::refresh() {
  if (m_refreshInFlight) {
    return;
  }
  m_refreshInFlight = true;
  const std::uint64_t generation = ++m_generation;
  const std::weak_ptr<void> lifetime = m_lifetime;
  loadGuiAuth();

  SyncthingSnapshot next;
  next.installed = process::commandExists("syncthing");
  next.busy = m_snapshot.busy;
  next.guiUrl = m_auth.origin.empty() ? kDefaultOrigin : m_auth.origin;
  if (!next.installed) {
    finishRefresh(std::move(next));
    return;
  }

  HttpRequest health;
  health.method = "GET";
  health.url = restUrl("/rest/noauth/health");
  health.allowWhenOffline = true;
  health.allowInsecureTls = m_auth.tls;
  health.timeoutSeconds = kRequestTimeout;
  m_http.request(std::move(health), [this, lifetime, generation, next = std::move(next)](HttpResponse res) mutable {
    if (lifetime.expired() || generation != m_generation) {
      return;
    }
    if (!(res.transportOk && res.status >= 200 && res.status < 300)) {
      finishRefresh(std::move(next));
      return;
    }
    next.running = true;
    if (m_auth.apiKey.empty()) {
      next.error = i18n::tr("control-center.syncthing.status-failed");
      finishRefresh(std::move(next));
      return;
    }
    requestJson("GET", "/rest/system/status", {}, [this, lifetime, generation, next](bool statusOk, std::string statusBody) mutable {
      if (lifetime.expired() || generation != m_generation) {
        return;
      }
      if (!statusOk) {
        next.error = i18n::tr("control-center.syncthing.status-failed");
        finishRefresh(std::move(next));
        return;
      }
      applyStatusJson(next, statusBody);
      requestJson("GET", "/rest/config", {}, [this, lifetime, generation, next = std::move(next)](bool configOk, std::string configBody) mutable {
        if (lifetime.expired() || generation != m_generation) {
          return;
        }
        if (!configOk) {
          next.error = i18n::tr("control-center.syncthing.status-failed");
          finishRefresh(std::move(next));
          return;
        }
        applyConfigJson(next, configBody);
        requestJson(
            "GET", "/rest/system/connections", {},
            [this, lifetime, generation, next = std::move(next)](bool connOk, std::string connBody) mutable {
              if (lifetime.expired() || generation != m_generation) {
                return;
              }
              if (connOk) {
                applyConnectionsJson(next, connBody);
              }
              fetchFolderStatuses(std::move(next), generation);
            }
        );
      });
    });
  });
}

void SyncthingService::tryStart() {
  if (m_snapshot.running || !process::commandExists("syncthing")) {
    refresh();
    return;
  }
  m_snapshot.busy = true;
  notifyChanged();
  const std::weak_ptr<void> lifetime = m_lifetime;
  (void)process::runAsync(
      std::vector<std::string>{"systemctl", "--user", "start", "syncthing.service"},
      process::RunCallbacks{
          .onExit =
              [this, lifetime](process::RunResult result) {
                DeferredCall::callLater([this, lifetime, ok = static_cast<bool>(result)]() {
                  if (lifetime.expired()) {
                    return;
                  }
                  if (!ok) {
                    (void)process::runAsync(std::vector<std::string>{"syncthing", "serve", "--no-browser"});
                  }
                  m_snapshot.busy = false;
                  m_retryTimer.start(std::chrono::milliseconds(800), [this]() { refresh(); });
                });
              },
      },
      process::RunOptions{.timeout = std::chrono::seconds(8)}
  );
}

void SyncthingService::shutdown() {
  if (!m_snapshot.running) {
    return;
  }
  m_snapshot.busy = true;
  notifyChanged();
  const std::weak_ptr<void> lifetime = m_lifetime;
  requestJson("POST", "/rest/system/shutdown", {}, [this, lifetime](bool /*ok*/, std::string) {
    if (lifetime.expired()) {
      return;
    }
    (void)process::runAsync(std::vector<std::string>{"systemctl", "--user", "stop", "syncthing.service"});
    m_snapshot.busy = false;
    m_retryTimer.start(std::chrono::milliseconds(600), [this]() { refresh(); });
  });
}

void SyncthingService::openGui() {
  const std::string url = m_snapshot.guiUrl.empty() ? kDefaultOrigin : m_snapshot.guiUrl;
  (void)net::openInBrowser(url);
}

void SyncthingService::setFolderPaused(const std::string& folderId, bool paused) {
  if (folderId.empty() || !m_snapshot.running) {
    return;
  }
  const std::string path = "/rest/config/folders/" + urlEncode(folderId);
  const std::string body = paused ? "{\"paused\":true}" : "{\"paused\":false}";
  const std::weak_ptr<void> lifetime = m_lifetime;
  requestJson("PATCH", path, body, [this, lifetime](bool ok, std::string) {
    if (lifetime.expired()) {
      return;
    }
    if (!ok) {
      m_snapshot.error = i18n::tr("control-center.syncthing.status-failed");
      notifyChanged();
    }
    refresh();
  });
}

void SyncthingService::scanFolder(const std::string& folderId) {
  if (folderId.empty() || !m_snapshot.running) {
    return;
  }
  const std::string path = "/rest/db/scan?folder=" + urlEncode(folderId);
  const std::weak_ptr<void> lifetime = m_lifetime;
  requestJson("POST", path, {}, [this, lifetime](bool /*ok*/, std::string) {
    if (lifetime.expired()) {
      return;
    }
    refresh();
  });
}

void SyncthingService::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::syncthingStatus, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: syncthing-status takes no arguments\n";
    }
    if (!m_snapshot.installed) {
      return "not-installed\n";
    }
    if (!m_snapshot.running) {
      return m_snapshot.busy ? "starting\n" : "stopped\n";
    }
    return std::format(
        "running name={} folders={} connected={}\n", m_snapshot.myName.empty() ? "-" : m_snapshot.myName,
        m_snapshot.folderCount, m_snapshot.connectedCount
    );
  });
  ipc.bind(noctalia::cli::msg::syncthingStart, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: syncthing-start takes no arguments\n";
    }
    if (!m_snapshot.installed) {
      return "error: syncthing is not installed\n";
    }
    tryStart();
    return "ok\n";
  });
  ipc.bind(noctalia::cli::msg::syncthingStop, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: syncthing-stop takes no arguments\n";
    }
    shutdown();
    return "ok\n";
  });
}
