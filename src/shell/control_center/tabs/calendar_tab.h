#pragma once

#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/controls/calendar_view.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Button;
class CalendarService;
class ConfigService;
class InputArea;
class Label;
class ScrollView;
class Segmented;
class Toggle;
struct ControlCenterServices;

class CalendarTab : public Tab {
public:
  CalendarTab(ConfigService* config, CalendarService* calendar, const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;

private:
  enum class Catalog : std::uint8_t { Month, Events, Accounts };

  void focusToday();
  void changeMonthBy(int delta);
  void cancelMonthSlide();
  void beginSlideOut(int delta);
  void beginSlideIn();
  void applyMonthSlide(float progress, bool slidingIn);
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void rebuild();
  void rebuildEventList(float scale);
  void buildAccountsCard(Flex& parent, float scale);
  void rebuildAccounts();
  void syncAccounts();
  void openAccount(std::optional<std::string> accountId);
  void setCalendarEnabled(bool enabled);
  void applyCatalog(std::size_t index, bool persist = true);
  void persistCatalog();
  void syncCatalog();
  [[nodiscard]] bool syncDetailLabel();
  [[nodiscard]] static Catalog catalogFromState(std::string_view value) noexcept;
  [[nodiscard]] static std::string_view catalogState(Catalog catalog) noexcept;
  [[nodiscard]] static std::size_t indexFromCatalog(Catalog catalog) noexcept;
  [[nodiscard]] static Catalog catalogFromIndex(std::size_t index) noexcept;

  ConfigService* m_config = nullptr;
  CalendarService* m_calendar = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  bool m_changeCallbackRegistered = false;
  bool m_eventsDirty = false;
  Flex* m_rootLayout = nullptr;
  Flex* m_compactCard = nullptr;
  Segmented* m_catalogSwitch = nullptr;
  Flex* m_enableRow = nullptr;
  Toggle* m_enableToggle = nullptr;
  Flex* m_monthHost = nullptr;
  Flex* m_eventsHost = nullptr;
  Flex* m_accountsHost = nullptr;
  InputArea* m_calendarArea = nullptr;
  Flex* m_card = nullptr;
  Flex* m_header = nullptr;
  Flex* m_previousSlot = nullptr;
  Flex* m_nextSlot = nullptr;
  Flex* m_monthWrap = nullptr;
  Label* m_todayLabel = nullptr;
  Label* m_detailLabel = nullptr;
  Label* m_monthLabel = nullptr;
  Button* m_previousButton = nullptr;
  Button* m_nextButton = nullptr;
  Flex* m_gridViewport = nullptr;
  Flex* m_grid = nullptr;
  ScrollView* m_eventsScroll = nullptr;
  Flex* m_accountsCard = nullptr;
  Button* m_accountsAddButton = nullptr;
  Flex* m_accountsList = nullptr;
  Label* m_accountsEmpty = nullptr;
  std::string m_lastAccountsKey;
  calendar_view::EventListState m_eventListState;
  Catalog m_catalog = Catalog::Month;
  Catalog m_pinnedCatalog = Catalog::Month;
  bool m_catalogSyncing = false;
  int m_selectedYear = std::numeric_limits<int>::min();
  int m_selectedMonth = -1;
  int m_selectedDay = -1;
  int m_monthOffset = 0;
  float m_lastInnerWidth = -1.0F;
  float m_lastInnerHeight = -1.0F;
  int m_lastDisplayYear = std::numeric_limits<int>::min();
  int m_lastDisplayMonth = -1;
  int m_lastCurrentYear = std::numeric_limits<int>::min();
  int m_lastCurrentMonth = -1;
  int m_lastToday = -1;
  int m_monthSlideDirection = 0;
  int m_pendingMonthDelta = 0;
  bool m_startMonthSlideIn = false;
  AnimationManager::Id m_monthSlideAnimId = 0;
  bool m_showWeekNumbers = false;
  Timer m_eventFadeTimer;
};
