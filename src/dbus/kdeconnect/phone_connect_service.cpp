#include "dbus/kdeconnect/phone_connect_service.h"

#include "cli/schema_msg.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("phone");

  const sdbus::ServiceName kKdeConnectBusName{"org.kde.kdeconnect"};
  const sdbus::ObjectPath kDaemonPath{"/modules/kdeconnect"};
  const sdbus::ServiceName kDbusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kDbusPath{"/org/freedesktop/DBus"};

  constexpr auto kDbusInterface = "org.freedesktop.DBus";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  constexpr auto kDaemonInterface = "org.kde.kdeconnect.daemon";
  constexpr auto kDeviceInterface = "org.kde.kdeconnect.device";
  constexpr auto kBatteryInterface = "org.kde.kdeconnect.device.battery";
  constexpr auto kPingInterface = "org.kde.kdeconnect.device.ping";
  constexpr auto kFindMyPhoneInterface = "org.kde.kdeconnect.device.findmyphone";
  constexpr auto kShareInterface = "org.kde.kdeconnect.device.share";

  constexpr auto kDiscoveryToken = "helix-shell";
  constexpr auto kRefreshDebounce = std::chrono::milliseconds(150);

  using InterfaceProps = std::map<std::string, sdbus::Variant>;

  template <typename T> std::optional<T> variantGet(const sdbus::Variant& value) {
    try {
      return value.get<T>();
    } catch (const sdbus::Error&) {
      return std::nullopt;
    }
  }

  bool isUnknownMethod(const sdbus::Error& err) {
    return err.getName() == sdbus::Error::Name{"org.freedesktop.DBus.Error.UnknownMethod"};
  }

  std::string deviceObjectPath(const std::string& id) { return "/modules/kdeconnect/devices/" + id; }

  std::string pluginObjectPath(const std::string& id, std::string_view plugin) {
    return deviceObjectPath(id) + "/" + std::string(plugin);
  }

  void mergeDeviceProps(const InterfaceProps& props, PhoneDevice& out) {
    if (auto it = props.find("id"); it != props.end()) {
      if (auto v = variantGet<std::string>(it->second); v && !v->empty()) {
        out.id = std::move(*v);
      }
    }
    if (auto it = props.find("name"); it != props.end()) {
      if (auto v = variantGet<std::string>(it->second)) {
        out.name = std::move(*v);
      }
    }
    if (auto it = props.find("type"); it != props.end()) {
      if (auto v = variantGet<std::string>(it->second)) {
        out.type = std::move(*v);
      }
    }
    if (auto it = props.find("isReachable"); it != props.end()) {
      if (auto v = variantGet<bool>(it->second)) {
        out.reachable = *v;
      }
    }
    if (auto it = props.find("isPaired"); it != props.end()) {
      if (auto v = variantGet<bool>(it->second)) {
        out.paired = *v;
      }
    } else if (auto trusted = props.find("isTrusted"); trusted != props.end()) {
      if (auto v = variantGet<bool>(trusted->second)) {
        out.paired = *v;
      }
    }
    if (auto it = props.find("isPairRequested"); it != props.end()) {
      if (auto v = variantGet<bool>(it->second)) {
        out.pairRequested = *v;
      }
    }
    if (auto it = props.find("isPairRequestedByPeer"); it != props.end()) {
      if (auto v = variantGet<bool>(it->second)) {
        out.pairRequestedByPeer = *v;
      }
    }
    if (auto it = props.find("verificationKey"); it != props.end()) {
      if (auto v = variantGet<std::string>(it->second)) {
        out.verificationKey = std::move(*v);
      }
    }
  }

  void mergeBatteryProps(const InterfaceProps& props, PhoneDevice& out) {
    if (auto it = props.find("hasBattery"); it != props.end()) {
      if (auto v = variantGet<bool>(it->second)) {
        out.hasBattery = *v;
      }
    }
    if (auto it = props.find("charge"); it != props.end()) {
      if (auto v = variantGet<std::int32_t>(it->second)) {
        out.charge = *v;
        if (out.charge >= 0) {
          out.hasBattery = true;
        }
      }
    }
    if (auto it = props.find("isCharging"); it != props.end()) {
      if (auto v = variantGet<bool>(it->second)) {
        out.charging = *v;
      }
    }
  }

  std::string fileUrlFromPath(std::string_view path) {
    std::string out = "file://";
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (const char raw : path) {
      const auto c = static_cast<unsigned char>(raw);
      const bool keep = c == '/'
          || (c >= 'A' && c <= 'Z')
          || (c >= 'a' && c <= 'z')
          || (c >= '0' && c <= '9')
          || c == '-'
          || c == '_'
          || c == '.'
          || c == '~';
      if (keep) {
        out.push_back(static_cast<char>(c));
      } else {
        out.push_back('%');
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0F]);
      }
    }
    return out;
  }

} // namespace

