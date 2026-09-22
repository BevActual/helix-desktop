#include "dbus/firewall/firewall_service.h"

#include "dbus/bus_name_watch.h"

#include "cli/schema_msg.h"
#include "core/log.h"
#include "core/process/process.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <utility>

namespace {

  constexpr Logger kLog("firewall");

  const sdbus::ServiceName kFirewallBusName{"org.fedoraproject.FirewallD1"};
  const sdbus::ObjectPath kFirewallPath{"/org/fedoraproject/FirewallD1"};
  const sdbus::ServiceName kDbusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kDbusPath{"/org/freedesktop/DBus"};
  const sdbus::ServiceName kSystemdName{"org.freedesktop.systemd1"};
  const sdbus::ObjectPath kSystemdPath{"/org/freedesktop/systemd1"};

  constexpr auto kFirewallInterface = "org.fedoraproject.FirewallD1";
  constexpr auto kZoneInterface = "org.fedoraproject.FirewallD1.zone";
  constexpr auto kSystemdManager = "org.freedesktop.systemd1.Manager";

  constexpr auto kRefreshDebounce = std::chrono::milliseconds(150);

  constexpr std::array<std::string_view, 10> kFeaturedServices{
      "ssh", "kdeconnect", "syncthing", "syncthing-gui", "mdns", "samba-client",
      "http", "https",     "cockpit",   "wireguard",
  };

  constexpr std::array<std::string_view, 4> kHiddenZones{"libvirt", "libvirt-routed", "nm-shared", "docker"};

  [[nodiscard]] bool isFeatured(std::string_view id) { return std::ranges::contains(kFeaturedServices, id); }

  [[nodiscard]] bool hideZone(std::string_view id, std::string_view defaultZone) {
    if (id == defaultZone) {
      return false;
    }
    return std::ranges::contains(kHiddenZones, id);
  }

  void sortZones(std::vector<std::string>& zones, std::string_view defaultZone) {
    std::ranges::sort(zones);
    const auto it = std::ranges::find(zones, defaultZone);
    if (it != zones.begin() && it != zones.end()) {
      std::rotate(zones.begin(), it, it + 1);
    }
  }

} // namespace

struct FirewallService::Impl {
  FirewallService& self;
  SystemBus& bus;
  std::unique_ptr<sdbus::IProxy> dbus;
  sdbus::Slot nameWatch;
  std::unique_ptr<sdbus::IProxy> daemon;
  std::shared_ptr<void> lifetime = std::make_shared<int>(0);
  std::uint64_t generation = 0;
  bool refreshInFlight = false;
  std::unique_ptr<sdbus::IProxy> inflight;

  explicit Impl(FirewallService& s, SystemBus& b) : self(s), bus(b) {}

  [[nodiscard]] std::weak_ptr<void> token() const { return lifetime; }

  void subscribeSignals() {
    if (daemon == nullptr) {
      return;
    }
    auto refresh = [this]() { self.scheduleRefresh(); };
    daemon->uponSignal("DefaultZoneChanged").onInterface(kFirewallInterface).call([refresh](const std::string&) {
      refresh();
    });
    daemon->uponSignal("PanicModeEnabled").onInterface(kFirewallInterface).call(refresh);
    daemon->uponSignal("PanicModeDisabled").onInterface(kFirewallInterface).call(refresh);
    daemon->uponSignal("Reloaded").onInterface(kFirewallInterface).call(refresh);
    daemon->uponSignal("ServiceAdded").onInterface(kZoneInterface).call([refresh](const std::string&, const std::string&, int) {
      refresh();
    });
    daemon->uponSignal("ServiceRemoved").onInterface(kZoneInterface).call([refresh](const std::string&, const std::string&) {
      refresh();
    });
    daemon->uponSignal("InterfaceAdded").onInterface(kZoneInterface).call([refresh](const std::string&, const std::string&) {
      refresh();
    });
    daemon->uponSignal("InterfaceRemoved").onInterface(kZoneInterface).call([refresh](const std::string&, const std::string&) {
      refresh();
    });
  }

