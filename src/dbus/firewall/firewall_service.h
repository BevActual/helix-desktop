#pragma once

#include "core/timer_manager.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class SystemBus;
class IpcService;

struct FirewallServiceItem {
  std::string id;
  bool enabled = false;
  bool featured = false;

  bool operator==(const FirewallServiceItem&) const = default;
};

struct FirewallSnapshot {
  bool installed = false;
  bool running = false;
  bool busy = false;
  bool panic = false;
  std::string defaultZone;
  std::vector<std::string> zones;
  std::vector<std::string> interfaces;
  std::vector<FirewallServiceItem> services;
  std::string error;

  bool operator==(const FirewallSnapshot&) const = default;
};

class FirewallService {
public:
  using ChangeCallback = std::function<void()>;

  explicit FirewallService(SystemBus& bus);
  ~FirewallService();

  FirewallService(const FirewallService&) = delete;
  FirewallService& operator=(const FirewallService&) = delete;

  [[nodiscard]] const FirewallSnapshot& snapshot() const noexcept { return m_snapshot; }
  [[nodiscard]] bool running() const noexcept { return m_snapshot.running; }

  void setChangeCallback(ChangeCallback callback);
  void start();
  void refresh();
  void tryStart();
  void setPanic(bool enabled);
  void setDefaultZone(std::string zone);
  void setServiceEnabled(const std::string& serviceId, bool enabled);
  void savePermanent();

  void registerIpc(IpcService& ipc);

private:
  struct Impl;

  void attachDaemon();
  void detachDaemon();
  void notifyChanged();
  void scheduleRefresh();
  void finishRefresh(FirewallSnapshot next);

  SystemBus& m_bus;
  FirewallSnapshot m_snapshot;
  ChangeCallback m_changeCallback;
  Timer m_refreshTimer;
  std::unique_ptr<Impl> m_impl;
};