struct PhoneConnectService::Impl {
  PhoneConnectService& self;
  SessionBus& bus;
  std::unique_ptr<sdbus::IProxy> dbus;
  std::unique_ptr<sdbus::IProxy> daemon;
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> deviceProxies;
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> batteryProxies;
  std::vector<std::unique_ptr<sdbus::IProxy>> inflight;
  std::shared_ptr<void> lifetime = std::make_shared<int>(0);
  bool discoveryHeld = false;
  bool wantDiscovery = false;
  bool devicesTwoArg = true;
  std::uint64_t listGeneration = 0;

  explicit Impl(PhoneConnectService& s, SessionBus& b) : self(s), bus(b) {}

  [[nodiscard]] std::weak_ptr<void> token() const { return lifetime; }

  PhoneDevice* findDevice(const std::string& id) { return self.findDevice(id); }

  std::unique_ptr<sdbus::IProxy> makeProxy(const std::string& path) {
    return sdbus::createProxy(bus.connection(), kKdeConnectBusName, sdbus::ObjectPath{path});
  }

  sdbus::IProxy* takeInflight(const std::string& path) {
    try {
      inflight.push_back(makeProxy(path));
      return inflight.back().get();
    } catch (const sdbus::Error& e) {
      kLog.warn("proxy create failed {}: {}", path, e.what());
      return nullptr;
    }
  }

  void fireVoid(const std::string& path, const char* interface, const char* method) {
    auto* proxy = takeInflight(path);
    if (proxy == nullptr) {
      return;
    }
    const std::weak_ptr<void> alive = token();
    try {
      proxy->callMethodAsync(method).onInterface(interface).uponReplyInvoke(
          [alive, path, method](std::optional<sdbus::Error> err) {
            if (alive.expired() || !err.has_value()) {
              return;
            }
            kLog.warn("{} {} failed: {}", method, path, err->what());
          }
      );
    } catch (const sdbus::Error& e) {
      kLog.warn("{} dispatch failed {}: {}", method, path, e.what());
    }
  }

  void fireString(const std::string& path, const char* interface, const char* method, std::string arg) {
    auto* proxy = takeInflight(path);
    if (proxy == nullptr) {
      return;
    }
    const std::weak_ptr<void> alive = token();
    try {
      proxy->callMethodAsync(method)
          .onInterface(interface)
          .withArguments(std::move(arg))
          .uponReplyInvoke([alive, path, method](std::optional<sdbus::Error> err) {
            if (alive.expired() || !err.has_value()) {
              return;
            }
            kLog.warn("{} {} failed: {}", method, path, err->what());
          });
    } catch (const sdbus::Error& e) {
      kLog.warn("{} dispatch failed {}: {}", method, path, e.what());
    }
  }

