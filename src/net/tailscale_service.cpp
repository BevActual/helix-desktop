#include "net/tailscale_service.h"

#include "cli/schema_msg.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "net/url_open.h"
#include "notification/notifications.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <format>
#include <nlohmann/json.hpp>
#include <optional>
#include <unordered_map>
#include <utility>

namespace {

  constexpr Logger kLog("tailscale");
  constexpr auto kStatusTimeout = std::chrono::seconds(8);
  constexpr auto kPrefsTimeout = std::chrono::seconds(4);
  constexpr auto kCliTimeout = std::chrono::seconds(15);
  constexpr auto kPrivilegedCliTimeout = std::chrono::seconds(120);
  constexpr std::size_t kMaxStatusBytes = 32 * 1024 * 1024;
  constexpr const char* kDefaultAdminUrl = "https://login.tailscale.com/admin";

  [[nodiscard]] std::string jsonString(const nlohmann::json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_string()) {
      return {};
    }
    return object[key].get<std::string>();
  }

  [[nodiscard]] bool jsonBool(const nlohmann::json& object, const char* key, bool fallback = false) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_boolean()) {
      return fallback;
    }
    return object[key].get<bool>();
  }

  [[nodiscard]] std::string firstIpv4(const nlohmann::json& ips) {
    if (!ips.is_array()) {
      return {};
    }
    for (const auto& ip : ips) {
      if (!ip.is_string()) {
        continue;
      }
      std::string value = ip.get<std::string>();
      if (value.find(':') == std::string::npos) {
        return value;
      }
    }
    return {};
  }

  [[nodiscard]] std::string firstIpv6(const nlohmann::json& ips) {
    if (!ips.is_array()) {
      return {};
    }
    for (const auto& ip : ips) {
      if (!ip.is_string()) {
        continue;
      }
      std::string value = ip.get<std::string>();
      if (value.find(':') != std::string::npos) {
        return value;
      }
    }
    return {};
  }

  [[nodiscard]] std::string trimDns(std::string dns) {
    while (!dns.empty() && dns.back() == '.') {
      dns.pop_back();
    }
    return dns;
  }

  [[nodiscard]] bool isMullvadPeer(const nlohmann::json& peer) {
    if (peer.contains("Location") && peer["Location"].is_object() && !peer["Location"].empty()) {
      return true;
    }
    const std::string dns = StringUtils::toLower(jsonString(peer, "DNSName"));
    return dns.find("mullvad.ts.net") != std::string::npos;
  }

  [[nodiscard]] std::string peerDisplayName(const nlohmann::json& peer) {
    const std::string host = jsonString(peer, "HostName");
    if (!host.empty()) {
      return host;
    }
    const std::string dns = trimDns(jsonString(peer, "DNSName"));
    if (!dns.empty()) {
      return dns;
    }
    return jsonString(peer, "ID");
  }

  [[nodiscard]] bool advertiseExitFromPrefs(const nlohmann::json& prefs) {
    if (jsonBool(prefs, "AdvertiseExitNode")) {
      return true;
    }
    if (!prefs.contains("AdvertiseRoutes") || !prefs["AdvertiseRoutes"].is_array()) {
      return false;
    }
    for (const auto& route : prefs["AdvertiseRoutes"]) {
      if (!route.is_string()) {
        continue;
      }
      const std::string value = route.get<std::string>();
      if (value == "0.0.0.0/0" || value == "::/0") {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] std::string currentUserName() {
    if (const char* user = std::getenv("USER"); user != nullptr && user[0] != '\0') {
      return user;
    }
    if (const char* user = std::getenv("LOGNAME"); user != nullptr && user[0] != '\0') {
      return user;
    }
    return "your-user";
  }

  [[nodiscard]] std::string resolveTailscaleBinary() {
    static const std::array kCandidates{"/usr/bin/tailscale", "/usr/local/bin/tailscale", "/usr/sbin/tailscale"};
    for (const char* candidate : kCandidates) {
      if (process::commandExists(candidate)) {
        return candidate;
      }
    }
    return "tailscale";
  }

  [[nodiscard]] bool looksLikeAccessDenied(std::string_view err, std::string_view out = {}) {
    const std::string lower = StringUtils::toLower(std::string(err) + "\n" + std::string(out));
    return lower.find("access denied") != std::string::npos || lower.find("prefs write access denied") != std::string::npos
        || lower.find("use 'sudo tailscale") != std::string::npos;
  }

  [[nodiscard]] bool looksLikeMissingAuthAgent(std::string_view err) {
    const std::string lower = StringUtils::toLower(err);
    return lower.find("no authentication agent found") != std::string::npos
        || lower.find("interactive authentication has not been enabled") != std::string::npos;
  }

  [[nodiscard]] bool isPrivilegeEscalator(std::string_view command) {
    return command == "run0" || command == "pkexec";
  }

  // pkexec is the GUI path: run0 over pipes cannot enable polkit interactive auth.
  [[nodiscard]] std::optional<std::string> resolveGuiPrivilegeEscalator() {
    if (process::commandExists("pkexec")) {
      return "pkexec";
    }
    return process::resolvePrivilegeEscalator();
  }

  [[nodiscard]] std::vector<std::string> escalateTailscale(const std::vector<std::string>& args) {
    const auto escalator = resolveGuiPrivilegeEscalator();
    if (!escalator.has_value() || args.empty() || isPrivilegeEscalator(args.front())) {
      return {};
    }
    std::vector<std::string> privileged;
    privileged.reserve(args.size() + 1);
    privileged.push_back(*escalator);
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (i == 0 && args[i] == "tailscale") {
        privileged.push_back(resolveTailscaleBinary());
      } else {
        privileged.push_back(args[i]);
      }
    }
    return privileged;
  }

} // namespace

