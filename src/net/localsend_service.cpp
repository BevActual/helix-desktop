#include "net/localsend_service.h"

#include "cli/schema_msg.h"
#include "config/config_service.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "net/http_client.h"
#include "notification/notification_manager.h"
#include "system/distro_info.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace {
  constexpr Logger kLog("localsend");
  constexpr auto kAnnounceInterval = std::chrono::seconds(5);
  constexpr auto kPeerTtl = std::chrono::seconds(30);
  constexpr auto kIncomingTimeout = std::chrono::seconds(60);
  constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
  constexpr std::size_t kMaxJsonBody = 8 * 1024 * 1024;
  constexpr std::size_t kMaxConns = 16;
  constexpr const char* kMulticast = "224.0.0.167";

  void closeFd(int& fd) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }

  [[nodiscard]] int makeNonblockSocket(int type) {
    int fd = ::socket(AF_INET, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
      return fd;
    }
    fd = ::socket(AF_INET, type, 0);
    if (fd < 0) {
      return -1;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
      (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
  }

  [[nodiscard]] std::string localIpv4() {
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) {
      return {};
    }
    std::string found;
    for (ifaddrs* it = list; it != nullptr; it = it->ifa_next) {
      if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) {
        continue;
      }
      const auto* addr = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
      char buf[INET_ADDRSTRLEN]{};
      if (::inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf)) == nullptr) {
        continue;
      }
      const std::string ip = buf;
      if (ip.starts_with("127.") || ip == "0.0.0.0") {
        continue;
      }
      found = ip;
      break;
    }
    ::freeifaddrs(list);
    return found;
  }

  [[nodiscard]] std::filesystem::path uniqueDest(const std::filesystem::path& dir, const std::string& name) {
    auto path = dir / name;
    if (!std::filesystem::exists(path)) {
      return path;
    }
    const auto stem = path.stem().string();
    const auto ext = path.extension().string();
    for (int i = 1; i < 100; ++i) {
      auto candidate = dir / (stem + "-" + std::to_string(i) + ext);
      if (!std::filesystem::exists(candidate)) {
        return candidate;
      }
    }
    return dir / (stem + "-" + StringUtils::generateUuid() + ext);
  }

  [[nodiscard]] std::string httpStatusText(int status) {
    switch (status) {
    case 200:
      return "OK";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 409:
      return "Conflict";
    case 429:
      return "Too Many Requests";
    default:
      return "Error";
    }
  }

  struct PeerRecord {
    LocalSendPeer peer;
    std::chrono::steady_clock::time_point seen{};
  };

  struct IncomingSession {
    std::string sessionId;
    std::string peerIp;
    std::string alias;
    std::unordered_map<std::string, std::string> tokens;
    std::unordered_map<std::string, localsend::FileMeta> files;
    std::unordered_map<std::string, bool> received;
  };

  struct HttpConn {
    enum class Phase { Headers, Body, Hold, Write, Upload, Done };

    int fd = -1;
    Phase phase = Phase::Headers;
    std::string peerIp;
    std::string in;
    std::string out;
    std::size_t outSent = 0;
    std::string method;
    std::string path;
    std::string query;
    std::uint64_t contentLength = 0;
    std::size_t headerEnd = 0;
    std::string jsonBody;
    FILE* uploadFile = nullptr;
    std::filesystem::path uploadPath;
    std::uint64_t uploadReceived = 0;
    std::string uploadFileId;
    std::chrono::steady_clock::time_point holdUntil{};

    ~HttpConn() {
      closeFd(fd);
      if (uploadFile != nullptr) {
        std::fclose(uploadFile);
        uploadFile = nullptr;
      }
    }

    HttpConn() = default;
    HttpConn(const HttpConn&) = delete;
    HttpConn& operator=(const HttpConn&) = delete;
  };
} // namespace

struct LocalSendService::Impl {
  int udpFd = -1;
  int tcpFd = -1;
  std::string fingerprint;
  std::vector<std::unique_ptr<HttpConn>> conns;
  std::unordered_map<std::string, PeerRecord> peers;
  HttpConn* pendingPrepare = nullptr;
  localsend::PrepareUploadRequest pendingRequest;
  IncomingSession session;
  std::uint64_t sendGeneration = 0;
};

LocalSendService::LocalSendService(ConfigService& config, HttpClient& http, NotificationManager& notifications)
    : m_config(config), m_http(http), m_notifications(notifications), m_impl(std::make_unique<Impl>()) {
  if (auto stored = m_config.stateString("localsend", "fingerprint"); stored && !stored->empty()) {
    m_impl->fingerprint = *stored;
  } else {
    m_impl->fingerprint = StringUtils::generateUuid();
    if (m_impl->fingerprint.empty()) {
      m_impl->fingerprint = "noctalia";
    }
    (void)m_config.setStateString("localsend", "fingerprint", m_impl->fingerprint);
  }
  m_config.addReloadCallback(
      [this]() {
        const auto& cfg = m_config.config().localsend;
        if (!cfg.enabled) {
          if (m_snapshot.enabled) {
            setEnabled(false);
          }
          return;
        }
        if (!m_snapshot.enabled || m_snapshot.port != (cfg.port > 0 ? cfg.port : localsend::kDefaultPort)) {
          setEnabled(true);
          return;
        }
        announce();
      },
      "localsend"
  );
}