  void ensureDeviceProxy(const std::string& id) {
    if (deviceProxies.contains(id)) {
      return;
    }
    std::unique_ptr<sdbus::IProxy> proxy;
    try {
      proxy = makeProxy(deviceObjectPath(id));
    } catch (const sdbus::Error& e) {
      kLog.debug("device proxy failed {}: {}", id, e.what());
      return;
    }
    proxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this, id](
                  const std::string& interfaceName, const InterfaceProps& changed,
                  const std::vector<std::string>& /*invalidated*/
              ) {
          if (interfaceName != kDeviceInterface) {
            return;
          }
          auto* device = findDevice(id);
          if (device == nullptr) {
            self.scheduleRefresh();
            return;
          }
          PhoneDevice updated = *device;
          mergeDeviceProps(changed, updated);
          if (updated != *device) {
            *device = std::move(updated);
            self.notifyChanged();
          }
        });
    deviceProxies.emplace(id, std::move(proxy));
  }

  void ensureBatteryProxy(const std::string& id) {
    if (batteryProxies.contains(id)) {
      return;
    }
    std::unique_ptr<sdbus::IProxy> proxy;
    try {
      proxy = makeProxy(pluginObjectPath(id, "battery"));
    } catch (const sdbus::Error& e) {
      kLog.debug("battery proxy failed {}: {}", id, e.what());
      return;
    }
    proxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this, id](
                  const std::string& interfaceName, const InterfaceProps& changed,
                  const std::vector<std::string>& /*invalidated*/
              ) {
          if (interfaceName != kBatteryInterface) {
            return;
          }
          auto* device = findDevice(id);
          if (device == nullptr) {
            return;
          }
          PhoneDevice updated = *device;
          mergeBatteryProps(changed, updated);
          if (updated != *device) {
            *device = std::move(updated);
            self.notifyChanged();
          }
        });
    proxy->uponSignal("refreshed")
        .onInterface(kBatteryInterface)
        .call([this, id](bool charging, std::int32_t charge) {
          auto* device = findDevice(id);
          if (device == nullptr) {
            return;
          }
          device->charging = charging;
          device->charge = charge;
          device->hasBattery = charge >= 0;
          self.notifyChanged();
        });
    batteryProxies.emplace(id, std::move(proxy));
  }

  void loadBattery(const std::string& id) {
    ensureBatteryProxy(id);
    auto it = batteryProxies.find(id);
    if (it == batteryProxies.end() || it->second == nullptr) {
      return;
    }
    const std::weak_ptr<void> alive = token();
    try {
      it->second->callMethodAsync("GetAll")
          .onInterface(kPropertiesInterface)
          .withArguments(std::string{kBatteryInterface})
          .uponReplyInvoke([this, alive, id](std::optional<sdbus::Error> err, InterfaceProps props) {
            if (alive.expired()) {
              return;
            }
            if (err.has_value()) {
              return;
            }
            auto* device = findDevice(id);
            if (device == nullptr) {
              return;
            }
            PhoneDevice updated = *device;
            mergeBatteryProps(props, updated);
            if (updated != *device) {
              *device = std::move(updated);
              self.notifyChanged();
            }
          });
    } catch (const sdbus::Error&) {
    }
  }

  void applyDeviceList(std::vector<std::string> ids) {
    std::vector<PhoneDevice> next;
    next.reserve(ids.size());
    std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> keepDevices;
    std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> keepBatteries;
    for (auto& id : ids) {
      if (id.empty()) {
        continue;
      }
      PhoneDevice device;
      if (auto* existing = findDevice(id)) {
        device = *existing;
      }
      device.id = id;
      next.push_back(std::move(device));
      if (auto it = deviceProxies.find(id); it != deviceProxies.end()) {
        keepDevices.emplace(id, std::move(it->second));
      }
      if (auto it = batteryProxies.find(id); it != batteryProxies.end()) {
        keepBatteries.emplace(id, std::move(it->second));
      }
    }
    deviceProxies = std::move(keepDevices);
    batteryProxies = std::move(keepBatteries);
    self.m_snapshot.devices = std::move(next);
    self.m_snapshot.error.clear();
    self.notifyChanged();

    for (const auto& device : self.m_snapshot.devices) {
      ensureDeviceProxy(device.id);
      loadDevice(device.id);
    }
  }

  void loadDevice(const std::string& id) {
    ensureDeviceProxy(id);
    auto it = deviceProxies.find(id);
    if (it == deviceProxies.end() || it->second == nullptr) {
      return;
    }
    const std::weak_ptr<void> alive = token();
    try {
      it->second->callMethodAsync("GetAll")
          .onInterface(kPropertiesInterface)
          .withArguments(std::string{kDeviceInterface})
          .uponReplyInvoke([this, alive, id](std::optional<sdbus::Error> err, InterfaceProps props) {
            if (alive.expired()) {
              return;
            }
            if (err.has_value()) {
              kLog.debug("device GetAll failed {}: {}", id, err->what());
              return;
            }
            PhoneDevice* device = findDevice(id);
            if (device == nullptr) {
              PhoneDevice created;
              device = &self.m_snapshot.devices.emplace_back(std::move(created));
              device->id = id;
            }
            PhoneDevice updated = *device;
            mergeDeviceProps(props, updated);
            if (updated.id.empty()) {
              updated.id = id;
            }
            *device = std::move(updated);
            self.notifyChanged();
            if (device->paired) {
              loadBattery(id);
            }
          });
    } catch (const sdbus::Error& e) {
      kLog.debug("device GetAll dispatch failed {}: {}", id, e.what());
    }
  }

  void requestDeviceList() {
    if (daemon == nullptr) {
      return;
    }
    const std::uint64_t generation = ++listGeneration;
    const std::weak_ptr<void> alive = token();
    auto onList = [this, alive, generation](std::optional<sdbus::Error> err, std::vector<std::string> ids) {
      if (alive.expired() || generation != listGeneration) {
        return;
      }
      if (err.has_value()) {
        if (devicesTwoArg && isUnknownMethod(*err)) {
          devicesTwoArg = false;
          requestDeviceList();
          return;
        }
        kLog.warn("devices() failed: {}", err->what());
        self.m_snapshot.error = i18n::tr("control-center.phone.status-failed");
        self.notifyChanged();
        return;
      }
      applyDeviceList(std::move(ids));
    };

    try {
      if (devicesTwoArg) {
        daemon->callMethodAsync("devices")
            .onInterface(kDaemonInterface)
            .withArguments(false, false)
            .uponReplyInvoke(onList);
      } else {
        daemon->callMethodAsync("devices").onInterface(kDaemonInterface).withArguments(false).uponReplyInvoke(onList);
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("devices() dispatch failed: {}", e.what());
    }
  }

  void loadAnnouncedName() {
    if (daemon == nullptr) {
      return;
    }
    const std::weak_ptr<void> alive = token();
    try {
      daemon->callMethodAsync("announcedName")
          .onInterface(kDaemonInterface)
          .uponReplyInvoke([this, alive](std::optional<sdbus::Error> err, std::string name) {
            if (alive.expired()) {
              return;
            }
            if (err.has_value()) {
              return;
            }
            if (self.m_snapshot.announcedName != name) {
              self.m_snapshot.announcedName = std::move(name);
              self.notifyChanged();
            }
          });
    } catch (const sdbus::Error&) {
    }
  }

  void applyDiscovery(bool enabled) {
    wantDiscovery = enabled;
    if (daemon == nullptr) {
      discoveryHeld = false;
      return;
    }
    const char* method = enabled ? "acquireDiscoveryMode" : "releaseDiscoveryMode";
    try {
      const std::weak_ptr<void> alive = token();
      daemon->callMethodAsync(method)
          .onInterface(kDaemonInterface)
          .withArguments(std::string{kDiscoveryToken})
          .uponReplyInvoke([alive, method](std::optional<sdbus::Error> err) {
            if (alive.expired() || !err.has_value()) {
              return;
            }
            kLog.debug("{} failed: {}", method, err->what());
          });
      discoveryHeld = enabled;
    } catch (const sdbus::Error& e) {
      kLog.debug("{} dispatch failed: {}", method, e.what());
    }
    if (enabled) {
      try {
        daemon->callMethodAsync("forceOnNetworkChange")
            .onInterface(kDaemonInterface)
            .uponReplyInvoke([](std::optional<sdbus::Error>) {});
      } catch (const sdbus::Error&) {
      }
    }
  }

  void watchDaemonSignals() {
    if (daemon == nullptr) {
      return;
    }
    daemon->uponSignal("deviceAdded")
        .onInterface(kDaemonInterface)
        .call([this](const std::string& /*id*/) { self.scheduleRefresh(); });
    daemon->uponSignal("deviceRemoved")
        .onInterface(kDaemonInterface)
        .call([this](const std::string& /*id*/) { self.scheduleRefresh(); });
    daemon->uponSignal("deviceListChanged").onInterface(kDaemonInterface).call([this]() { self.scheduleRefresh(); });
    daemon->uponSignal("deviceVisibilityChanged")
        .onInterface(kDaemonInterface)
        .call([this](const std::string& /*id*/, bool /*visible*/) { self.scheduleRefresh(); });
    daemon->uponSignal("announcedNameChanged")
        .onInterface(kDaemonInterface)
        .call([this](const std::string& name) {
          self.m_snapshot.announcedName = name;
          self.notifyChanged();
        });
  }
};

