#pragma once
#include "dbus/network/inetwork_service.h"
#include "core/timer_manager.h"
#include "desktop_api/client.h"

namespace helix {
class RemoteNetwork final : public INetworkService {
public:
  RemoteNetwork();
  ~RemoteNetwork() override;
  void setChangeCallback(ChangeCallback callback) override { m_callback = std::move(callback); }
  void refresh() override;
  const NetworkState& state() const noexcept override { return m_state; }
  bool hasStateSnapshot() const noexcept override { return m_available; }
  const std::vector<AccessPointInfo>& accessPoints() const noexcept override { return m_aps; }
  const std::vector<VpnConnectionInfo>& vpnConnections() const noexcept override { return m_vpns; }
  void requestScan() override;
  bool activateAccessPoint(const AccessPointInfo& ap) override;
  bool activateAccessPoint(const AccessPointInfo& ap, const std::string& psk) override;
  bool supportsEnterprise() const noexcept override { return true; }
  bool activateEnterpriseAccessPoint(const AccessPointInfo&, const network_enterprise::EnterpriseCredentials&) override;
  bool activateVpnConnection(const VpnConnectionInfo& vpn) override;
  bool deactivateVpnConnection(const VpnConnectionInfo& vpn) override;
  bool canActivateWiredConnection() const noexcept override { return m_wired; }
  bool activateWiredConnection() override;
  bool canActivateCellularConnection() const noexcept override { return m_cellular; }
  bool activateCellularConnection() override;
  bool deactivateCellularConnection() override;
  void setWirelessEnabled(bool enabled, WirelessEnabledCompletion complete = {}) override;
  void disconnect() override;
  void forgetSsid(const std::string& ssid) override;
  bool hasSavedConnection(const std::string& ssid) const override;
  bool supportsSecretAgent() const noexcept override { return true; }
  bool supportsDnsOverride() const noexcept override { return true; }
  void requestActiveWifiPsk(WifiPskCallback callback) override;
  void requestActiveDnsOverride(DnsOverrideQueryCallback callback) override;
  void setDnsOverride(const DnsOverride&, DnsOverrideCallback complete = {}) override;
private:
  bool action(const char* name, const Json& arguments);
  NetworkState m_state;
  std::vector<AccessPointInfo> m_aps;
  std::vector<VpnConnectionInfo> m_vpns;
  std::vector<std::string> m_saved;
  ChangeCallback m_callback;
  bool m_available = false;
  bool m_wired = false;
  bool m_cellular = false;
  bool m_refreshing = false;
  unsigned m_observer = 0;
  Timer m_refreshTimer;
};
}
