#pragma once

#include "core/timer_manager.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class HttpClient;
class IpcService;

struct SyncthingFolder {
  std::string id;
  std::string label;
  std::string path;
  std::string type;
  std::string state;
  bool paused = false;
  double completion = 100.0;
  std::int64_t needBytes = 0;

  bool operator==(const SyncthingFolder&) const = default;
};

struct SyncthingRemote {
  std::string id;
  std::string name;
  bool connected = false;
  bool paused = false;

  bool operator==(const SyncthingRemote&) const = default;
};

struct SyncthingSnapshot {
  bool installed = false;
  bool running = false;
  bool busy = false;
  std::string myId;
  std::string myName;
  std::string guiUrl;
  std::vector<SyncthingFolder> folders;
  std::vector<SyncthingRemote> remotes;
  int connectedCount = 0;
  int folderCount = 0;
  std::string error;

  bool operator==(const SyncthingSnapshot&) const = default;
};

class SyncthingService {
public:
  using ChangeCallback = std::function<void()>;

  explicit SyncthingService(HttpClient& http);
  ~SyncthingService();

  SyncthingService(const SyncthingService&) = delete;
  SyncthingService& operator=(const SyncthingService&) = delete;

  [[nodiscard]] const SyncthingSnapshot& snapshot() const noexcept { return m_snapshot; }
  [[nodiscard]] bool installed() const noexcept { return m_snapshot.installed; }
  [[nodiscard]] bool running() const noexcept { return m_snapshot.running; }

  void setChangeCallback(ChangeCallback callback);
  void start();
  void refresh();
  void tryStart();
  void shutdown();
  void openGui();
  void setFolderPaused(const std::string& folderId, bool paused);
  void scanFolder(const std::string& folderId);
  void setWantFastPoll(bool enabled);

  void registerIpc(IpcService& ipc);

private:
  struct GuiAuth {
    std::string apiKey;
    std::string origin;
    bool tls = false;
  };

  void schedulePoll();
  void notifyChanged();
  void finishRefresh(SyncthingSnapshot next);
  void requestJson(
      std::string_view method, std::string path, std::string body, std::function<void(bool ok, std::string body)> onDone
  );
  void loadGuiAuth();
  [[nodiscard]] std::string restUrl(std::string_view path) const;
  void applyStatusJson(SyncthingSnapshot& next, std::string_view body);
  void applyConfigJson(SyncthingSnapshot& next, std::string_view body);
  void applyConnectionsJson(SyncthingSnapshot& next, std::string_view body);
  void applyFolderStatusJson(SyncthingFolder& folder, std::string_view body);
  void fetchFolderStatuses(SyncthingSnapshot next, std::uint64_t generation);

  HttpClient& m_http;
  SyncthingSnapshot m_snapshot;
  ChangeCallback m_changeCallback;
  Timer m_pollTimer;
  Timer m_retryTimer;
  GuiAuth m_auth;
  std::uint64_t m_generation = 0;
  std::shared_ptr<void> m_lifetime = std::make_shared<int>(0);
  bool m_refreshInFlight = false;
  bool m_fastPoll = false;
};
