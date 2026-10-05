#pragma once
#include "ui/controls/scroll_view.h"

#include "core/timer_manager.h"
#include "dbus/network/dns_override.h"
#include "dbus/network/enterprise_credentials.h"
#include "dbus/network/network_secret_agent.h"
#include "dbus/network/network_types.h"
#include "net/http_client.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "util/qr_code.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class AccessPointRow;
class Button;
class CellularRow;
class ClipboardService;
class ConfigService;
class ExternalIpService;
class Flex;
class HttpClient;
class Image;
class InputArea;
class Input;
class Label;
class ModemManagerService;
class ScrollView;
class Select;
class Segmented;
class Spinner;
class SystemMonitorService;
class Toggle;
class INetworkService;
struct ControlCenterServices;

class NetworkTab : public Tab {
public:
  explicit NetworkTab(const ControlCenterServices& services);
  ~NetworkTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;

  void syncCurrentCard();
  void beginPendingAction(bool wasConnected);
  void requestWirelessEnabled(bool enabled);
  void handleWirelessEnabledCompletion(std::uint64_t generation, bool success);
  void rebuildApList(Renderer& renderer);
  bool syncApRows();
  bool syncCellularCard();
  [[nodiscard]] bool cellularToggleChecked() const;
  [[nodiscard]] bool cellularToggleDisplayChecked() const;
  void requestCellularEnabled(bool enabled);
  void syncPasswordCard();
  void showPasswordPrompt(const NetworkSecretAgent::SecretRequest& request);
  void showPasswordPrompt(const AccessPointInfo& ap);
  void submitPasswordPrompt(const std::string& value);
  void cancelPasswordPrompt();
  void clearPasswordPrompt();
  [[nodiscard]] network_enterprise::EnterpriseCredentials
  collectEnterpriseCredentials(const std::string& password) const;
  void setCredentialError(const std::string& message);
  [[nodiscard]] std::string enterpriseBlockReason(const AccessPointInfo& ap) const;
  [[nodiscard]] std::string
  structureKey(const std::vector<AccessPointInfo>& aps, const std::vector<VpnConnectionInfo>& vpns) const;
  void applyAccessPointFilters(std::vector<AccessPointInfo>& aps) const;
  void startPing();
  void startSpeedTest();
  void cancelSpeedTest();
  void copyIpv4();
  void shareWifi();
  void hideWifiQr();
  void applyWifiShare(const std::string& payload, const std::string& hint);
  void uploadQr(Renderer& renderer);
  void refreshDnsOverride();
  void applySelectedDnsProfile();
  void applyDnsOverride(const DnsOverride& override);
  void syncDnsPicker();
  void persistDnsFields();

  enum class Catalog : std::uint8_t { Wifi, Vpn, Cellular };
  void applyCatalog(std::size_t index);
  void persistCatalog();
  void syncCatalogBar(bool hasVpns, bool hasCellular);
  [[nodiscard]] std::vector<Catalog> catalogOrder(bool hasVpns, bool hasCellular) const;
  enum class Page : std::uint8_t { Root, WifiSettings, ConnectionDetails };
  void openPage(Page page);
  void syncPage();

  INetworkService* m_network = nullptr;
  NetworkSecretAgent* m_secrets = nullptr;
  ExternalIpService* m_externalIpService = nullptr;
  ModemManagerService* m_modem = nullptr;
  ConfigService* m_config = nullptr;
  HttpClient* m_http = nullptr;
  SystemMonitorService* m_sysmon = nullptr;
  ClipboardService* m_clipboard = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  std::unique_ptr<SettingsPane> m_wifiSettingsPane;
  Page m_page = Page::Root;