PhoneConnectService::PhoneConnectService(SessionBus& bus)
    : m_bus(bus), m_impl(std::make_unique<Impl>(*this, bus)) {}

PhoneConnectService::~PhoneConnectService() {
  if (m_impl != nullptr) {
    m_impl->applyDiscovery(false);
    m_impl->lifetime.reset();
  }
}

void PhoneConnectService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void PhoneConnectService::start() {
  try {
    m_impl->dbus = sdbus::createProxy(m_bus.connection(), kDbusName, kDbusPath);
  } catch (const sdbus::Error& e) {
    kLog.warn("session dbus proxy failed: {}", e.what());
    m_snapshot.error = i18n::tr("control-center.phone.missing");
    notifyChanged();
    return;
  }

  m_impl->dbus->uponSignal("NameOwnerChanged")
      .onInterface(kDbusInterface)
      .call([this](const std::string& name, const std::string& /*oldOwner*/, const std::string& newOwner) {
        if (name != "org.kde.kdeconnect") {
          return;
        }
        if (newOwner.empty()) {
          detachDaemon();
        } else {
          attachDaemon();
        }
      });

  bool hasOwner = false;
  try {
    m_impl->dbus->callMethod("NameHasOwner")
        .onInterface(kDbusInterface)
        .withArguments(std::string{"org.kde.kdeconnect"})
        .storeResultsTo(hasOwner);
  } catch (const sdbus::Error& e) {
    kLog.debug("NameHasOwner failed: {}", e.what());
  }
  if (hasOwner) {
    attachDaemon();
  } else {
    m_snapshot.available = false;
    notifyChanged();
  }
}

