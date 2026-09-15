#pragma once

#include "app/poll_source.h"
#include "core/timer_manager.h"
#include "net/localsend_protocol.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ConfigService;
class HttpClient;
class IpcService;
class NotificationManager;

struct LocalSendPeer {
  std::string key;
  std::string alias;
  std::string ip;
  int port = localsend::kDefaultPort;
  std::string protocol = "http";
  std::string deviceType = "desktop";
  std::string deviceModel;
  std::string fingerprint;
  bool download = false;
};

struct LocalSendIncomingFile {
  std::string id;
  std::string fileName;
  std::uint64_t size = 0;
};

struct LocalSendIncoming {
  std::string alias;
  std::string ip;
  std::vector<LocalSendIncomingFile> files;
};

struct LocalSendTransfer {
  std::string label;
  std::string peerAlias;
  bool incoming = false;
  bool failed = false;
  std::string error;
};

struct LocalSendSnapshot {
  bool enabled = true;
  bool listening = false;
  bool discoveryReady = false;
  bool busy = false;
  std::string alias;
  std::string fingerprint;
  std::string ip;
  int port = localsend::kDefaultPort;
  std::string error;
  std::vector<LocalSendPeer> peers;
  std::optional<LocalSendIncoming> incoming;
  std::optional<LocalSendTransfer> transfer;
};

class LocalSendService : public PollSource {
public:
  using ChangeCallback = std::function<void()>;

  LocalSendService(ConfigService& config, HttpClient& http, NotificationManager& notifications);
  ~LocalSendService() override;

  LocalSendService(const LocalSendService&) = delete;
  LocalSendService& operator=(const LocalSendService&) = delete;

  [[nodiscard]] const LocalSendSnapshot& snapshot() const noexcept { return m_snapshot; }
  [[nodiscard]] bool enabled() const noexcept { return m_snapshot.enabled; }
  [[nodiscard]] bool listening() const noexcept { return m_snapshot.listening; }

  void setChangeCallback(ChangeCallback callback);
  void start();
  void announce();
  void setEnabled(bool enabled);
  void sendFiles(std::string peerKey, std::vector<std::filesystem::path> paths, std::string pin = {});
  void sendText(std::string peerKey, std::string text, std::string pin = {});
  void acceptIncoming();
  void rejectIncoming();
  void registerIpc(IpcService& ipc);

  [[nodiscard]] int pollTimeoutMs() const override;
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  struct Impl;
  [[nodiscard]] localsend::DeviceInfo deviceInfo() const;
  void prepareAndSend(
      LocalSendPeer peer, localsend::PrepareUploadRequest request, std::vector<std::filesystem::path> paths,
      std::string pin, std::size_t protoIndex, std::uint64_t generation
  );
  void uploadRemaining(
      LocalSendPeer peer, localsend::PrepareUploadRequest request, std::vector<std::filesystem::path> paths,
      localsend::PrepareUploadResponse prepared, std::string protocol, std::size_t index, std::uint64_t generation
  );
  void finishSendFailure(std::string error, std::uint64_t generation);
  void noteIncomingFileDone(const std::string& fileId);

  ConfigService& m_config;
  HttpClient& m_http;
  NotificationManager& m_notifications;
  LocalSendSnapshot m_snapshot;
  ChangeCallback m_changeCallback;
  Timer m_announceTimer;
  Timer m_expireTimer;
  std::shared_ptr<void> m_lifetime = std::make_shared<int>(0);
  std::unique_ptr<Impl> m_impl;
};
