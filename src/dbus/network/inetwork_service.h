#pragma once

#include "dbus/network/dns_override.h"
#include "dbus/network/enterprise_credentials.h"
#include "dbus/network/network_types.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

// Abstract interface shared by the NetworkManager, wpa_supplicant, and iwd
// backends. UI code should use this type so it works with any backend.
class IpcService;

class INetworkService {
public:
  using ChangeCallback = std::function<void(const NetworkState&, NetworkChangeOrigin)>;
  using WirelessFeedbackCallback = std::function<void(bool enabled)>;
  using WirelessEnabledCompletion = std::function<void(bool success)>;

  virtual ~INetworkService() = default;

  virtual void setChangeCallback(ChangeCallback callback) = 0;
  virtual void refresh() = 0;

  [[nodiscard]] virtual const NetworkState& state() const noexcept = 0;
  [[nodiscard]] virtual bool hasStateSnapshot() const noexcept = 0;
  [[nodiscard]] virtual const std::vector<AccessPointInfo>& accessPoints() const noexcept = 0;
  [[nodiscard]] virtual const std::vector<VpnConnectionInfo>& vpnConnections() const noexcept = 0;

  virtual void requestScan() = 0;
  virtual bool activateAccessPoint(const AccessPointInfo& ap) = 0;
  virtual bool activateAccessPoint(const AccessPointInfo& ap, const std::string& psk) = 0;

  // 802.1X association. Backends that cannot build an EAP profile keep the
  // defaults, and the UI offers the enterprise form only where it is supported.
  [[nodiscard]] virtual bool supportsEnterprise() const noexcept { return false; }
  virtual bool activateEnterpriseAccessPoint(
      const AccessPointInfo& /*ap*/, const network_enterprise::EnterpriseCredentials& /*credentials*/
  ) {
    return false;
  }
  virtual bool activateVpnConnection(const VpnConnectionInfo& vpn) = 0;
  virtual bool deactivateVpnConnection(const VpnConnectionInfo& vpn) = 0;
  [[nodiscard]] virtual bool canActivateWiredConnection() const noexcept { return false; }
  virtual bool activateWiredConnection() { return false; }
  // GNOME-style mobile-data control over a saved cellular (gsm) connection.
  // Activation brings up the modem and the data connection; deactivation drops
  // the data connection but leaves the modem registered. Only backends that own
  // cellular profiles (NetworkManager) implement this.
  [[nodiscard]] virtual bool canActivateCellularConnection() const noexcept { return false; }
  virtual bool activateCellularConnection() { return false; }
  virtual bool deactivateCellularConnection() { return false; }
  virtual void setWirelessEnabled(bool enabled, WirelessEnabledCompletion onComplete = {}) = 0;
  virtual void disconnect() = 0;
  virtual void forgetSsid(const std::string& ssid) = 0;
  [[nodiscard]] virtual bool hasSavedConnection(const std::string& ssid) const = 0;
  [[nodiscard]] virtual bool supportsSecretAgent() const noexcept { return false; }

  // Stored PSK (or WEP key) for the active Wi-Fi profile. NetworkManager can
  // return it via GetSecrets; other backends call back with nullopt.
  using WifiPskCallback = std::function<void(std::optional<std::string> psk)>;
  virtual void requestActiveWifiPsk(WifiPskCallback callback) {
    if (callback) {
      callback(std::nullopt);
    }
  }

  using DnsOverrideCallback = std::function<void(bool success, std::string message)>;
  using DnsOverrideQueryCallback = std::function<void(DnsOverride override)>;
  [[nodiscard]] virtual bool supportsDnsOverride() const noexcept { return false; }
  virtual void requestActiveDnsOverride(DnsOverrideQueryCallback callback) {
    if (callback) {
      callback(DnsOverride{});
    }
  }
  virtual void setDnsOverride(const DnsOverride& /*override*/, DnsOverrideCallback onComplete = {}) {
    if (onComplete) {
      onComplete(false, "DNS override requires NetworkManager");
    }
  }

  void registerIpc(IpcService& ipc, WirelessFeedbackCallback wirelessFeedback = {});
};