void PhoneConnectService::scheduleRefresh() {
  m_refreshTimer.stop();
  m_refreshTimer.start(kRefreshDebounce, [this]() { refresh(); });
}

void PhoneConnectService::refresh() {
  if (!m_snapshot.available) {
    bool hasOwner = false;
    if (m_impl->dbus != nullptr) {
      try {
        m_impl->dbus->callMethod("NameHasOwner")
            .onInterface(kDbusInterface)
            .withArguments(std::string{"org.kde.kdeconnect"})
            .storeResultsTo(hasOwner);
      } catch (const sdbus::Error&) {
      }
    }
    if (hasOwner) {
      attachDaemon();
    }
    return;
  }
  if (m_impl->daemon == nullptr) {
    attachDaemon();
    return;
  }
  m_impl->loadAnnouncedName();
  m_impl->requestDeviceList();
}

void PhoneConnectService::tryActivate() {
  if (m_snapshot.available || m_impl->dbus == nullptr) {
    return;
  }
  m_snapshot.activating = true;
  notifyChanged();
  const std::weak_ptr<void> alive = m_impl->token();
  try {
    m_impl->dbus->callMethodAsync("StartServiceByName")
        .onInterface(kDbusInterface)
        .withArguments(std::string{"org.kde.kdeconnect"}, std::uint32_t{0})
        .uponReplyInvoke([this, alive](std::optional<sdbus::Error> err, std::uint32_t /*result*/) {
          if (alive.expired()) {
            return;
          }
          m_snapshot.activating = false;
          if (err.has_value()) {
            kLog.info("kdeconnectd is not available: {}", err->what());
            m_snapshot.available = false;
            m_snapshot.error.clear();
            notifyChanged();
            return;
          }
          attachDaemon();
        });
  } catch (const sdbus::Error& e) {
    m_snapshot.activating = false;
    kLog.info("kdeconnectd start failed: {}", e.what());
    notifyChanged();
  }
}

void PhoneConnectService::setDiscovery(bool enabled) { m_impl->applyDiscovery(enabled); }

void PhoneConnectService::requestPair(const std::string& deviceId) {
  m_impl->fireVoid(deviceObjectPath(deviceId), kDeviceInterface, "requestPairing");
}

void PhoneConnectService::acceptPair(const std::string& deviceId) {
  m_impl->fireVoid(deviceObjectPath(deviceId), kDeviceInterface, "acceptPairing");
}

void PhoneConnectService::rejectPair(const std::string& deviceId) {
  m_impl->fireVoid(deviceObjectPath(deviceId), kDeviceInterface, "rejectPairing");
}

void PhoneConnectService::unpair(const std::string& deviceId) {
  m_impl->fireVoid(deviceObjectPath(deviceId), kDeviceInterface, "unpair");
}

void PhoneConnectService::ping(const std::string& deviceId) {
  m_impl->fireVoid(pluginObjectPath(deviceId, "ping"), kPingInterface, "sendPing");
}

void PhoneConnectService::ring(const std::string& deviceId) {
  m_impl->fireVoid(pluginObjectPath(deviceId, "findmyphone"), kFindMyPhoneInterface, "ring");
}