TailscaleService::TailscaleService(ConfigService& config) : m_config(config) {
  m_snapshot.installed = process::commandExists("tailscale");
  m_config.addReloadCallback([this]() { schedulePoll(); }, "tailscale");
}

void TailscaleService::start() { refresh(); }

TailscaleService::~TailscaleService() { m_lifetime.reset(); }

void TailscaleService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void TailscaleService::setAuthPrep(std::function<void(std::function<void()>)> prep) { m_authPrep = std::move(prep); }

void TailscaleService::refresh() {
  if (m_refreshInFlight) {
    return;
  }
  m_snapshot.installed = process::commandExists("tailscale");
  if (!m_snapshot.installed) {
    TailscaleSnapshot empty;
    empty.installed = false;
    empty.error = i18n::tr("control-center.tailscale.missing");
    const TailscaleSnapshot previous = std::move(m_snapshot);
    m_snapshot = std::move(empty);
    notifyPeerChanges(previous, m_snapshot);
    notifyChanged();
    schedulePoll();
    return;
  }

  m_refreshInFlight = true;
  const auto gen = ++m_generation;
  const std::weak_ptr<void> lifetime = m_lifetime;
  (void)process::runAsync(
      std::vector<std::string>{"tailscale", "status", "--json"},
      process::RunCallbacks{
          .onExit =
              [this, gen, lifetime](process::RunResult statusResult) {
                DeferredCall::callLater([this, gen, lifetime, statusResult = std::move(statusResult)]() mutable {
                  if (lifetime.expired() || gen != m_generation) {
                    m_refreshInFlight = false;
                    return;
                  }
                  (void)process::runAsync(
                      std::vector<std::string>{"tailscale", "debug", "prefs"},
                      process::RunCallbacks{
                          .onExit =
                              [this, gen, lifetime, statusResult = std::move(statusResult)](process::RunResult prefsResult
                              ) {
                                DeferredCall::callLater(
                                    [this, gen, lifetime, statusResult = std::move(statusResult),
                                     prefsResult = std::move(prefsResult)]() mutable {
                                      if (lifetime.expired() || gen != m_generation) {
                                        m_refreshInFlight = false;
                                        return;
                                      }
                                      std::string error;
                                      if (!statusResult && statusResult.out.empty()) {
                                        error = StringUtils::trim(statusResult.err);
                                        if (error.empty()) {
                                          error = i18n::tr("control-center.tailscale.status-failed");
                                        }
                                      }
                                      finishRefresh(std::move(statusResult.out), std::move(prefsResult.out), std::move(error));
                                    }
                                );
                              },
                      },
                      process::RunOptions{.timeout = kPrefsTimeout, .maxOutputBytes = 1024 * 1024}
                  );
                });
              },
      },
      process::RunOptions{.timeout = kStatusTimeout, .maxOutputBytes = kMaxStatusBytes}
  );
}

