#pragma once

#include "core/timer_manager.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class INetworkService;
class IpcService;
class SessionBus;

struct CastSink {
  std::string uuid;
  std::string name;
  std::string protocol;
  std::string state;
  std::string unit;

  bool operator==(const CastSink&) const = default;
};

struct CastSnapshot {
  bool shareInstalled = false;
  bool shareEnabled = false;
  bool shareBusy = false;
  bool viewOnly = true;
  int sharePort = 3389;
  std::string shareAddress;
  std::string username;
  std::string password;
  bool wirelessInstalled = false;
  bool wirelessRunning = false;
  bool wirelessBusy = false;
  std::vector<CastSink> sinks;
  std::string error;

  bool operator==(const CastSnapshot&) const = default;
};

class CastService {
public:
  using ChangeCallback = std::function<void()>;

  CastService(SessionBus* bus, INetworkService* network);
  ~CastService();

  CastService(const CastService&) = delete;
  CastService& operator=(const CastService&) = delete;

  [[nodiscard]] const CastSnapshot& snapshot() const noexcept { return m_snapshot; }
  [[nodiscard]] bool sharing() const noexcept { return m_snapshot.shareEnabled; }

  void setChangeCallback(ChangeCallback callback);
  void start();
  void refresh();
  void setWantFastPoll(bool enabled);
  void setShareEnabled(bool enabled);
  void setViewOnly(bool enabled);
  void tryStartWireless();
  void openWirelessApp();
  void startStream(const std::string& uuid);
  void stopStream(const std::string& unit);

  void registerIpc(IpcService& ipc);

private:
  struct Impl;

  void notifyChanged();
  void schedulePoll();
  void finishRefresh(CastSnapshot next);
  void applyShareCommands(bool enabled);

  SessionBus* m_bus = nullptr;
  INetworkService* m_network = nullptr;
  CastSnapshot m_snapshot;
  ChangeCallback m_changeCallback;
  Timer m_pollTimer;
  bool m_fastPoll = false;
  std::unique_ptr<Impl> m_impl;
};