void PhoneConnectService::shareUrl(const std::string& deviceId, std::string url) {
  if (url.empty()) {
    return;
  }
  m_impl->fireString(pluginObjectPath(deviceId, "share"), kShareInterface, "shareUrl", std::move(url));
}

void PhoneConnectService::shareText(const std::string& deviceId, std::string text) {
  if (text.empty()) {
    return;
  }
  m_impl->fireString(pluginObjectPath(deviceId, "share"), kShareInterface, "shareText", std::move(text));
}

void PhoneConnectService::sharePath(const std::string& deviceId, std::string path) {
  if (path.empty()) {
    return;
  }
  shareUrl(deviceId, fileUrlFromPath(path));
}

void PhoneConnectService::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::phoneStatus, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: phone-status takes no arguments\n";
    }
    if (!m_snapshot.available) {
      return m_snapshot.activating ? "starting\n" : "not-installed\n";
    }
    int reachable = 0;
    int paired = 0;
    for (const auto& device : m_snapshot.devices) {
      if (device.paired) {
        ++paired;
      }
      if (device.reachable) {
        ++reachable;
      }
    }
    return std::format(
        "running name={} paired={} reachable={}\n", m_snapshot.announcedName.empty() ? "-" : m_snapshot.announcedName,
        paired, reachable
    );
  });
  ipc.bind(noctalia::cli::msg::phonePing, [this](const std::string& args) -> std::string {
    const auto* device = resolveDevice(StringUtils::trim(args));
    if (device == nullptr) {
      return "error: no matching paired device\n";
    }
    ping(device->id);
    return "ok\n";
  });
  ipc.bind(noctalia::cli::msg::phoneRing, [this](const std::string& args) -> std::string {
    const auto* device = resolveDevice(StringUtils::trim(args));
    if (device == nullptr) {
      return "error: no matching paired device\n";
    }
    ring(device->id);
    return "ok\n";
  });
}

void PhoneConnectService::attachDaemon() {
  if (m_impl->daemon != nullptr && m_snapshot.available) {
    refresh();
    return;
  }
  try {
    m_impl->daemon = m_impl->makeProxy(std::string{kDaemonPath});
  } catch (const sdbus::Error& e) {
    kLog.warn("kdeconnect daemon proxy failed: {}", e.what());
    m_snapshot.available = false;
    notifyChanged();
    return;
  }
  m_snapshot.available = true;
  m_snapshot.activating = false;
  m_snapshot.error.clear();
  m_impl->watchDaemonSignals();
  m_impl->loadAnnouncedName();
  m_impl->requestDeviceList();
  if (m_impl->wantDiscovery) {
    m_impl->applyDiscovery(true);
  }
  notifyChanged();
}

void PhoneConnectService::detachDaemon() {
  m_impl->discoveryHeld = false;
  m_impl->daemon.reset();
  m_impl->deviceProxies.clear();
  m_impl->batteryProxies.clear();
  m_impl->inflight.clear();
  m_snapshot = PhoneSnapshot{};
  notifyChanged();
}

void PhoneConnectService::notifyChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

PhoneDevice* PhoneConnectService::findDevice(const std::string& deviceId) {
  auto it = std::ranges::find(m_snapshot.devices, deviceId, &PhoneDevice::id);
  return it != m_snapshot.devices.end() ? &*it : nullptr;
}

const PhoneDevice* PhoneConnectService::findDevice(const std::string& deviceId) const {
  auto it = std::ranges::find(m_snapshot.devices, deviceId, &PhoneDevice::id);
  return it != m_snapshot.devices.end() ? &*it : nullptr;
}

const PhoneDevice* PhoneConnectService::resolveDevice(std::string_view query) const {
  const std::string needle = StringUtils::toLower(StringUtils::trim(query));
  const PhoneDevice* onlyPaired = nullptr;
  int pairedCount = 0;
  for (const auto& device : m_snapshot.devices) {
    if (!device.paired) {
      continue;
    }
    ++pairedCount;
    onlyPaired = &device;
    if (!needle.empty()
        && (device.id == query || StringUtils::toLower(device.name) == needle
            || StringUtils::toLower(device.name).find(needle) != std::string::npos)) {
      return &device;
    }
  }
  if (needle.empty() && pairedCount == 1) {
    return onlyPaired;
  }
  return nullptr;
}
