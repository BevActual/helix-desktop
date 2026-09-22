#include "dbus/cast/cast_service.h"

#include "cli/schema_msg.h"
#include "core/deferred_call.h"
#include "dbus/bus_name_watch.h"
#include "core/log.h"
#include "core/process/process.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "net/url_open.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <random>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("cast");

  const sdbus::ServiceName kDbusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kDbusPath{"/org/freedesktop/DBus"};
  const sdbus::ServiceName kManagerName{"org.gnome.NetworkDisplays.Manager"};
  const sdbus::ObjectPath kManagerPath{"/org/gnome/NetworkDisplays/Manager"};

  constexpr auto kDbusInterface = "org.freedesktop.DBus";
  constexpr auto kManagerInterface = "org.gnome.NetworkDisplays.Manager";

  constexpr std::string_view kShareDir = ".local/share/gnome-remote-desktop";

  [[nodiscard]] std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
      return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
  }

  [[nodiscard]] std::string randomPassword() {
    static constexpr char kAlphabet[] = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::random_device device;
    std::mt19937 rng(device());
    std::uniform_int_distribution<int> pick(0, static_cast<int>(sizeof(kAlphabet) - 2));
    std::string out(10, '0');
    for (char& ch : out) {
      ch = kAlphabet[static_cast<std::size_t>(pick(rng))];
    }
    return out;
  }

  [[nodiscard]] std::filesystem::path shareDir() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
      return {};
    }
    return std::filesystem::path(home) / kShareDir;
  }

  [[nodiscard]] bool ensureShareCerts() {
    const auto dir = shareDir();
    if (dir.empty()) {
      return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto cert = dir / "rdp-tls.crt";
    const auto key = dir / "rdp-tls.key";
    if (std::filesystem::exists(cert, ec) && std::filesystem::exists(key, ec)) {
      return true;
    }
    if (!process::commandExists("openssl")) {
      return false;
    }
    const auto result = process::runSync(
        {"openssl", "req", "-x509", "-newkey", "rsa:3072", "-sha256", "-days", "730", "-nodes", "-subj",
         "/CN=helix-shell", "-keyout", key.string(), "-out", cert.string()}
    );
    return static_cast<bool>(result);
  }

  [[nodiscard]] std::string protocolLabel(std::uint32_t protocol) {
    switch (protocol) {
    case 3: // ND_SINK_PROTOCOL_WFD_P2P
    case 4: // ND_SINK_PROTOCOL_WFD_MICE
      return "miracast";
    case 5: // ND_SINK_PROTOCOL_CC
      return "chromecast";
    default:
      return "display";
    }
  }

  [[nodiscard]] std::string sinkStateLabel(std::uint32_t state) {
    if (state >= 0x10000) {
      return "error";
    }
    if (state >= 0x1000) {
      return "streaming";
    }
    if (state > 0) {
      return "connecting";
    }
    return "idle";
  }

  void parseGrdctl(const std::string& text, CastSnapshot& snap) {
    snap.shareInstalled = true;
    std::string section;
    for (std::string_view remain = text; !remain.empty();) {
      const auto nl = remain.find('\n');
      const auto line = remain.substr(0, nl);
      remain = nl == std::string_view::npos ? std::string_view{} : remain.substr(nl + 1);
      const std::string trimmed = trim(line);
      if (trimmed.empty()) {
        continue;
      }
      if (!trimmed.empty() && trimmed.back() == ':' && (line.empty() || (line.front() != '\t' && line.front() != ' '))) {
        section = trimmed.substr(0, trimmed.size() - 1);
        continue;
      }
      const auto colon = trimmed.find(':');
      if (colon == std::string::npos) {
        continue;
      }
      const std::string key = trim(trimmed.substr(0, colon));
      const std::string value = trim(trimmed.substr(colon + 1));
      if (section == "RDP" && key == "Status") {
        snap.shareEnabled = value == "enabled";
      } else if (section == "RDP" && key == "Port") {
        try {
          snap.sharePort = std::stoi(value);
        } catch (...) {
        }
      } else if (section == "RDP" && key == "View-only") {
        snap.viewOnly = value == "yes";
      } else if (section == "RDP" && key == "Username") {
        snap.username = value == "(empty)" ? std::string{} : value;
      } else if (section == "RDP" && key == "Password") {
        snap.password = value == "(empty)" ? std::string{} : value;
      }
    }
  }

} // namespace