void TailscaleService::setWantRunning(bool up) {
  setBusy(true);
  runCli({"tailscale", up ? "up" : "down"}, [this](bool ok, std::string /*out*/, std::string err) {
    finishWrite(ok, std::move(err));
  });
}

void TailscaleService::toggle() { setWantRunning(!m_snapshot.running); }

void TailscaleService::setShieldsUp(bool enabled) {
  setBusy(true);
  runCli(
      {"tailscale", "set", enabled ? "--shields-up=true" : "--shields-up=false"},
      [this](bool ok, std::string /*out*/, std::string err) { finishWrite(ok, std::move(err)); }
  );
}

void TailscaleService::setAcceptRoutes(bool enabled) {
  setBusy(true);
  runCli(
      {"tailscale", "set", enabled ? "--accept-routes=true" : "--accept-routes=false"},
      [this](bool ok, std::string /*out*/, std::string err) { finishWrite(ok, std::move(err)); }
  );
}

void TailscaleService::setSsh(bool enabled) {
  setBusy(true);
  runCli(
      {"tailscale", "set", enabled ? "--ssh=true" : "--ssh=false"},
      [this](bool ok, std::string /*out*/, std::string err) { finishWrite(ok, std::move(err)); }
  );
}

void TailscaleService::setAcceptDns(bool enabled) {
  setBusy(true);
  runCli(
      {"tailscale", "set", enabled ? "--accept-dns=true" : "--accept-dns=false"},
      [this](bool ok, std::string /*out*/, std::string err) { finishWrite(ok, std::move(err)); }
  );
}

void TailscaleService::setAdvertiseExitNode(bool enabled) {
  setBusy(true);
  runCli(
      {"tailscale", "set", enabled ? "--advertise-exit-node=true" : "--advertise-exit-node=false"},
      [this](bool ok, std::string /*out*/, std::string err) { finishWrite(ok, std::move(err)); }
  );
}

void TailscaleService::setAllowLan(bool enabled) {
  setBusy(true);
  runCli(
      {"tailscale", "set", enabled ? "--exit-node-allow-lan-access=true" : "--exit-node-allow-lan-access=false"},
      [this](bool ok, std::string /*out*/, std::string err) { finishWrite(ok, std::move(err)); }
  );
}

void TailscaleService::setExitNode(std::string node) {
  setBusy(true);
  const std::string flag = "--exit-node=" + node;
  runCli({"tailscale", "set", flag}, [this](bool ok, std::string /*out*/, std::string err) {
    finishWrite(ok, std::move(err));
  });
}

void TailscaleService::openAdmin() { (void)net::openInBrowser(adminUrl()); }

