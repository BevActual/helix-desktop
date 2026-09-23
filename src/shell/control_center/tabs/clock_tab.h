#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class ConfigService;
class Flex;
class Label;
class ScrollView;
class SearchPicker;
struct ControlCenterServices;

class ClockTab : public Tab {
public:
  explicit ClockTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void rebuildZones();
  void syncZoneTimes();
  void syncPicker();
  void addTimezone(std::string_view raw);
  void removeTimezone(std::size_t index);
  void persistTimezones(const std::vector<std::string>& timezones);
  void setTimezoneError(std::string_view message);
  void syncLocalClock();

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Flex* m_rootLayout = nullptr;
  Flex* m_compactCard = nullptr;
  Label* m_localTime = nullptr;
  Label* m_localDate = nullptr;
  Label* m_localZone = nullptr;
  Flex* m_zoneList = nullptr;
  Flex* m_emptyState = nullptr;
  ScrollView* m_zoneScroll = nullptr;
  Label* m_timezoneError = nullptr;
  SearchPicker* m_timezonePicker = nullptr;
  struct ZoneRow {
    std::string zone;
    Label* timeLabel = nullptr;
    Label* relativeLabel = nullptr;
  };
  std::vector<ZoneRow> m_zoneRows;
  std::string m_lastTimezoneKey;
  Timer m_clockTimer;
};