struct CastService::Impl {
  CastService& self;
  SessionBus* bus = nullptr;
  std::unique_ptr<sdbus::IProxy> dbus;
  sdbus::Slot nameWatch;
  std::unique_ptr<sdbus::IProxy> manager;
  std::shared_ptr<void> lifetime = std::make_shared<int>(0);
  std::map<std::string, std::string> streamUnits;

  explicit Impl(CastService& s, SessionBus* b) : self(s), bus(b) {}

  [[nodiscard]] std::weak_ptr<void> token() const { return lifetime; }

  void attachManager() {
    if (bus == nullptr) {
      return;
    }
    try {
      manager = sdbus::createProxy(bus->connection(), kManagerName, kManagerPath);
      manager->uponSignal("PropertiesChanged")
          .onInterface("org.freedesktop.DBus.Properties")
          .call([this](const std::string&, const std::map<std::string, sdbus::Variant>&, const std::vector<std::string>&) {
            self.refresh();
          });
    } catch (const sdbus::Error& e) {
      kLog.warn("network displays manager proxy failed: {}", e.what());
      manager.reset();
    }
  }
};

CastService::CastService(SessionBus* bus, INetworkService* network)
    : m_bus(bus), m_network(network), m_impl(std::make_unique<Impl>(*this, bus)) {}

CastService::~CastService() {
  m_pollTimer.stop();
  m_impl->lifetime.reset();
}

void CastService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void CastService::notifyChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void CastService::schedulePoll() {
  m_pollTimer.stop();
  const auto seconds = m_fastPoll ? 3 : 12;
  m_pollTimer.start(std::chrono::seconds(seconds), [this]() { refresh(); });
}