void TailscaleService::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::tailscaleUp, [this](const std::string& /*args*/) -> std::string {
    setWantRunning(true);
    return "ok\n";
  });
  ipc.bind(noctalia::cli::msg::tailscaleDown, [this](const std::string& /*args*/) -> std::string {
    setWantRunning(false);
    return "ok\n";
  });
  ipc.bind(noctalia::cli::msg::tailscaleToggle, [this](const std::string& /*args*/) -> std::string {
    toggle();
    return "ok\n";
  });
  ipc.bind(noctalia::cli::msg::tailscaleStatus, [this](const std::string& /*args*/) -> std::string {
    if (!m_snapshot.installed) {
      return "not-installed\n";
    }
    if (m_snapshot.running) {
      return std::format(
          "running hostname={} ipv4={} exit={}\n", m_snapshot.hostname, m_snapshot.ipv4,
          m_snapshot.exitNode.empty() ? "none" : m_snapshot.exitNode
      );
    }
    return std::format("stopped state={}\n", m_snapshot.backendState.empty() ? "Stopped" : m_snapshot.backendState);
  });
}

void TailscaleService::schedulePoll() {
  m_pollTimer.stop();
  m_pollTimer.start(std::chrono::seconds(pollIntervalSeconds()), [this]() { refresh(); });
}

void TailscaleService::runCli(
    std::vector<std::string> args, std::function<void(bool ok, std::string out, std::string err)> onDone,
    bool allowEscalate
) {
  const bool privileged = !args.empty() && isPrivilegeEscalator(args.front());
  const std::weak_ptr<void> lifetime = m_lifetime;
  const std::vector<std::string> command = args;
  (void)process::runAsync(
      command,
      process::RunCallbacks{
          .onExit =
              [this, lifetime, onDone = std::move(onDone), args = std::move(args), allowEscalate](
                  process::RunResult result
              ) {
                DeferredCall::callLater(
                    [this, lifetime, onDone = std::move(onDone), args = std::move(args), allowEscalate,
                     result = std::move(result)]() mutable {
                      if (lifetime.expired()) {
                        return;
                      }
                      if (!result && allowEscalate && looksLikeAccessDenied(result.err, result.out)) {
                        auto privilegedArgs = escalateTailscale(args);
                        if (privilegedArgs.empty()) {
                          onDone(
                              false, std::move(result.out),
                              i18n::tr("control-center.tailscale.needs-operator", "user", currentUserName())
                          );
                          return;
                        }
                        auto resume = [this, lifetime, onDone = std::move(onDone),
                                        privilegedArgs = std::move(privilegedArgs)]() mutable {
                          if (lifetime.expired()) {
                            return;
                          }
                          kLog.info("retrying tailscale command with {}", privilegedArgs.front());
                          runCli(std::move(privilegedArgs), std::move(onDone), false);
                        };
                        if (m_authPrep) {
                          m_authPrep(std::move(resume));
                        } else {
                          resume();
                        }
                        return;
                      }
                      if (!result) {
                        kLog.warn(
                            "tailscale command failed: {}",
                            StringUtils::trim(result.err.empty() ? result.out : result.err)
                        );
                      }
                      onDone(static_cast<bool>(result), std::move(result.out), std::move(result.err));
                    }
                );
              },
      },
      process::RunOptions{.timeout = privileged ? kPrivilegedCliTimeout : kCliTimeout}
  );
}

void TailscaleService::finishWrite(bool ok, std::string err) {
  if (!ok) {
    const std::string trimmed = StringUtils::trim(err);
    if (looksLikeMissingAuthAgent(trimmed)) {
      m_pendingCommandError = i18n::tr("control-center.tailscale.needs-operator", "user", currentUserName());
    } else if (!trimmed.empty()) {
      m_pendingCommandError = trimmed;
    } else {
      m_pendingCommandError = i18n::tr("control-center.tailscale.command-failed");
    }
    m_snapshot.error = m_pendingCommandError;
  }
  setBusy(false);
  refresh();
}

