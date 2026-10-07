#include "desktop_api/remote_network.h"
#include <algorithm>

namespace helix {
RemoteNetwork::RemoteNetwork() {
  m_observer = desktopClient().observe([this](std::string_view interface, std::string_view signal, const Json&) {
    if (interface == networkInterface && signal == "Changed") refresh();
  });
  // The service announces changes; this only covers a signal that was missed.
  m_refreshTimer.startRepeating(std::chrono::seconds(30), [this] { refresh(); });
  refresh();
}
RemoteNetwork::~RemoteNetwork() { desktopClient().unobserve(m_observer); }
void RemoteNetwork::refresh() {
  if (m_refreshing) return;
  // Shell snapshots use bounded synchronous calls; service actions remain asynchronous.
  try {
    if (!desktopClient().available()) throw std::runtime_error("Desktop service unavailable");
    auto snapshot = desktopClient().call("network", "GetSnapshot");
    const auto old = m_state;
    auto oldAps = m_aps; auto oldVpns = m_vpns;
    m_available = snapshot.value("available", false);
    m_state = {}; m_aps.clear(); m_vpns.clear(); m_saved.clear();
    if (m_available) {
      m_state.connected = snapshot.value("connected", false); m_state.wirelessEnabled = snapshot.value("enabled", false);
      m_state.resolving = snapshot.value("resolving", false); m_state.scanning = snapshot.value("scanning", false);
      m_state.ssid = snapshot.value("ssid", ""); m_state.ipv4 = snapshot.value("address", "");
      m_state.interfaceName = snapshot.value("interface", ""); m_state.kind = NetworkConnectivity(snapshot.value("kind", 0));
      m_state.signalStrength = snapshot.value("signal", std::uint8_t{}); m_state.frequencyMhz = snapshot.value("frequency", 0u);
      m_state.vpnActive = snapshot.value("vpn-active", false); m_state.vpnConnected = snapshot.value("vpn-connected", false);
      m_state.cellularActive = snapshot.value("cellular-active", false);
      m_wired = snapshot.value("wired", false); m_cellular = snapshot.value("cellular", false);
      for (const auto& ap : snapshot.at("access-points")) {
        m_aps.push_back({.path = ap.at("path"), .devicePath = ap.at("device"), .ssid = ap.at("ssid"),
          .strength = ap.at("strength"), .secured = ap.at("secured"),
          .keyManagement = network_manager_security::KeyManagement(ap.at("key-management").get<int>()),
          .active = ap.at("active"), .frequencyMhz = ap.at("frequency")});
        if (ap.value("saved", false)) m_saved.push_back(ap.at("ssid"));
      }
      for (const auto& vpn : snapshot.at("vpns")) m_vpns.push_back({vpn.at("path"), vpn.at("name"), vpn.at("active")});
    }
    if (m_callback && (old != m_state || oldAps != m_aps || oldVpns != m_vpns)) m_callback(m_state, NetworkChangeOrigin::External);
  } catch (...) {
    m_available = false;
    if (m_state != NetworkState{}) { m_state = {}; m_aps.clear(); m_vpns.clear(); if (m_callback) m_callback(m_state, NetworkChangeOrigin::External); }
  }
}
bool RemoteNetwork::action(const char* name, const Json& args) {
  try { desktopClient().call("network", "Action", g_variant_new("(s@a{sv})", name, toVariant(args))); return true; }
  catch (...) { return false; }
}
void RemoteNetwork::requestScan() { action("scan", Json::object()); }
bool RemoteNetwork::activateAccessPoint(const AccessPointInfo& ap) { return action("connect", {{"path", ap.path}}); }
bool RemoteNetwork::activateAccessPoint(const AccessPointInfo& ap, const std::string& secret) { return action("connect", {{"path", ap.path}, {"secret", secret}}); }
bool RemoteNetwork::activateEnterpriseAccessPoint(const AccessPointInfo& ap, const network_enterprise::EnterpriseCredentials& credentials) {
  return action("enterprise", {{"path", ap.path}, {"identity", credentials.identity}, {"anonymous", credentials.anonymousIdentity},
    {"secret", credentials.password}, {"eap", int(credentials.eap)}, {"phase2", int(credentials.phase2)},
    {"certificate", credentials.caCertPath}, {"domain", credentials.domainSuffixMatch}});
}
bool RemoteNetwork::activateVpnConnection(const VpnConnectionInfo& vpn) { return action("vpn-connect", {{"path", vpn.path}}); }
bool RemoteNetwork::deactivateVpnConnection(const VpnConnectionInfo& vpn) { return action("vpn-disconnect", {{"path", vpn.path}}); }
bool RemoteNetwork::activateWiredConnection() { return action("wired", Json::object()); }
bool RemoteNetwork::activateCellularConnection() { return action("cellular-connect", Json::object()); }
bool RemoteNetwork::deactivateCellularConnection() { return action("cellular-disconnect", Json::object()); }
void RemoteNetwork::setWirelessEnabled(bool enabled, WirelessEnabledCompletion complete) {
  const bool accepted = action("enable", {{"value", enabled}}); if (complete) complete(accepted);
}
void RemoteNetwork::disconnect() { action("disconnect", Json::object()); }
void RemoteNetwork::forgetSsid(const std::string& ssid) { action("forget", {{"ssid", ssid}}); }
bool RemoteNetwork::hasSavedConnection(const std::string& ssid) const { return std::ranges::find(m_saved, ssid) != m_saved.end(); }
void RemoteNetwork::requestActiveWifiPsk(WifiPskCallback callback) {
  desktopClient().callAsync("network", "GetWifiSecret", nullptr, [callback](Json result, std::string error) {
    callback(error.empty() && !result.get<std::string>().empty() ? std::optional(result.get<std::string>()) : std::nullopt);
  });
}
void RemoteNetwork::requestActiveDnsOverride(DnsOverrideQueryCallback callback) {
  desktopClient().callAsync("network", "GetDns", nullptr, [callback](Json result, std::string error) {
    DnsOverride value;
    if (error.empty()) { value.profile = DnsProfile(result.value("profile", 0)); value.servers = result.at("servers").get<std::vector<std::string>>(); value.nextDnsId = result.value("nextdns", ""); }
    callback(value);
  });
}
void RemoteNetwork::setDnsOverride(const DnsOverride& value, DnsOverrideCallback complete) {
  desktopClient().callAsync("network", "SetDns", g_variant_new("(@a{sv})", toVariant({{"profile", int(value.profile)}, {"servers", value.servers}, {"nextdns", value.nextDnsId}})),
    [complete](Json result, std::string error) { if (complete) complete(error.empty() && result.value("success", false), error.empty() ? result.value("message", "") : "DNS could not be changed"); });
}
}