  Flex* m_rootLayout = nullptr;
  Flex* m_rootPage = nullptr;
  Flex* m_subHeader = nullptr;
  Label* m_subTitle = nullptr;
  ScrollView* m_subScroll = nullptr;
  Flex* m_wifiSettingsPage = nullptr;
  Flex* m_connectionDetailsPage = nullptr;
  Label* m_connectionStatus = nullptr;
  Label* m_detailsTitle = nullptr;
  InputArea* m_returnFocus = nullptr;
  Flex* m_currentCard = nullptr;
  Flex* m_catalogHost = nullptr;
  Segmented* m_catalogSwitch = nullptr;
  Catalog m_catalog = Catalog::Wifi;
  std::vector<Catalog> m_catalogOrder;
  bool m_catalogSyncing = false;
  Label* m_currentTitle = nullptr;
  Label* m_currentDetail = nullptr;
  Label* m_throughputLabel = nullptr;
  Label* m_dnsLabel = nullptr;
  Select* m_dnsProfilePicker = nullptr;
  Flex* m_dnsNextDnsRow = nullptr;
  Flex* m_dnsCustomRow = nullptr;
  Input* m_dnsNextDnsInput = nullptr;
  Input* m_dnsCustomInput = nullptr;
  Button* m_dnsApplyButton = nullptr;
  Label* m_toolsStatus = nullptr;
  Button* m_pingButton = nullptr;
  Button* m_speedButton = nullptr;
  Button* m_copyIpButton = nullptr;
  Button* m_shareButton = nullptr;
  Flex* m_qrBlock = nullptr;
  Image* m_qrImage = nullptr;
  Button* m_qrCloseButton = nullptr;
  Label* m_qrHint = nullptr;
  Flex* m_passwordCard = nullptr;
  Label* m_passwordTitle = nullptr;
  Input* m_passwordInput = nullptr;
  Button* m_passwordRevealButton = nullptr;
  bool m_passwordRevealed = false;
  Flex* m_enterpriseFields = nullptr;
  Select* m_eapSelect = nullptr;
  Select* m_phase2Select = nullptr;
  Input* m_identityInput = nullptr;
  Input* m_anonymousIdentityInput = nullptr;
  Input* m_caCertInput = nullptr;
  Input* m_domainMatchInput = nullptr;
  Label* m_credentialError = nullptr;
  ScrollView* m_liveScroll = nullptr;
  ScrollViewState m_liveScrollState{};
  Flex* m_list = nullptr;

  Button* m_rescanButton = nullptr;
  Toggle* m_wifiToggle = nullptr;
  Flex* m_currentRow = nullptr;
  Button* m_disconnectButton = nullptr;
  Spinner* m_scanSpinner = nullptr;
  bool m_vpnVisible = true;

  std::unordered_map<std::string, AccessPointRow*> m_apRows;

  Toggle* m_cellularToggle = nullptr;
  std::vector<CellularRow*> m_cellularRows;

  std::string m_lastStructureKey;
  float m_lastListWidth = -1.0F;

  bool m_hasPendingSecret = false;
  bool m_pendingEnterprise = false;
  std::string m_pendingSsid;
  std::optional<AccessPointInfo> m_pendingAccessPoint;
  bool m_active = false;

  bool m_actionPending = false;
  bool m_actionPendingConnected = false;
  std::chrono::steady_clock::time_point m_actionPendingSince;

  bool m_wifiTogglePending = false;
  bool m_wifiToggleTarget = false;
  bool m_wifiToggleWriteComplete = false;
  bool m_wifiToggleTargetObserved = false;
  std::uint64_t m_wifiToggleRequestGeneration = 0;

  bool m_cellularTogglePending = false;
  bool m_cellularToggleTarget = false;
  std::chrono::steady_clock::time_point m_cellularTogglePendingSince;

  Timer m_actionPendingTimer;
  Timer m_cellularTogglePendingTimer;
  bool m_pingRunning = false;
  std::uint64_t m_pingGeneration = 0;
  std::uint64_t m_speedGeneration = 0;
  std::uint64_t m_shareGeneration = 0;
  bool m_shareRunning = false;
  HttpClient::StreamId m_speedStream = 0;
  std::size_t m_speedBytes = 0;
  std::chrono::steady_clock::time_point m_speedStarted;
  bool m_speedRunning = false;
  qr_code::Bitmap m_qrBitmap;
  bool m_qrDirty = false;
  bool m_qrVisible = false;
  DnsProfile m_dnsProfile = DnsProfile::Automatic;
  bool m_dnsSyncing = false;
  bool m_dnsRunning = false;
  std::uint64_t m_dnsGeneration = 0;
  std::string m_dnsConnectionKey;

  static constexpr std::chrono::seconds kActionPendingTimeout = std::chrono::seconds(6);
  static constexpr std::chrono::seconds kCellularPendingTimeout = std::chrono::seconds(25);
};