void TailscaleService::applyStatusJson(std::string jsonText) {
  if (jsonText.empty()) {
    return;
  }
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(jsonText);
  } catch (const nlohmann::json::exception& e) {
    kLog.warn("failed to parse tailscale status: {}", e.what());
    m_snapshot.error = i18n::tr("control-center.tailscale.status-failed");
    return;
  }
  if (!root.is_object()) {
    return;
  }

  m_snapshot.backendState = jsonString(root, "BackendState");
  m_snapshot.version = jsonString(root, "Version");
  m_snapshot.authUrl = jsonString(root, "AuthURL");
  m_snapshot.running = m_snapshot.backendState == "Running";
  m_snapshot.ipv4 = firstIpv4(root.contains("TailscaleIPs") ? root["TailscaleIPs"] : nlohmann::json{});
  m_snapshot.ipv6 = firstIpv6(root.contains("TailscaleIPs") ? root["TailscaleIPs"] : nlohmann::json{});

  if (root.contains("CurrentTailnet") && root["CurrentTailnet"].is_object()) {
    m_snapshot.tailnet = jsonString(root["CurrentTailnet"], "Name");
  }
  if (root.contains("Self") && root["Self"].is_object()) {
    const auto& self = root["Self"];
    m_snapshot.hostname = jsonString(self, "HostName");
    m_snapshot.dnsName = trimDns(jsonString(self, "DNSName"));
    if (m_snapshot.ipv4.empty()) {
      m_snapshot.ipv4 = firstIpv4(self.contains("TailscaleIPs") ? self["TailscaleIPs"] : nlohmann::json{});
    }
    if (m_snapshot.ipv6.empty()) {
      m_snapshot.ipv6 = firstIpv6(self.contains("TailscaleIPs") ? self["TailscaleIPs"] : nlohmann::json{});
    }
  }

  m_snapshot.health.clear();
  if (root.contains("Health") && root["Health"].is_array()) {
    for (const auto& item : root["Health"]) {
      if (item.is_string()) {
        m_snapshot.health.push_back(item.get<std::string>());
      }
    }
  }

  m_snapshot.peers.clear();
  m_snapshot.exitNodes.clear();
  m_snapshot.exitNode.clear();
  m_snapshot.exitNodeOnline = false;
  m_snapshot.onlineCount = 0;
  m_snapshot.peerCount = 0;

  if (!root.contains("Peer") || !root["Peer"].is_object()) {
    return;
  }

  for (auto it = root["Peer"].begin(); it != root["Peer"].end(); ++it) {
    if (!it.value().is_object()) {
      continue;
    }
    const auto& peer = it.value();
    TailscalePeer item;
    item.id = jsonString(peer, "ID");
    if (item.id.empty()) {
      item.id = it.key();
    }
    item.name = peerDisplayName(peer);
    item.dnsName = trimDns(jsonString(peer, "DNSName"));
    item.ipv4 = firstIpv4(peer.contains("TailscaleIPs") ? peer["TailscaleIPs"] : nlohmann::json{});
    item.os = jsonString(peer, "OS");
    item.relay = jsonString(peer, "Relay");
    item.online = jsonBool(peer, "Online");
    item.active = jsonBool(peer, "Active");
    item.exitNodeOption = jsonBool(peer, "ExitNodeOption");
    item.isExitNode = jsonBool(peer, "ExitNode");
    const bool mullvad = isMullvadPeer(peer);

    if (item.exitNodeOption || item.isExitNode) {
      TailscaleExitNode exit;
      exit.id = item.id;
      exit.name = item.name;
      exit.ip = item.ipv4;
      exit.online = item.online;
      exit.selected = item.isExitNode;
      m_snapshot.exitNodes.push_back(std::move(exit));
    }
    if (item.isExitNode) {
      m_snapshot.exitNode = item.name;
      m_snapshot.exitNodeOnline = item.online;
    }
    if (mullvad) {
      continue;
    }
    if (item.online) {
      ++m_snapshot.onlineCount;
    }
    ++m_snapshot.peerCount;
    m_snapshot.peers.push_back(std::move(item));
  }

  std::ranges::sort(m_snapshot.peers, [](const TailscalePeer& a, const TailscalePeer& b) {
    if (a.online != b.online) {
      return a.online && !b.online;
    }
    return StringUtils::toLower(a.name) < StringUtils::toLower(b.name);
  });
  std::ranges::sort(m_snapshot.exitNodes, [](const TailscaleExitNode& a, const TailscaleExitNode& b) {
    if (a.selected != b.selected) {
      return a.selected && !b.selected;
    }
    if (a.online != b.online) {
      return a.online && !b.online;
    }
    return StringUtils::toLower(a.name) < StringUtils::toLower(b.name);
  });
}