  void fireVoid(const char* interface, const char* method) {
    if (daemon == nullptr) {
      return;
    }
    const std::weak_ptr<void> alive = token();
    try {
      daemon->callMethodAsync(method).onInterface(interface).uponReplyInvoke(
          [this, alive, method](std::optional<sdbus::Error> err) {
            if (alive.expired()) {
              return;
            }
            if (err.has_value()) {
              kLog.warn("{} failed: {}", method, err->what());
              self.m_snapshot.error = i18n::tr("control-center.firewall.write-failed");
              self.notifyChanged();
            }
            self.scheduleRefresh();
          }
      );
    } catch (const sdbus::Error& e) {
      kLog.warn("{} dispatch failed: {}", method, e.what());
    }
  }

  void fireString(const char* interface, const char* method, std::string arg) {
    if (daemon == nullptr) {
      return;
    }
    const std::weak_ptr<void> alive = token();
    try {
      daemon->callMethodAsync(method)
          .onInterface(interface)
          .withArguments(std::move(arg))
          .uponReplyInvoke([this, alive, method](std::optional<sdbus::Error> err) {
            if (alive.expired()) {
              return;
            }
            if (err.has_value()) {
              kLog.warn("{} failed: {}", method, err->what());
              self.m_snapshot.error = i18n::tr("control-center.firewall.write-failed");
              self.notifyChanged();
            }
            self.scheduleRefresh();
          });
    } catch (const sdbus::Error& e) {
      kLog.warn("{} dispatch failed: {}", method, e.what());
    }
  }
};

FirewallService::FirewallService(SystemBus& bus) : m_bus(bus), m_impl(std::make_unique<Impl>(*this, bus)) {}

FirewallService::~FirewallService() {
  m_refreshTimer.stop();
  m_impl->lifetime.reset();
}

void FirewallService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void FirewallService::notifyChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void FirewallService::scheduleRefresh() {
  m_refreshTimer.stop();
  m_refreshTimer.start(kRefreshDebounce, [this]() { refresh(); });
}

void FirewallService::start() {
  try {
    m_impl->dbus = sdbus::createProxy(m_bus.connection(), kDbusName, kDbusPath);
  } catch (const sdbus::Error& e) {
    kLog.warn("system dbus proxy failed: {}", e.what());
    m_snapshot.installed = process::commandExists("firewall-cmd");
    m_snapshot.error = i18n::tr("control-center.firewall.missing");
    notifyChanged();
    return;
  }

  m_impl->nameWatch = dbus_watch::watchBusName(
      m_bus.connection(), "org.fedoraproject.FirewallD1",
      [this](const std::string& /*name*/, const std::string& /*oldOwner*/, const std::string& newOwner) {
        if (newOwner.empty()) {
          detachDaemon();
        } else {
          attachDaemon();
        }
      }
  );

  if (m_bus.nameHasOwner("org.fedoraproject.FirewallD1")) {
    attachDaemon();
  } else {
    m_snapshot.installed = process::commandExists("firewall-cmd");
    notifyChanged();
  }
}

void FirewallService::attachDaemon() {
  if (m_impl->daemon != nullptr && m_snapshot.running) {
    refresh();
    return;
  }
  try {
    m_impl->daemon = sdbus::createProxy(m_bus.connection(), kFirewallBusName, kFirewallPath);
  } catch (const sdbus::Error& e) {
    kLog.warn("firewalld proxy failed: {}", e.what());
    detachDaemon();
    return;
  }
  m_impl->subscribeSignals();
  m_snapshot.installed = true;
  m_snapshot.running = true;
  notifyChanged();
  refresh();
}

void FirewallService::detachDaemon() {
  m_impl->daemon.reset();
  FirewallSnapshot next;
  next.installed = process::commandExists("firewall-cmd");
  finishRefresh(std::move(next));
}

void FirewallService::finishRefresh(FirewallSnapshot next) {
  m_impl->refreshInFlight = false;
  if (next == m_snapshot) {
    return;
  }
  m_snapshot = std::move(next);
  notifyChanged();
}

