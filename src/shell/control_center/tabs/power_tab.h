#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Button;
class ConfigService;
class Flex;
class Glyph;
class IdleInhibitor;
class Label;
class ProgressBar;
class Renderer;
class Segmented;
class Separator;
class SessionActionRunner;
class Toggle;
class UPowerService;
class PowerProfilesService;
class PowerTabTestAccess;
struct ControlCenterServices;
struct UPowerChargeLimitState;

class PowerTab : public Tab {
public:
  PowerTab(UPowerService* upower, PowerProfilesService* powerProfiles, const ControlCenterServices* services = nullptr);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;

private:
  friend class PowerTabTestAccess;

  enum class ChargeLimitMode : std::uint8_t {
    Unsupported,
    UPowerActive,
    UPowerDisabled,
    ExternallyManaged,
    FirmwareManaged,
    ReadOnly,
  };

  struct ChargeLimitControlState {
    bool visible = false;
    bool checked = false;
    bool enabled = false;

    bool operator==(const ChargeLimitControlState&) const = default;
  };

  [[nodiscard]] static ChargeLimitMode classifyChargeLimit(const UPowerChargeLimitState& state) noexcept;
  [[nodiscard]] static ChargeLimitControlState chargeLimitControlState(const UPowerChargeLimitState& state) noexcept;
  [[nodiscard]] static bool shouldShowChargeLimit(const UPowerChargeLimitState& state) noexcept;

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;

  void buildSessionCard(Flex& root, float scale);
  void syncSessionCard();
  void runSessionAction(std::string_view action);
  void buildStatusCard(Flex& root, float scale);
  void buildChargingCard(Flex& root, float scale);
  void buildProfilesCard(Flex& root, float scale);
  void buildHealthCard(Flex& root, float scale);
  void buildPeripheralsCard(Flex& root, float scale);

  void syncBatteryStatus();
  void rebuildChargeLimits();
  void syncPowerProfiles();
  void syncBatteryHealth();
  void rebuildPeripherals();
  void syncBatteryWell();

  UPowerService* m_upower = nullptr;
  PowerProfilesService* m_powerProfiles = nullptr;
  ConfigService* m_config = nullptr;
  IdleInhibitor* m_idleInhibitor = nullptr;
  SessionActionRunner* m_session = nullptr;
  std::unique_ptr<SettingsPane> m_settings;

  Flex* m_root = nullptr;

  Flex* m_sessionCard = nullptr;
  Separator* m_caffeineDivider = nullptr;
  Button* m_lockButton = nullptr;
  Button* m_suspendButton = nullptr;
  Button* m_logoutButton = nullptr;
  Button* m_rebootButton = nullptr;
  Button* m_shutdownButton = nullptr;
  Flex* m_powerActionsRow = nullptr;
  Toggle* m_caffeineToggle = nullptr;
  Flex* m_caffeineRow = nullptr;
  bool m_syncingCaffeine = false;

  Flex* m_batteryWell = nullptr;
  Separator* m_profilesDivider = nullptr;
  Separator* m_chargingDivider = nullptr;
  Separator* m_healthDivider = nullptr;
  Separator* m_peripheralsDivider = nullptr;

  // Battery status
  Flex* m_statusCard = nullptr;
  Glyph* m_statusGlyph = nullptr;
  Label* m_percentLabel = nullptr;
  Label* m_stateLabel = nullptr;
  ProgressBar* m_levelBar = nullptr;
  Flex* m_timeRow = nullptr;
  Label* m_timeLabel = nullptr;
  Flex* m_rateRow = nullptr;
  Label* m_rateLabel = nullptr;

  // Battery charging limits
  Flex* m_chargingCard = nullptr;
  Flex* m_chargingList = nullptr;
  struct ChargeLimitRow {
    Flex* row = nullptr;
    Label* nameLabel = nullptr;
    Label* behaviorLabel = nullptr;
    Label* configuredLabel = nullptr;
    Label* managementLabel = nullptr;
    Label* errorLabel = nullptr;
    Flex* controlRow = nullptr;
    Label* controlLabel = nullptr;
    Toggle* toggle = nullptr;
  };
  std::vector<ChargeLimitRow> m_chargeLimitRows;
  std::string m_lastChargeLimitKey;

  // Power profiles
  Flex* m_profilesCard = nullptr;
  Segmented* m_profiles = nullptr;
  Flex* m_inhibitedRow = nullptr;
  Label* m_inhibitedLabel = nullptr;
  std::vector<std::string> m_profileOrder;
  bool m_syncingProfiles = false;

  // Battery health
  Flex* m_healthCard = nullptr;
  Label* m_healthLabel = nullptr;
  ProgressBar* m_healthBar = nullptr;

  // Peripheral batteries
  Flex* m_peripheralsCard = nullptr;
  Flex* m_peripheralsList = nullptr;

  struct PeripheralRow {
    std::string path;
    Flex* row = nullptr;
    Label* nameLabel = nullptr;
    Label* pctLabel = nullptr;
    ProgressBar* bar = nullptr;
  };
  std::vector<PeripheralRow> m_peripheralRows;
  std::string m_lastPeripheralKey;
};
