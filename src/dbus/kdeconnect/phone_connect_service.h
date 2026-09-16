#pragma once

#include "core/timer_manager.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class SessionBus;
class IpcService;

struct PhoneDevice {
  std::string id;
  std::string name;
  std::string type;
  bool reachable = false;
  bool paired = false;
  bool pairRequested = false;
  bool pairRequestedByPeer = false;
  std::string verificationKey;
  bool hasBattery = false;
  int charge = -1;
  bool charging = false;

  bool operator==(const PhoneDevice&) const = default;
};

struct PhoneSnapshot {
  bool available = false;
  bool activating = false;
  std::string announcedName;
  std::vector<PhoneDevice> devices;
  std::string error;

  bool operator==(const PhoneSnapshot&) const = default;
};

class PhoneConnectService {
public:
  using ChangeCallback = std::function<void()>;

  explicit PhoneConnectService(SessionBus& bus);
  ~PhoneConnectService();

  PhoneConnectService(const PhoneConnectService&) = delete;
  PhoneConnectService& operator=(const PhoneConnectService&) = delete;

  [[nodiscard]] const PhoneSnapshot& snapshot() const noexcept { return m_snapshot; }
  [[nodiscard]] bool available() const noexcept { return m_snapshot.available; }

  void setChangeCallback(ChangeCallback callback);
  void start();
  void refresh();
  void scheduleRefresh();
  void tryActivate();
  void setDiscovery(bool enabled);

  void requestPair(const std::string& deviceId);
  void acceptPair(const std::string& deviceId);
  void rejectPair(const std::string& deviceId);
  void unpair(const std::string& deviceId);
  void ping(const std::string& deviceId);
  void ring(const std::string& deviceId);
  void shareUrl(const std::string& deviceId, std::string url);
  void shareText(const std::string& deviceId, std::string text);
  void sharePath(const std::string& deviceId, std::string path);

  void registerIpc(IpcService& ipc);

private:
  struct Impl;
  friend struct Impl;

  void attachDaemon();
  void detachDaemon();
  void notifyChanged();
  [[nodiscard]] PhoneDevice* findDevice(const std::string& deviceId);
  [[nodiscard]] const PhoneDevice* findDevice(const std::string& deviceId) const;
  [[nodiscard]] const PhoneDevice* resolveDevice(std::string_view query) const;

  SessionBus& m_bus;
  PhoneSnapshot m_snapshot;
  ChangeCallback m_changeCallback;
  Timer m_refreshTimer;
  std::unique_ptr<Impl> m_impl;
};
