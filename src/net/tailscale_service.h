#pragma once

#include "core/timer_manager.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class ConfigService;
class IpcService;

namespace tailscale {
}

struct TailscalePeer {
  std::string id;
  std::string name;
  std::string dnsName;
  std::string ipv4;
  std::string os;
  std::string relay;
  bool online = false;
  bool active = false;
  bool exitNodeOption = false;
  bool isExitNode = false;
};

struct TailscaleExitNode {
  std::string id;
  std::string name;
  std::string ip;
  bool online = false;
  bool selected = false;
};

struct TailscaleSnapshot {
  bool installed = false;
  bool running = false;
  bool busy = false;
  std::string backendState;
  std::string hostname;
  std::string dnsName;
  std::string ipv4;
  std::string ipv6;
  std::string tailnet;
  std::string version;
  std::string authUrl;
  std::string exitNode;
  bool exitNodeOnline = false;
  bool shieldsUp = false;
  bool acceptRoutes = false;
  bool runSsh = false;
  bool acceptDns = false;
  bool advertiseExitNode = false;
  bool exitNodeAllowLan = false;
  std::vector<std::string> health;
  std::vector<TailscalePeer> peers;
  std::vector<TailscaleExitNode> exitNodes;
  int onlineCount = 0;
  int peerCount = 0;
  std::string error;
};

class TailscaleService {
public:
  using ChangeCallback = std::function<void()>;

  explicit TailscaleService(ConfigService& config);
  ~TailscaleService();

  TailscaleService(const TailscaleService&) = delete;
  TailscaleService& operator=(const TailscaleService&) = delete;

  [[nodiscard]] const TailscaleSnapshot& snapshot() const noexcept { return m_snapshot; }
  [[nodiscard]] bool installed() const noexcept { return m_snapshot.installed; }
  [[nodiscard]] bool running() const noexcept { return m_snapshot.running; }

  void setChangeCallback(ChangeCallback callback);
  void start();
  void refresh();
  void setWantRunning(bool up);
  void toggle();
  void setShieldsUp(bool enabled);
  void setAcceptRoutes(bool enabled);
  void setSsh(bool enabled);
  void setAcceptDns(bool enabled);
  void setAdvertiseExitNode(bool enabled);
  void setAllowLan(bool enabled);
  void setExitNode(std::string node);
  void openAdmin();
  void setAuthPrep(std::function<void(std::function<void()>)> prep);

  void registerIpc(IpcService& ipc);

private:
  void schedulePoll();
  void runCli(
      std::vector<std::string> args, std::function<void(bool ok, std::string out, std::string err)> onDone,
      bool allowEscalate = true
  );
  void finishWrite(bool ok, std::string err);
  void applyStatusJson(std::string jsonText);
  void applyPrefsJson(std::string jsonText);
  void finishRefresh(std::string statusJson, std::string prefsJson, std::string error);
  void notifyPeerChanges(const TailscaleSnapshot& previous, const TailscaleSnapshot& next);
  void setBusy(bool busy);
  void notifyChanged();
  [[nodiscard]] int pollIntervalSeconds() const;
  [[nodiscard]] std::string adminUrl() const;

  ConfigService& m_config;
  TailscaleSnapshot m_snapshot;
  ChangeCallback m_changeCallback;
  std::function<void(std::function<void()>)> m_authPrep;
  Timer m_pollTimer;
  std::uint64_t m_generation = 0;
  std::shared_ptr<void> m_lifetime = std::make_shared<int>(0);
  bool m_refreshInFlight = false;
  std::string m_pendingCommandError;
};