LocalSendService::~LocalSendService() {
  m_lifetime.reset();
  m_announceTimer.stop();
  m_expireTimer.stop();
  if (m_impl) {
    m_impl->pendingPrepare = nullptr;
    m_impl->conns.clear();
    closeFd(m_impl->udpFd);
    closeFd(m_impl->tcpFd);
  }
}

void LocalSendService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void LocalSendService::start() {
  m_snapshot.enabled = m_config.config().localsend.enabled;
  if (m_snapshot.enabled) {
    setEnabled(true);
  } else {
    m_snapshot.alias = m_config.config().localsend.alias.empty() ? hostName() : m_config.config().localsend.alias;
    m_snapshot.fingerprint = m_impl->fingerprint;
    m_snapshot.port = m_config.config().localsend.port;
  }
}

localsend::DeviceInfo LocalSendService::deviceInfo() const {
  localsend::DeviceInfo info;
  info.alias = m_snapshot.alias;
  info.deviceModel = distroLabel();
  info.deviceType = "desktop";
  info.fingerprint = m_impl->fingerprint;
  info.port = m_snapshot.port;
  info.protocol = "http";
  info.download = false;
  return info;
}

void LocalSendService::setEnabled(bool enabled) {
  m_snapshot.enabled = enabled;
  m_announceTimer.stop();
  m_expireTimer.stop();
  m_impl->pendingPrepare = nullptr;
  m_impl->conns.clear();
  m_snapshot.incoming.reset();
  closeFd(m_impl->udpFd);
  closeFd(m_impl->tcpFd);
  m_snapshot.listening = false;
  m_snapshot.discoveryReady = false;
  m_snapshot.error.clear();

  const auto& cfg = m_config.config().localsend;
  m_snapshot.alias = cfg.alias.empty() ? hostName() : cfg.alias;
  m_snapshot.port = cfg.port > 0 ? cfg.port : localsend::kDefaultPort;
  m_snapshot.fingerprint = m_impl->fingerprint;
  m_snapshot.ip = localIpv4();

  if (!enabled) {
    if (m_changeCallback) {
      m_changeCallback();
    }
    return;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(m_snapshot.port));
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  const int reuse = 1;

  m_impl->udpFd = makeNonblockSocket(SOCK_DGRAM);
  if (m_impl->udpFd >= 0) {
    (void)::setsockopt(m_impl->udpFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    (void)::setsockopt(m_impl->udpFd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif
    if (::bind(m_impl->udpFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      kLog.warn("udp bind failed: {}", std::strerror(errno));
      closeFd(m_impl->udpFd);
    } else {
      ip_mreq mreq{};
      mreq.imr_multiaddr.s_addr = ::inet_addr(kMulticast);
      mreq.imr_interface.s_addr = htonl(INADDR_ANY);
      if (::setsockopt(m_impl->udpFd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
        kLog.warn("multicast join failed: {}", std::strerror(errno));
      }
      const unsigned char ttl = 1;
      (void)::setsockopt(m_impl->udpFd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
      m_snapshot.discoveryReady = true;
    }
  }

  m_impl->tcpFd = makeNonblockSocket(SOCK_STREAM);
  if (m_impl->tcpFd >= 0) {
    (void)::setsockopt(m_impl->tcpFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (::bind(m_impl->tcpFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(m_impl->tcpFd, 16) != 0) {
      kLog.warn("tcp bind failed: {}", std::strerror(errno));
      closeFd(m_impl->tcpFd);
      m_snapshot.error = i18n::tr("control-center.localsend.port-busy");
    } else {
      m_snapshot.listening = true;
    }
  }

  auto weak = std::weak_ptr<void>(m_lifetime);
  m_announceTimer.start(kAnnounceInterval, [this, weak]() {
    if (weak.lock()) {
      announce();
    }
  });
  m_expireTimer.start(std::chrono::seconds(2), [this, weak]() {
    if (!weak.lock()) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(m_impl->peers, [&](const auto& entry) { return now - entry.second.seen > kPeerTtl; });
    m_snapshot.peers.clear();
    for (const auto& [_, rec] : m_impl->peers) {
      m_snapshot.peers.push_back(rec.peer);
    }
    std::ranges::sort(m_snapshot.peers, {}, &LocalSendPeer::alias);
    if (m_changeCallback) {
      m_changeCallback();
    }
  });
  announce();
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void LocalSendService::announce() {
  if (!m_snapshot.enabled || m_impl->udpFd < 0) {
    return;
  }
  m_snapshot.alias = m_config.config().localsend.alias.empty() ? hostName() : m_config.config().localsend.alias;
  m_snapshot.ip = localIpv4();
  auto info = deviceInfo();
  info.announce = true;
  const std::string payload = localsend::serializeDeviceJson(info, true);
  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(static_cast<std::uint16_t>(m_snapshot.port));
  dest.sin_addr.s_addr = ::inet_addr(kMulticast);
  const auto sent = ::sendto(
      m_impl->udpFd, payload.data(), payload.size(), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)
  );
  if (sent < 0) {
    kLog.debug("announce send failed: {}", std::strerror(errno));
  }
}

namespace {
  void queueHttp(HttpConn& conn, int status, std::string_view contentType, std::string body) {
    conn.out = std::format(
        "HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", status,
        httpStatusText(status), contentType, body.size(), body
    );
    conn.outSent = 0;
    conn.phase = HttpConn::Phase::Write;
  }

  void queueEmpty(HttpConn& conn, int status) { queueHttp(conn, status, "text/plain", {}); }

  [[nodiscard]] bool parseRequestLine(HttpConn& conn) {
    const auto end = conn.in.find("\r\n\r\n");
    if (end == std::string::npos) {
      return false;
    }
    conn.headerEnd = end + 4;
    const auto lineEnd = conn.in.find("\r\n");
    if (lineEnd == std::string::npos) {
      return false;
    }
    const auto line = std::string_view(conn.in).substr(0, lineEnd);
    const auto sp1 = line.find(' ');
    const auto sp2 = sp1 == std::string_view::npos ? std::string_view::npos : line.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
      return false;
    }
    conn.method = std::string(line.substr(0, sp1));
    const auto target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const auto q = target.find('?');
    conn.path = std::string(q == std::string_view::npos ? target : target.substr(0, q));
    conn.query = std::string(q == std::string_view::npos ? std::string_view{} : target.substr(q + 1));
    conn.contentLength = 0;
    std::size_t cursor = lineEnd + 2;
    while (cursor < end) {
      const auto next = conn.in.find("\r\n", cursor);
      if (next == std::string::npos || next > end) {
        break;
      }
      const auto header = std::string_view(conn.in).substr(cursor, next - cursor);
      const auto colon = header.find(':');
      if (colon != std::string_view::npos) {
        auto name = StringUtils::toLower(StringUtils::trim(header.substr(0, colon)));
        auto value = StringUtils::trim(header.substr(colon + 1));
        if (name == "content-length") {
          try {
            conn.contentLength = static_cast<std::uint64_t>(std::stoull(std::string(value)));
          } catch (...) {
            conn.contentLength = 0;
          }
        }
      }
      cursor = next + 2;
    }
    return true;
  }
} // namespace

std::string LocalSendService::filesLabel(std::size_t count, std::string_view singleName) const {
  if (count <= 1) {
    return std::string(singleName);
  }
  return i18n::tr("control-center.localsend.files-count", "count", count);
}

void LocalSendService::pushHistory(LocalSendHistoryItem item) {
  m_snapshot.history.insert(m_snapshot.history.begin(), std::move(item));
  constexpr std::size_t kMaxHistory = 8;
  if (m_snapshot.history.size() > kMaxHistory) {
    m_snapshot.history.resize(kMaxHistory);
  }
}

void LocalSendService::noteIncomingProgress(const std::string& fileId) {
  if (m_impl->session.sessionId.empty()) {
    return;
  }
  std::size_t done = 0;
  for (const auto& [id, received] : m_impl->session.received) {
    if (received) {
      ++done;
    }
  }
  std::string name;
  auto fileIt = m_impl->session.files.find(fileId);
  if (fileIt != m_impl->session.files.end()) {
    name = fileIt->second.fileName;
  }
  m_snapshot.transfer = LocalSendTransfer{
      .label = name,
      .peerAlias = m_impl->session.alias,
      .incoming = true,
      .fileIndex = done + 1,
      .fileCount = m_impl->session.files.size(),
  };
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void LocalSendService::sendPath(std::string peerKey, std::filesystem::path path, std::string pin) {
  const auto collected = localsend::collectSendFiles(path);
  if (collected.files.empty()) {
    m_snapshot.transfer = LocalSendTransfer{
        .label = {},
        .peerAlias = {},
        .incoming = false,
        .failed = true,
        .error = i18n::tr("control-center.localsend.folder-empty"),
    };
    if (m_changeCallback) {
      m_changeCallback();
    }
    return;
  }
  sendFiles(std::move(peerKey), std::move(collected.files), std::move(pin));
}

void LocalSendService::sendFiles(std::string peerKey, std::vector<std::filesystem::path> paths, std::string pin) {
  if (paths.empty()) {
    return;
  }
  auto it = m_impl->peers.find(peerKey);
  if (it == m_impl->peers.end()) {
    m_snapshot.transfer = LocalSendTransfer{.label = {}, .peerAlias = {}, .incoming = false, .failed = true,
                                            .error = i18n::tr("control-center.localsend.peer-gone")};
    if (m_changeCallback) {
      m_changeCallback();
    }
    return;
  }
  const LocalSendPeer peer = it->second.peer;
  localsend::PrepareUploadRequest request;
  request.info = deviceInfo();
  std::vector<std::filesystem::path> existing;
  for (const auto& path : paths) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
      continue;
    }
    localsend::FileMeta file;
    file.id = StringUtils::generateUuid();
    if (file.id.empty()) {
      file.id = path.filename().string();
    }
    file.fileName = localsend::sanitizeFileName(path.filename().string());
    file.size = static_cast<std::uint64_t>(std::filesystem::file_size(path, ec));
    file.fileType = localsend::mimeFromFileName(file.fileName);
    request.files.push_back(file);
    existing.push_back(path);
  }
  if (existing.empty()) {
    return;
  }

  m_snapshot.busy = true;
  m_snapshot.transfer = LocalSendTransfer{
      .label = filesLabel(existing.size(), existing.front().filename().string()),
      .peerAlias = peer.alias,
      .incoming = false,
      .fileIndex = 1,
      .fileCount = existing.size(),
  };
  if (m_changeCallback) {
    m_changeCallback();
  }
  prepareAndSend(peer, std::move(request), std::move(existing), std::move(pin), 0, ++m_impl->sendGeneration);
}

void LocalSendService::noteIncomingFileDone(const std::string& fileId) {
  if (m_impl->session.sessionId.empty()) {
    return;
  }
  m_impl->session.received[fileId] = true;
  bool all = true;
  for (const auto& [id, rec] : m_impl->session.received) {
    all = all && rec;
  }
  if (!all) {
    return;
  }
  if (m_config.config().localsend.notifyOnReceive) {
    m_notifications.addInternal(
        "Helix", i18n::tr("control-center.localsend.title"),
        i18n::tr("control-center.localsend.received", "name", m_impl->session.alias)
    );
  }
  const std::string label = filesLabel(
      m_impl->session.files.size(),
      m_impl->session.files.empty() ? std::string{} : m_impl->session.files.begin()->second.fileName
  );
  pushHistory(LocalSendHistoryItem{
      .alias = m_impl->session.alias,
      .label = label,
      .incoming = true,
      .failed = false,
  });
  m_snapshot.transfer.reset();
  m_impl->session = {};
}

void LocalSendService::finishSendFailure(std::string error, std::uint64_t generation) {
  if (generation != m_impl->sendGeneration) {
    return;
  }
  m_snapshot.busy = false;
  if (m_snapshot.transfer) {
    m_snapshot.transfer->failed = true;
    m_snapshot.transfer->error = std::move(error);
    pushHistory(LocalSendHistoryItem{
        .alias = m_snapshot.transfer->peerAlias,
        .label = m_snapshot.transfer->label,
        .incoming = false,
        .failed = true,
    });
  }
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void LocalSendService::prepareAndSend(
    LocalSendPeer peer, localsend::PrepareUploadRequest request, std::vector<std::filesystem::path> paths,
    std::string pin, std::size_t protoIndex, std::uint64_t generation
) {
  const std::vector<std::string> protocols = peer.protocol == "https" ? std::vector<std::string>{"https", "http"}
                                                                     : std::vector<std::string>{"http", "https"};
  if (generation != m_impl->sendGeneration) {
    return;
  }
  if (protoIndex >= protocols.size()) {
    finishSendFailure(i18n::tr("control-center.localsend.send-failed"), generation);
    return;
  }
  std::string pinQuery;
  if (!pin.empty()) {
    pinQuery = "?pin=" + pin;
  }
  HttpRequest req;
  req.method = "POST";
  req.url = std::format(
      "{}://{}:{}{}/prepare-upload{}", protocols[protoIndex], peer.ip, peer.port, localsend::kApiPrefixV2, pinQuery
  );
  req.body = localsend::serializePrepareUploadRequestJson(request);
  req.headers = {"Content-Type: application/json", "Expect:"};
  req.allowInsecureTls = true;
  req.allowWhenOffline = true;
  req.timeoutSeconds = 120;
  auto weak = std::weak_ptr<void>(m_lifetime);
  m_http.request(
      std::move(req),
      [this, weak, generation, peer, request, paths, pin, protoIndex, protocol = protocols[protoIndex]](
          HttpResponse response
      ) {
        if (!weak.lock() || generation != m_impl->sendGeneration) {
          return;
        }
        if (!response.transportOk || (response.status != 200 && response.status != 204)) {
          if (response.status == 401) {
            finishSendFailure(i18n::tr("control-center.localsend.pin-required"), generation);
            return;
          }
          if (response.status == 403) {
            finishSendFailure(i18n::tr("control-center.localsend.rejected"), generation);
            return;
          }
          prepareAndSend(peer, request, paths, pin, protoIndex + 1, generation);
          return;
        }
        if (response.status == 204) {
          m_snapshot.busy = false;
          m_snapshot.transfer.reset();
          if (m_changeCallback) {
            m_changeCallback();
          }
          return;
        }
        const auto prepared = localsend::parsePrepareUploadResponseJson(response.body);
        if (!prepared) {
          finishSendFailure(i18n::tr("control-center.localsend.send-failed"), generation);
          return;
        }
        uploadRemaining(peer, request, paths, *prepared, protocol, 0, generation);
      }
  );
}

void LocalSendService::uploadRemaining(
    LocalSendPeer peer, localsend::PrepareUploadRequest request, std::vector<std::filesystem::path> paths,
    localsend::PrepareUploadResponse prepared, std::string protocol, std::size_t index, std::uint64_t generation
) {
  if (generation != m_impl->sendGeneration) {
    return;
  }
  if (index >= request.files.size()) {
    const std::string label = filesLabel(
        request.files.size(), request.files.empty() ? std::string{} : request.files.front().fileName
    );
    pushHistory(LocalSendHistoryItem{
        .alias = peer.alias,
        .label = label,
        .incoming = false,
        .failed = false,
    });
    m_snapshot.busy = false;
    m_snapshot.transfer.reset();
    m_notifications.addInternal(
        "Helix", i18n::tr("control-center.localsend.title"),
        i18n::tr("control-center.localsend.sent", "name", peer.alias)
    );
    if (m_changeCallback) {
      m_changeCallback();
    }
    return;
  }
  const auto& file = request.files[index];
  if (m_snapshot.transfer) {
    m_snapshot.transfer->fileIndex = index + 1;
    m_snapshot.transfer->fileCount = request.files.size();
    m_snapshot.transfer->label = file.fileName;
    if (m_changeCallback) {
      m_changeCallback();
    }
  }
  const auto tokenIt = prepared.fileTokens.find(file.id);
  if (tokenIt == prepared.fileTokens.end()) {
    uploadRemaining(peer, std::move(request), std::move(paths), std::move(prepared), std::move(protocol), index + 1, generation);
    return;
  }
  HttpRequest up;
  up.method = "POST";
  up.url = std::format(
      "{}://{}:{}/api/localsend/v2/upload?sessionId={}&fileId={}&token={}", protocol, peer.ip, peer.port,
      prepared.sessionId, file.id, tokenIt->second
  );
  up.uploadPath = paths[index];
  up.headers = {"Content-Type: application/octet-stream", "Expect:"};
  up.allowInsecureTls = true;
  up.allowWhenOffline = true;
  up.timeoutSeconds = 0;
  auto weak = std::weak_ptr<void>(m_lifetime);
  m_http.request(
      std::move(up),
      [this, weak, generation, peer, request = std::move(request), paths = std::move(paths),
       prepared = std::move(prepared), protocol = std::move(protocol), index](HttpResponse upRes) mutable {
        if (!weak.lock() || generation != m_impl->sendGeneration) {
          return;
        }
        if (!upRes.transportOk || (upRes.status != 200 && upRes.status != 204)) {
          finishSendFailure(i18n::tr("control-center.localsend.send-failed"), generation);
          return;
        }
        uploadRemaining(
            peer, std::move(request), std::move(paths), std::move(prepared), std::move(protocol), index + 1, generation
        );
      }
  );
}

void LocalSendService::sendText(std::string peerKey, std::string text, std::string pin) {
  if (text.empty()) {
    return;
  }
  const auto dir = std::filesystem::path(FileUtils::stateDir()) / "localsend";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const auto path = dir / "clipboard.txt";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return;
  }
  out << text;
  out.close();
  sendFiles(std::move(peerKey), {path}, std::move(pin));
}

void LocalSendService::acceptIncoming() {
  if (m_impl->pendingPrepare == nullptr) {
    return;
  }
  localsend::PrepareUploadResponse response;
  response.sessionId = StringUtils::generateUuid();
  if (response.sessionId.empty()) {
    response.sessionId = "session";
  }
  IncomingSession session;
  session.sessionId = response.sessionId;
  session.peerIp = m_impl->pendingPrepare->peerIp;
  session.alias = m_impl->pendingRequest.info.alias;
  for (const auto& file : m_impl->pendingRequest.files) {
    const auto token = StringUtils::generateUuid();
    response.fileTokens[file.id] = token.empty() ? file.id : token;
    session.tokens[file.id] = response.fileTokens[file.id];
    session.files[file.id] = file;
    session.received[file.id] = false;
  }
  m_impl->session = std::move(session);
  queueHttp(*m_impl->pendingPrepare, 200, "application/json", localsend::serializePrepareUploadResponseJson(response));
  m_impl->pendingPrepare = nullptr;
  m_snapshot.incoming.reset();
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void LocalSendService::rejectIncoming() {
  if (m_impl->pendingPrepare != nullptr) {
    queueEmpty(*m_impl->pendingPrepare, 403);
    m_impl->pendingPrepare = nullptr;
  }
  m_snapshot.incoming.reset();
  m_impl->pendingRequest = {};
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void LocalSendService::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::localsendToggle, [this](const std::string&) -> std::string {
    const bool next = !m_snapshot.enabled;
    (void)m_config.setOverride({"localsend", "enabled"}, next);
    return "ok\n";
  });
  ipc.bind(noctalia::cli::msg::localsendAnnounce, [this](const std::string&) -> std::string {
    announce();
    return "ok\n";
  });
  ipc.bind(noctalia::cli::msg::localsendStatus, [this](const std::string&) -> std::string {
    if (!m_snapshot.enabled) {
      return "disabled\n";
    }
    return std::format(
        "{} alias={} port={} peers={}\n", m_snapshot.listening ? "listening" : "receive-blocked", m_snapshot.alias,
        m_snapshot.port, m_snapshot.peers.size()
    );
  });
}

int LocalSendService::pollTimeoutMs() const {
  if (m_impl->pendingPrepare != nullptr) {
    return 250;
  }
  return m_impl->udpFd >= 0 || m_impl->tcpFd >= 0 || !m_impl->conns.empty() ? 500 : -1;
}

void LocalSendService::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_impl->udpFd >= 0) {
    fds.push_back(pollfd{.fd = m_impl->udpFd, .events = POLLIN, .revents = 0});
  }
  if (m_impl->tcpFd >= 0) {
    fds.push_back(pollfd{.fd = m_impl->tcpFd, .events = POLLIN, .revents = 0});
  }
  for (auto& conn : m_impl->conns) {
    short events = POLLIN;
    if (conn->phase == HttpConn::Phase::Write) {
      events = POLLOUT;
    } else if (conn->phase == HttpConn::Phase::Hold) {
      events = POLLIN | POLLHUP;
    }
    fds.push_back(pollfd{.fd = conn->fd, .events = events, .revents = 0});
  }
}

void LocalSendService::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  std::size_t idx = startIdx;
  const auto take = [&](int fd) -> short {
    if (fd < 0 || idx >= fds.size() || fds[idx].fd != fd) {
      return 0;
    }
    return fds[idx++].revents;
  };

  if (const short revents = take(m_impl->udpFd); (revents & POLLIN) != 0) {
    for (;;) {
      char buf[2048];
      sockaddr_in src{};
      socklen_t srcLen = sizeof(src);
      const auto n = ::recvfrom(m_impl->udpFd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&src), &srcLen);
      if (n <= 0) {
        break;
      }
      buf[n] = 0;
      const auto info = localsend::parseDeviceJson(std::string_view(buf, static_cast<std::size_t>(n)));
      if (!info || info->fingerprint == m_impl->fingerprint) {
        continue;
      }
      char ipBuf[INET_ADDRSTRLEN]{};
      ::inet_ntop(AF_INET, &src.sin_addr, ipBuf, sizeof(ipBuf));
      LocalSendPeer peer;
      peer.key = localsend::peerKey(ipBuf, info->port, info->fingerprint);
      peer.alias = info->alias;
      peer.ip = ipBuf;
      peer.port = info->port;
      peer.protocol = info->protocol;
      peer.deviceType = info->deviceType;
      peer.deviceModel = info->deviceModel;
      peer.fingerprint = info->fingerprint;
      peer.download = info->download;
      m_impl->peers[peer.key] = PeerRecord{.peer = peer, .seen = std::chrono::steady_clock::now()};
      if (info->announce) {
        HttpRequest req;
        req.method = "POST";
        req.url = std::format("{}://{}:{}/api/localsend/v2/register", info->protocol, peer.ip, peer.port);
        auto ours = deviceInfo();
        req.body = localsend::serializeDeviceJson(ours, false);
        req.headers = {"Content-Type: application/json", "Expect:"};
        req.allowInsecureTls = true;
        req.allowWhenOffline = true;
        auto weak = std::weak_ptr<void>(m_lifetime);
        m_http.request(std::move(req), [this, weak](HttpResponse response) {
          if (!weak.lock() || !response.transportOk || response.body.empty()) {
            return;
          }
          const auto reply = localsend::parseDeviceJson(response.body);
          if (!reply || reply->fingerprint == m_impl->fingerprint) {
            return;
          }
        });
        auto replyInfo = deviceInfo();
        replyInfo.announce = false;
        const auto payload = localsend::serializeDeviceJson(replyInfo, true);
        sockaddr_in dest = src;
        dest.sin_port = htons(static_cast<std::uint16_t>(info->port));
        (void)::sendto(
            m_impl->udpFd, payload.data(), payload.size(), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)
        );
      }
    }
    m_snapshot.peers.clear();
    for (const auto& [_, rec] : m_impl->peers) {
      m_snapshot.peers.push_back(rec.peer);
    }
    std::ranges::sort(m_snapshot.peers, {}, &LocalSendPeer::alias);
    if (m_changeCallback) {
      m_changeCallback();
    }
  }

  if (const short revents = take(m_impl->tcpFd); (revents & POLLIN) != 0) {
    for (;;) {
      sockaddr_in src{};
      socklen_t srcLen = sizeof(src);
      const int fd = ::accept4(m_impl->tcpFd, reinterpret_cast<sockaddr*>(&src), &srcLen, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd < 0) {
        break;
      }
      if (m_impl->conns.size() >= kMaxConns) {
        ::close(fd);
        continue;
      }
      auto conn = std::make_unique<HttpConn>();
      conn->fd = fd;
      char ipBuf[INET_ADDRSTRLEN]{};
      ::inet_ntop(AF_INET, &src.sin_addr, ipBuf, sizeof(ipBuf));
      conn->peerIp = ipBuf;
      m_impl->conns.push_back(std::move(conn));
    }
  }

  const auto now = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < m_impl->conns.size();) {
    auto& conn = *m_impl->conns[i];
    const short revents = take(conn.fd);
    bool drop = conn.phase == HttpConn::Phase::Done || conn.fd < 0;
    if (conn.phase == HttpConn::Phase::Hold && now > conn.holdUntil) {
      queueEmpty(conn, 403);
      if (m_impl->pendingPrepare == &conn) {
        m_impl->pendingPrepare = nullptr;
        m_snapshot.incoming.reset();
      }
    }
    if ((revents & (POLLHUP | POLLERR)) != 0 && conn.phase != HttpConn::Phase::Write) {
      drop = true;
    }
    if (!drop && (revents & POLLIN) != 0 && conn.phase != HttpConn::Phase::Write) {
      char buf[16 * 1024];
      const auto n = ::recv(conn.fd, buf, sizeof(buf), 0);
      if (n == 0) {
        drop = true;
      } else if (n > 0) {
        if (conn.phase == HttpConn::Phase::Upload && conn.uploadFile != nullptr) {
          const auto wrote = std::fwrite(buf, 1, static_cast<std::size_t>(n), conn.uploadFile);
          conn.uploadReceived += wrote;
          if (conn.uploadReceived >= conn.contentLength) {
            std::fclose(conn.uploadFile);
            conn.uploadFile = nullptr;
            noteIncomingFileDone(conn.uploadFileId);
            queueEmpty(conn, 200);
          }
        } else {
          conn.in.append(buf, static_cast<std::size_t>(n));
          if (conn.phase == HttpConn::Phase::Headers) {
            if (conn.in.size() > kMaxHeaderBytes) {
              drop = true;
            } else if (parseRequestLine(conn)) {
              if (conn.contentLength == 0) {
                // handled below
              } else if (conn.method == "POST" && localsend::isLocalSendApiPath(conn.path, "/upload")) {
                conn.phase = HttpConn::Phase::Upload;
              } else {
                conn.phase = HttpConn::Phase::Body;
              }
            }
          }
        }
      }
    }

    if (!drop && conn.phase == HttpConn::Phase::Headers && conn.headerEnd > 0 && conn.contentLength == 0) {
      // request fully read
    }

    auto handleComplete = [&]() {
      const auto params = localsend::parseQuery(conn.query);
      if (conn.method == "GET" && localsend::isLocalSendApiPath(conn.path, "/info")) {
        queueHttp(conn, 200, "application/json", localsend::serializeDeviceJson(deviceInfo(), false));
        return;
      }
      if (conn.method == "POST" && localsend::isLocalSendApiPath(conn.path, "/register")) {
        if (const auto info = localsend::parseDeviceJson(conn.jsonBody)) {
          if (info->fingerprint != m_impl->fingerprint) {
            LocalSendPeer peer;
            peer.key = localsend::peerKey(conn.peerIp, info->port, info->fingerprint);
            peer.alias = info->alias;
            peer.ip = conn.peerIp;
            peer.port = info->port;
            peer.protocol = info->protocol;
            peer.deviceType = info->deviceType;
            peer.deviceModel = info->deviceModel;
            peer.fingerprint = info->fingerprint;
            m_impl->peers[peer.key] = PeerRecord{.peer = peer, .seen = std::chrono::steady_clock::now()};
          }
        }
        queueHttp(conn, 200, "application/json", localsend::serializeDeviceJson(deviceInfo(), false));
        return;
      }
      if (conn.method == "POST" && conn.path == "/api/localsend/v2/cancel") {
        if (!m_impl->session.sessionId.empty() && params.sessionId == m_impl->session.sessionId) {
          m_impl->session = {};
        }
        if (m_impl->pendingPrepare != nullptr) {
          rejectIncoming();
        }
        queueEmpty(conn, 200);
        return;
      }
      if (conn.method == "POST" && conn.path == "/api/localsend/v2/prepare-upload") {
        const auto pin = StringUtils::trim(m_config.config().localsend.pin);
        if (!pin.empty() && params.pin != pin) {
          queueEmpty(conn, 401);
          return;
        }
        if (m_impl->pendingPrepare != nullptr || !m_impl->session.sessionId.empty()) {
          queueEmpty(conn, 409);
          return;
        }
        auto prepared = localsend::parsePrepareUploadJson(conn.jsonBody);
        if (!prepared) {
          queueEmpty(conn, 400);
          return;
        }
        if (m_config.config().localsend.autoAccept) {
          m_impl->pendingRequest = std::move(*prepared);
          m_impl->pendingPrepare = &conn;
          acceptIncoming();
          return;
        }
        m_impl->pendingRequest = std::move(*prepared);
        m_impl->pendingPrepare = &conn;
        conn.phase = HttpConn::Phase::Hold;
        conn.holdUntil = std::chrono::steady_clock::now() + kIncomingTimeout;
        LocalSendIncoming incoming;
        incoming.alias = m_impl->pendingRequest.info.alias;
        incoming.ip = conn.peerIp;
        for (const auto& file : m_impl->pendingRequest.files) {
          incoming.files.push_back(LocalSendIncomingFile{.id = file.id, .fileName = file.fileName, .size = file.size});
        }
        m_snapshot.incoming = incoming;
        if (m_config.config().localsend.notifyOnReceive) {
          m_notifications.addInternal(
              "Helix", i18n::tr("control-center.localsend.title"),
              i18n::tr("control-center.localsend.incoming", "name", incoming.alias)
          );
        }
        if (m_changeCallback) {
          m_changeCallback();
        }
        return;
      }
      if (conn.method == "POST" && conn.path == "/api/localsend/v2/upload") {
        if (m_impl->session.sessionId.empty() || params.sessionId != m_impl->session.sessionId
            || m_impl->session.tokens[params.fileId] != params.token) {
          queueEmpty(conn, 403);
          return;
        }
        auto fileIt = m_impl->session.files.find(params.fileId);
        if (fileIt == m_impl->session.files.end()) {
          queueEmpty(conn, 403);
          return;
        }
        auto dir = FileUtils::expandUserPath(
            m_config.config().localsend.downloadDir.empty() ? "~/Downloads" : m_config.config().localsend.downloadDir
        );
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        conn.uploadPath = uniqueDest(dir, localsend::sanitizeFileName(fileIt->second.fileName));
        conn.uploadFile = std::fopen(conn.uploadPath.c_str(), "wb");
        conn.uploadFileId = params.fileId;
        conn.contentLength = fileIt->second.size;
        if (conn.uploadFile == nullptr) {
          queueEmpty(conn, 500);
          return;
        }
        noteIncomingProgress(params.fileId);
        const auto leftover = conn.in.size() > conn.headerEnd ? conn.in.size() - conn.headerEnd : 0;
        if (leftover > 0) {
          const auto wrote = std::fwrite(conn.in.data() + conn.headerEnd, 1, leftover, conn.uploadFile);
          conn.uploadReceived = wrote;
        }
        if (conn.uploadReceived >= conn.contentLength) {
          std::fclose(conn.uploadFile);
          conn.uploadFile = nullptr;
          noteIncomingFileDone(params.fileId);
          queueEmpty(conn, 200);
        } else {
          conn.phase = HttpConn::Phase::Upload;
        }
        return;
      }
      queueEmpty(conn, 404);
    };

    if (!drop && conn.phase == HttpConn::Phase::Body) {
      const auto have = conn.in.size() > conn.headerEnd ? conn.in.size() - conn.headerEnd : 0;
      if (have >= conn.contentLength) {
        if (conn.contentLength > kMaxJsonBody) {
          drop = true;
        } else {
          conn.jsonBody = conn.in.substr(conn.headerEnd, static_cast<std::size_t>(conn.contentLength));
          handleComplete();
        }
      }
    } else if (!drop && conn.phase == HttpConn::Phase::Headers && conn.headerEnd > 0 && conn.contentLength == 0) {
      handleComplete();
    } else if (!drop && conn.phase == HttpConn::Phase::Upload && conn.uploadFile == nullptr && conn.headerEnd > 0) {
      handleComplete();
    }

    if (!drop && conn.phase == HttpConn::Phase::Write && (revents & POLLOUT) != 0) {
      const auto remain = conn.out.size() - conn.outSent;
      const auto n = ::send(conn.fd, conn.out.data() + conn.outSent, remain, MSG_NOSIGNAL);
      if (n > 0) {
        conn.outSent += static_cast<std::size_t>(n);
        if (conn.outSent >= conn.out.size()) {
          drop = true;
        }
      } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        drop = true;
      }
    }

    if (drop) {
      if (m_impl->pendingPrepare == &conn) {
        m_impl->pendingPrepare = nullptr;
        m_snapshot.incoming.reset();
      }
      m_impl->conns.erase(m_impl->conns.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
}