void TailscaleService::applyPrefsJson(std::string jsonText) {
  if (jsonText.empty()) {
    return;
  }
  nlohmann::json prefs;
  try {
    prefs = nlohmann::json::parse(jsonText);
  } catch (const nlohmann::json::exception& e) {
    kLog.warn("failed to parse tailscale prefs: {}", e.what());
    return;
  }
  if (!prefs.is_object()) {
    return;
  }
  m_snapshot.shieldsUp = jsonBool(prefs, "ShieldsUp");
  m_snapshot.acceptRoutes = jsonBool(prefs, "RouteAll");
  m_snapshot.runSsh = jsonBool(prefs, "RunSSH");
  m_snapshot.acceptDns = jsonBool(prefs, "CorpDNS");
  m_snapshot.exitNodeAllowLan = jsonBool(prefs, "ExitNodeAllowLANAccess");
  m_snapshot.advertiseExitNode = advertiseExitFromPrefs(prefs);
  if (m_snapshot.exitNode.empty()) {
    m_snapshot.exitNode = jsonString(prefs, "ExitNodeIP");
    if (m_snapshot.exitNode.empty()) {
      m_snapshot.exitNode = jsonString(prefs, "ExitNodeID");
    }
  }
}

void TailscaleService::finishRefresh(std::string statusJson, std::string prefsJson, std::string error) {
  m_refreshInFlight = false;
  const TailscaleSnapshot previous = m_snapshot;
  const bool busy = m_snapshot.busy;
  if (!m_pendingCommandError.empty()) {
    error = std::move(m_pendingCommandError);
    m_pendingCommandError.clear();
  }
  m_snapshot = TailscaleSnapshot{};
  m_snapshot.installed = process::commandExists("tailscale");
  m_snapshot.busy = busy;
  m_snapshot.error = std::move(error);
  applyStatusJson(std::move(statusJson));
  applyPrefsJson(std::move(prefsJson));
  notifyPeerChanges(previous, m_snapshot);
  notifyChanged();
  schedulePoll();
}

void TailscaleService::notifyPeerChanges(const TailscaleSnapshot& previous, const TailscaleSnapshot& next) {
  if (!m_config.config().tailscale.notifyOnPeerChange || !previous.installed || !next.installed) {
    return;
  }
  std::unordered_map<std::string, bool> wasOnline;
  wasOnline.reserve(previous.peers.size());
  for (const auto& peer : previous.peers) {
    wasOnline[peer.id] = peer.online;
  }
  for (const auto& peer : next.peers) {
    const auto it = wasOnline.find(peer.id);
    if (it == wasOnline.end() || it->second == peer.online) {
      continue;
    }
    notify::info(
        "Noctalia", i18n::tr("control-center.tailscale.title"),
        peer.online ? i18n::tr("control-center.tailscale.peer-online", "name", peer.name)
                    : i18n::tr("control-center.tailscale.peer-offline", "name", peer.name)
    );
  }
}

void TailscaleService::setBusy(bool busy) {
  if (m_snapshot.busy == busy) {
    return;
  }
  m_snapshot.busy = busy;
  notifyChanged();
}

void TailscaleService::notifyChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

int TailscaleService::pollIntervalSeconds() const {
  return std::clamp(m_config.config().tailscale.refreshIntervalSeconds, 3, 120);
}

std::string TailscaleService::adminUrl() const {
  if (!m_snapshot.authUrl.empty()) {
    return m_snapshot.authUrl;
  }
  const std::string configured = StringUtils::trim(m_config.config().tailscale.adminUrl);
  if (!configured.empty()) {
    return configured;
  }
  return kDefaultAdminUrl;
}