void CastService::start() {
  if (m_bus != nullptr) {
    try {
      m_impl->dbus = sdbus::createProxy(m_bus->connection(), kDbusName, kDbusPath);
      m_impl->nameWatch = dbus_watch::watchBusName(
          m_bus->connection(), "org.gnome.NetworkDisplays.Manager",
          [this](const std::string&, const std::string&, const std::string& newOwner) {
            if (newOwner.empty()) {
              m_impl->manager.reset();
            } else {
              m_impl->attachManager();
            }
            refresh();
          }
      );
      bool hasOwner = false;
      m_impl->dbus->callMethod("NameHasOwner")
          .onInterface(kDbusInterface)
          .withArguments(std::string{"org.gnome.NetworkDisplays.Manager"})
          .storeResultsTo(hasOwner);
      if (hasOwner) {
        m_impl->attachManager();
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("session dbus proxy failed: {}", e.what());
    }
  }
  refresh();
  schedulePoll();
}

void CastService::setWantFastPoll(bool enabled) {
  if (m_fastPoll == enabled) {
    return;
  }
  m_fastPoll = enabled;
  schedulePoll();
  if (enabled) {
    refresh();
  }
}

void CastService::finishRefresh(CastSnapshot next) {
  next.shareBusy = m_snapshot.shareBusy;
  next.wirelessBusy = m_snapshot.wirelessBusy;
  if (next.password.empty()) {
    next.password = m_snapshot.password;
  }
  if (next == m_snapshot) {
    return;
  }
  m_snapshot = std::move(next);
  notifyChanged();
}

// PATH lookups and `grdctl status` fork a process and take ~25ms; at poll cadence that
// was a hitch in every animation, so the probe runs on a worker and only the D-Bus
// property read and the snapshot swap happen on the main thread.
void CastService::refresh() {
  if (m_refreshInFlight) {
    m_refreshQueued = true;
    return;
  }
  m_refreshInFlight = true;
  std::weak_ptr<void> lifetime = m_lifetime;
  std::thread([this, lifetime]() {
    CastSnapshot probed;
    probed.shareInstalled = process::commandExists("grdctl");
    probed.wirelessInstalled =
        process::commandExists("gnome-network-displays") || process::commandExists("gnome-network-displays-daemon");
    if (probed.shareInstalled) {
      const auto status = process::runSync({"grdctl", "status", "--show-credentials"});
      if (status) {
        parseGrdctl(status.out, probed);
      } else {
        parseGrdctl(status.out + status.err, probed);
      }
    }
    DeferredCall::callLater([this, lifetime, probed = std::move(probed)]() mutable {
      if (lifetime.expired()) {
        return;
      }
      m_refreshInFlight = false;
      completeRefresh(std::move(probed));
      if (m_refreshQueued) {
        m_refreshQueued = false;
        refresh();
      }
    });
  }).detach();
}

void CastService::completeRefresh(CastSnapshot next) {
  next.wirelessRunning = m_impl->manager != nullptr;

  if (m_network != nullptr && !m_network->state().ipv4.empty() && next.sharePort > 0) {
    next.shareAddress = std::format("{}:{}", m_network->state().ipv4, next.sharePort);
  }

  if (m_impl->manager != nullptr) {
    try {
      using Dict = std::map<std::string, sdbus::Variant>;
      const auto displays = m_impl->manager->getProperty("Displays").onInterface(kManagerInterface).get<std::vector<Dict>>();
      for (const auto& display : displays) {
        CastSink sink;
        if (const auto it = display.find("uuid"); it != display.end() && it->second.containsValueOfType<std::string>()) {
          sink.uuid = it->second.get<std::string>();
        }
        if (const auto it = display.find("display-name");
            it != display.end() && it->second.containsValueOfType<std::string>()) {
          sink.name = it->second.get<std::string>();
        }
        if (const auto it = display.find("protocol"); it != display.end() && it->second.containsValueOfType<std::uint32_t>()) {
          sink.protocol = protocolLabel(it->second.get<std::uint32_t>());
        }
        if (const auto it = display.find("state"); it != display.end() && it->second.containsValueOfType<std::uint32_t>()) {
          sink.state = sinkStateLabel(it->second.get<std::uint32_t>());
        }
        if (const auto unit = m_impl->streamUnits.find(sink.uuid); unit != m_impl->streamUnits.end()) {
          sink.unit = unit->second;
          if (sink.state == "idle") {
            sink.state = "streaming";
          }
        }
        if (!sink.uuid.empty()) {
          next.sinks.push_back(std::move(sink));
        }
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("Displays property failed: {}", e.what());
      next.error = i18n::tr("control-center.cast.status-failed");
    }
  }

  finishRefresh(std::move(next));
}

void CastService::applyShareCommands(bool enabled) {
  if (!process::commandExists("grdctl")) {
    m_snapshot.shareBusy = false;
    m_snapshot.error = i18n::tr("control-center.cast.share-missing");
    notifyChanged();
    return;
  }

  if (enabled) {
    if (!ensureShareCerts()) {
      m_snapshot.shareBusy = false;
      m_snapshot.error = i18n::tr("control-center.cast.share-failed");
      notifyChanged();
      return;
    }
    const auto dir = shareDir();
    (void)process::runSync({"grdctl", "rdp", "set-tls-cert", (dir / "rdp-tls.crt").string()});
    (void)process::runSync({"grdctl", "rdp", "set-tls-key", (dir / "rdp-tls.key").string()});
    if (m_snapshot.username.empty() || m_snapshot.password.empty()) {
      const char* user = std::getenv("USER");
      const std::string username = user != nullptr && user[0] != '\0' ? user : "helix";
      const std::string password = randomPassword();
      (void)process::runSync({"grdctl", "rdp", "set-credentials", username, password});
      m_snapshot.username = username;
      m_snapshot.password = password;
    }
    (void)process::runSync({"grdctl", "rdp", "enable"});
    (void)process::runSync({"systemctl", "--user", "start", "gnome-remote-desktop.service"});
  } else {
    (void)process::runSync({"grdctl", "rdp", "disable"});
    (void)process::runSync({"systemctl", "--user", "stop", "gnome-remote-desktop.service"});
  }
  m_snapshot.shareBusy = false;
  refresh();
}

void CastService::setShareEnabled(bool enabled) {
  m_snapshot.shareBusy = true;
  notifyChanged();
  applyShareCommands(enabled);
}

void CastService::setViewOnly(bool enabled) {
  if (!process::commandExists("grdctl")) {
    return;
  }
  (void)process::runSync({"grdctl", "rdp", enabled ? "enable-view-only" : "disable-view-only"});
  refresh();
}

void CastService::tryStartWireless() {
  if (m_snapshot.wirelessRunning) {
    refresh();
    return;
  }
  m_snapshot.wirelessBusy = true;
  notifyChanged();
  if (process::commandExists("gnome-network-displays-daemon")) {
    (void)process::runAsync(std::vector<std::string>{"gnome-network-displays-daemon"});
  } else if (process::commandExists("gnome-network-displays")) {
    (void)process::runAsync(std::vector<std::string>{"gnome-network-displays"});
  } else {
    m_snapshot.wirelessBusy = false;
    m_snapshot.error = i18n::tr("control-center.cast.wireless-missing");
    notifyChanged();
    return;
  }
  m_pollTimer.stop();
  m_pollTimer.start(std::chrono::milliseconds(400), [this]() {
    m_snapshot.wirelessBusy = false;
    refresh();
    schedulePoll();
  });
}

void CastService::openWirelessApp() {
  if (process::commandExists("gnome-network-displays")) {
    (void)process::runAsync(std::vector<std::string>{"gnome-network-displays"});
    return;
  }
  (void)net::openInBrowser("https://gitlab.gnome.org/GNOME/gnome-network-displays");
}

void CastService::startStream(const std::string& uuid) {
  if (m_impl->manager == nullptr) {
    return;
  }
  const std::weak_ptr<void> lifetime = m_impl->token();
  try {
    m_impl->manager->callMethodAsync("StartStream")
        .onInterface(kManagerInterface)
        .withArguments(uuid)
        .uponReplyInvoke([this, lifetime, uuid](std::optional<sdbus::Error> err, std::string unit) {
          if (lifetime.expired()) {
            return;
          }
          if (err.has_value()) {
            kLog.warn("StartStream failed: {}", err->what());
            m_snapshot.error = i18n::tr("control-center.cast.stream-failed");
            notifyChanged();
            return;
          }
          m_impl->streamUnits[uuid] = std::move(unit);
          refresh();
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("StartStream dispatch failed: {}", e.what());
    m_snapshot.error = i18n::tr("control-center.cast.stream-failed");
    notifyChanged();
  }
}

void CastService::stopStream(const std::string& unit) {
  if (m_impl->manager == nullptr || unit.empty()) {
    return;
  }
  const std::weak_ptr<void> lifetime = m_impl->token();
  try {
    m_impl->manager->callMethodAsync("StopStream")
        .onInterface(kManagerInterface)
        .withArguments(unit)
        .uponReplyInvoke([this, lifetime, unit](std::optional<sdbus::Error> err) {
          if (lifetime.expired()) {
            return;
          }
          if (err.has_value()) {
            kLog.warn("StopStream failed: {}", err->what());
            m_snapshot.error = i18n::tr("control-center.cast.stream-failed");
            notifyChanged();
          }
          for (auto it = m_impl->streamUnits.begin(); it != m_impl->streamUnits.end();) {
            if (it->second == unit) {
              it = m_impl->streamUnits.erase(it);
            } else {
              ++it;
            }
          }
          refresh();
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("StopStream dispatch failed: {}", e.what());
    m_snapshot.error = i18n::tr("control-center.cast.stream-failed");
    notifyChanged();
  }
}

void CastService::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::castStatus, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: cast-status takes no arguments\n";
    }
    return std::format(
        "share={} address={} wireless={} sinks={}\n", m_snapshot.shareEnabled ? "on" : "off",
        m_snapshot.shareAddress.empty() ? "-" : m_snapshot.shareAddress,
        m_snapshot.wirelessRunning ? "on" : (m_snapshot.wirelessInstalled ? "off" : "not-installed"),
        m_snapshot.sinks.size()
    );
  });
  ipc.bind(noctalia::cli::msg::castShareToggle, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: cast-share-toggle takes no arguments\n";
    }
    if (!m_snapshot.shareInstalled) {
      return "error: screen sharing is not available\n";
    }
    setShareEnabled(!m_snapshot.shareEnabled);
    return "ok\n";
  });
}