void FirewallService::refresh() {
  if (m_impl->refreshInFlight) {
    scheduleRefresh();
    return;
  }
  m_impl->refreshInFlight = true;
  const std::uint64_t generation = ++m_impl->generation;
  const std::weak_ptr<void> lifetime = m_impl->token();

  auto next = std::make_shared<FirewallSnapshot>();
  next->installed = process::commandExists("firewall-cmd") || m_bus.nameHasOwner("org.fedoraproject.FirewallD1");
  next->busy = m_snapshot.busy;
  if (m_impl->daemon == nullptr || !m_bus.nameHasOwner("org.fedoraproject.FirewallD1")) {
    finishRefresh(std::move(*next));
    return;
  }
  next->running = true;

  try {
    m_impl->daemon->callMethodAsync("getDefaultZone")
        .onInterface(kFirewallInterface)
        .uponReplyInvoke([this, lifetime, generation, next](std::optional<sdbus::Error> err, std::string zone) {
          if (lifetime.expired() || generation != m_impl->generation) {
            return;
          }
          if (err.has_value()) {
            next->error = i18n::tr("control-center.firewall.status-failed");
            finishRefresh(std::move(*next));
            return;
          }
          next->defaultZone = std::move(zone);
          m_impl->daemon->callMethodAsync("queryPanicMode")
              .onInterface(kFirewallInterface)
              .uponReplyInvoke([this, lifetime, generation, next](std::optional<sdbus::Error> panicErr, bool panic) {
                if (lifetime.expired() || generation != m_impl->generation) {
                  return;
                }
                if (!panicErr.has_value()) {
                  next->panic = panic;
                }
                m_impl->daemon->callMethodAsync("getZones")
                    .onInterface(kZoneInterface)
                    .uponReplyInvoke([this, lifetime, generation, next](
                                        std::optional<sdbus::Error> zonesErr, std::vector<std::string> zones
                                    ) {
                      if (lifetime.expired() || generation != m_impl->generation) {
                        return;
                      }
                      if (!zonesErr.has_value()) {
                        for (auto& id : zones) {
                          if (!hideZone(id, next->defaultZone)) {
                            next->zones.push_back(std::move(id));
                          }
                        }
                        sortZones(next->zones, next->defaultZone);
                      }
                      const std::string defaultZone = next->defaultZone;
                      m_impl->daemon->callMethodAsync("getInterfaces")
                          .onInterface(kZoneInterface)
                          .withArguments(defaultZone)
                          .uponReplyInvoke([this, lifetime, generation, next, defaultZone](
                                              std::optional<sdbus::Error> ifaceErr, std::vector<std::string> ifaces
                                          ) {
                            if (lifetime.expired() || generation != m_impl->generation) {
                              return;
                            }
                            if (!ifaceErr.has_value()) {
                              next->interfaces = std::move(ifaces);
                            }
                            m_impl->daemon->callMethodAsync("getServices")
                                .onInterface(kZoneInterface)
                                .withArguments(defaultZone)
                                .uponReplyInvoke([this, lifetime, generation, next](
                                                    std::optional<sdbus::Error> svcErr, std::vector<std::string> enabled
                                                ) {
                                  if (lifetime.expired() || generation != m_impl->generation) {
                                    return;
                                  }
                                  if (svcErr.has_value()) {
                                    next->error = i18n::tr("control-center.firewall.status-failed");
                                    finishRefresh(std::move(*next));
                                    return;
                                  }
                                  std::ranges::sort(enabled);
                                  for (const auto featured : kFeaturedServices) {
                                    FirewallServiceItem item;
                                    item.id = std::string(featured);
                                    item.featured = true;
                                    item.enabled = std::ranges::binary_search(enabled, item.id);
                                    next->services.push_back(std::move(item));
                                  }
                                  for (const auto& id : enabled) {
                                    if (isFeatured(id) || id == "dhcpv6-client") {
                                      continue;
                                    }
                                    next->services.push_back({.id = id, .enabled = true, .featured = false});
                                  }
                                  finishRefresh(std::move(*next));
                                });
                          });
                    });
              });
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("firewall refresh dispatch failed: {}", e.what());
    next->error = i18n::tr("control-center.firewall.status-failed");
    finishRefresh(std::move(*next));
  }
}

void FirewallService::tryStart() {
  if (m_snapshot.running) {
    refresh();
    return;
  }
  m_snapshot.busy = true;
  notifyChanged();
  const std::weak_ptr<void> lifetime = m_impl->token();
  try {
    m_impl->inflight = sdbus::createProxy(m_bus.connection(), kSystemdName, kSystemdPath);
    m_impl->inflight->callMethodAsync("StartUnit")
        .onInterface(kSystemdManager)
        .withArguments(std::string{"firewalld.service"}, std::string{"replace"})
        .uponReplyInvoke([this, lifetime](std::optional<sdbus::Error> err, sdbus::ObjectPath /*job*/) {
          if (lifetime.expired()) {
            return;
          }
          m_snapshot.busy = false;
          if (err.has_value()) {
            kLog.warn("start firewalld failed: {}", err->what());
            m_snapshot.error = i18n::tr("control-center.firewall.write-failed");
            notifyChanged();
          }
          scheduleRefresh();
        });
  } catch (const sdbus::Error& e) {
    m_snapshot.busy = false;
    kLog.warn("start firewalld dispatch failed: {}", e.what());
    m_snapshot.error = i18n::tr("control-center.firewall.write-failed");
    notifyChanged();
  }
}

void FirewallService::setPanic(bool enabled) {
  m_impl->fireVoid(kFirewallInterface, enabled ? "enablePanicMode" : "disablePanicMode");
}

void FirewallService::setDefaultZone(std::string zone) {
  if (zone.empty() || zone == m_snapshot.defaultZone) {
    return;
  }
  m_impl->fireString(kFirewallInterface, "setDefaultZone", std::move(zone));
}

void FirewallService::setServiceEnabled(const std::string& serviceId, bool enabled) {
  if (m_impl->daemon == nullptr || m_snapshot.defaultZone.empty() || serviceId.empty()) {
    return;
  }
  const std::weak_ptr<void> lifetime = m_impl->token();
  try {
    if (enabled) {
      m_impl->daemon->callMethodAsync("addService")
          .onInterface(kZoneInterface)
          .withArguments(m_snapshot.defaultZone, serviceId, 0)
          .uponReplyInvoke([this, lifetime](std::optional<sdbus::Error> err, std::string /*unused*/) {
            if (lifetime.expired()) {
              return;
            }
            if (err.has_value()) {
              kLog.warn("addService failed: {}", err->what());
              m_snapshot.error = i18n::tr("control-center.firewall.write-failed");
              notifyChanged();
            }
            scheduleRefresh();
          });
    } else {
      m_impl->daemon->callMethodAsync("removeService")
          .onInterface(kZoneInterface)
          .withArguments(m_snapshot.defaultZone, serviceId)
          .uponReplyInvoke([this, lifetime](std::optional<sdbus::Error> err, std::string /*unused*/) {
            if (lifetime.expired()) {
              return;
            }
            if (err.has_value()) {
              kLog.warn("removeService failed: {}", err->what());
              m_snapshot.error = i18n::tr("control-center.firewall.write-failed");
              notifyChanged();
            }
            scheduleRefresh();
          });
    }
  } catch (const sdbus::Error& e) {
    kLog.warn("setServiceEnabled dispatch failed: {}", e.what());
  }
}

void FirewallService::savePermanent() {
  m_impl->fireVoid(kFirewallInterface, "runtimeToPermanent");
}

void FirewallService::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::firewallStatus, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: firewall-status takes no arguments\n";
    }
    if (!m_snapshot.installed) {
      return "not-installed\n";
    }
    if (!m_snapshot.running) {
      return m_snapshot.busy ? "starting\n" : "stopped\n";
    }
    return std::format(
        "running zone={} panic={} services={}\n", m_snapshot.defaultZone.empty() ? "-" : m_snapshot.defaultZone,
        m_snapshot.panic ? "on" : "off", m_snapshot.services.size()
    );
  });
  ipc.bind(noctalia::cli::msg::firewallPanicToggle, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: firewall-panic-toggle takes no arguments\n";
    }
    if (!m_snapshot.running) {
      return "error: firewalld is not running\n";
    }
    setPanic(!m_snapshot.panic);
    return "ok\n";
  });
}
