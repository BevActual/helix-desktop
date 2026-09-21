#include "shell/control_center/tabs/calendar_tab.h"

#include "calendar/calendar_service.h"
#include "config/config_service.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/animation/animation.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_manager.h"
#include "system/desktop_entry_launch.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/calendar_view.h"
#include "ui/controls/flex.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/toggle.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>
#include <wayland-client-protocol.h>

using namespace control_center;

namespace {

  constexpr float kCalendarGridGap = Style::spaceSm;
  constexpr float kCalendarNavButtonSize = Style::controlHeight;
  constexpr float kCalendarWeekdayRowHeight = Style::fontSizeCaption + Style::spaceXs;
  constexpr float kCalendarHeaderHeight = Style::controlHeight;
  constexpr float kCalendarCellSizeMin = Style::controlHeightSm + Style::spaceXs;
  constexpr float kCalendarCellSizeMax = Style::controlHeightLg + Style::spaceXs;
  constexpr float kCalendarDayButtonSizeMax = Style::controlHeightLg;
  constexpr float kCalendarLayoutEpsilon = 0.5F;
  // Week-number column: a fraction of a day column so it stays visually subordinate, floored at the
  // width of the two digits it holds (as a multiple of the caption font size) so it can never clip.
  constexpr float kCalendarWeekColumnRatio = 0.55F;
  constexpr float kCalendarWeekColumnMinFontScale = 1.45F;

  [[nodiscard]] bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return changed;
  }

  std::string formatShellDate(const ConfigService* config) {
    const char* format = config != nullptr ? config->config().shell.dateFormat.c_str() : "%A, %x";
    return formatLocalTime(format);
  }

  [[nodiscard]] std::string formatSelectedDay(const ConfigService* config, int year, int month, int day) {
    if (year == std::numeric_limits<int>::min() || month < 0 || day < 0) {
      return {};
    }
    std::tm selected{};
    selected.tm_year = year - 1900;
    selected.tm_mon = month;
    selected.tm_mday = day;
    selected.tm_isdst = -1;
    const std::time_t selectedRaw = std::mktime(&selected);
    const char* format = config != nullptr ? config->config().calendar.eventDateFormat.c_str() : "%A %e %B";
    return formatLocalUnixTime(static_cast<std::int64_t>(selectedRaw), format);
  }

  [[nodiscard]] bool isManagedCalendarAccount(const CalendarConfig::Account& account) {
    return account.type == "google" || account.type == "caldav" || account.type == "ics" || account.type == "vdir";
  }

  [[nodiscard]] std::string calendarAccountTypeLabel(const CalendarConfig::Account& account) {
    if (account.type == "google") {
      return i18n::tr("settings.calendar-accounts.provider.google");
    }
    if (account.type == "ics") {
      return i18n::tr("settings.calendar-accounts.provider.ics");
    }
    if (account.type == "vdir") {
      return i18n::tr("settings.calendar-accounts.provider.vdir");
    }
    if (account.provider == "custom") {
      return i18n::tr("settings.calendar-accounts.provider.custom");
    }
    return i18n::tr("settings.calendar-accounts.provider.icloud");
  }

  [[nodiscard]] std::string calendarAccountsKey(const CalendarConfig& calendar, CalendarService* service) {
    std::string key;
    key += calendar.enabled ? '1' : '0';
    key.push_back('\n');
    for (const auto& account : calendar.accounts) {
      if (!isManagedCalendarAccount(account)) {
        continue;
      }
      key += account.id;
      key.push_back('\t');
      key += account.type;
      key.push_back('\t');
      key += account.displayName;
      key.push_back('\t');
      key += account.provider;
      if (service != nullptr && account.type == "google") {
        if (service->googleAccountCredentialLocked(account.id)) {
          key += "\tlocked";
        } else if (service->googleAccountNeedsReconnect(account.id)) {
          key += "\treconnect";
        }
      }
      key.push_back('\n');
    }
    return key;
  }

  void requestPanelLayout() {
    if (auto* panels = PanelManager::current(); panels != nullptr) {
      panels->requestLayout();
    }
  }
} // namespace

CalendarTab::CalendarTab(ConfigService* config, CalendarService* calendar, const ControlCenterServices& services)
    : m_config(config), m_calendar(calendar),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "services", .includedGroups = {"calendar"}}
      )) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> catalog = m_config->stateString("calendar_tab", "catalog")) {
      m_catalog = catalogFromState(*catalog);
      m_pinnedCatalog = m_catalog;
    }
  }
}

std::unique_ptr<Flex> CalendarTab::create() {
  const float scale = contentScale();
  const float controlHeight = Style::controlHeight * scale;

  if (m_calendar != nullptr && !m_changeCallbackRegistered) {
    m_changeCallbackRegistered = true;
    (void)m_calendar->addChangeCallback([this]() {
      m_eventsDirty = true;
      PanelManager::instance().refresh();
    });
  }

  if (m_config != nullptr) {
    m_showWeekNumbers = m_config->config().controlCenter.calendarTab.showWeekNumbers;
  }

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto compactCard = ui::column({
      .out = &m_compactCard,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) {
        applyCompactCardStyle(card, scale, opacity, false);
      },
  });

  auto titleRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .minHeight = controlHeight,
  });
  titleRow->addChild(
      ui::label({
          .out = &m_todayLabel,
          .text = formatShellDate(m_config),
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .flexGrow = 1.0F,
          .configure =
              [this](Label& label) {
                label.setHitTestVisible(true);
                label.setTooltip(i18n::tr("control-center.calendar.today"));
                label.setOnClick([this](const InputArea::PointerData&) {
                  focusToday();
                  applyCatalog(indexFromCatalog(Catalog::Month));
                  PanelManager::instance().refresh();
                });
              },
      })
  );
  titleRow->addChild(
      ui::button({
          .out = &m_accountsAddButton,
          .glyph = "plus",
          .glyphSize = Style::baseGlyphSize * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.calendar.accounts.add"),
          .minHeight = controlHeight,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusSm(scale),
          .visible = false,
          .participatesInLayout = false,
          .onClick = [this]() { openAccount(std::nullopt); },
      })
  );
  compactCard->addChild(std::move(titleRow));
  compactCard->addChild(
      ui::segmented({
          .out = &m_catalogSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = "",
                      .glyph = "calendar-month",
                      .tooltip = i18n::tr("control-center.calendar.sections.month"),
                  },
                  {
                      .label = "",
                      .glyph = "calendar-event",
                      .tooltip = i18n::tr("control-center.calendar.sections.events"),
                  },
                  {
                      .label = "",
                      .glyph = "users",
                      .tooltip = i18n::tr("control-center.calendar.sections.accounts"),
                  },
              },
          .selectedIndex = indexFromCatalog(m_catalog),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .onChange = [this](std::size_t index) { applyCatalog(index, true); },
      })
  );
  compactCard->addChild(
      ui::label({
          .out = &m_detailLabel,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
      })
  );

  auto enableRow = ui::row({
      .out = &m_enableRow,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .visible = false,
      .participatesInLayout = false,
  });
  enableRow->addChild(
      ui::label({
          .text = i18n::tr("control-center.calendar.accounts.disabled"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0F,
      })
  );
  enableRow->addChild(
      ui::toggle({
          .out = &m_enableToggle,
          .checked = m_calendar != nullptr && m_calendar->enabled(),
          .toggleSize = ToggleSize::Medium,
          .scale = scale,
          .onChange = [this](bool value) { setCalendarEnabled(value); },
      })
  );
  compactCard->addChild(std::move(enableRow));
  tab->addChild(std::move(compactCard));

  auto monthHost = ui::column({
      .out = &m_monthHost,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
      .configure = [scale](Flex& host) { applyContentCardStyle(host, scale); },
  });
  auto calendarArea = ui::inputArea({});
  calendarArea->setFlexGrow(1.0F);
  calendarArea->setOnAxis([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return;
    }
    const float steps = data.scrollSteps();
    if (steps == 0.0F) {
      return;
    }
    changeMonthBy(steps > 0.0F ? 1 : -1);
  });
  m_calendarArea = calendarArea.get();

  auto calendarCard = ui::column({
      .out = &m_card,
      .align = FlexAlign::Stretch,
      .gap = kCalendarGridGap * scale,
      .flexGrow = 1.0F,
  });

  auto header = ui::row({
      .out = &m_header,
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceSm * scale,
      .minHeight = kCalendarHeaderHeight * scale,
  });
  header->addChild(ui::row(
      {.out = &m_previousSlot, .align = FlexAlign::Center, .justify = FlexJustify::Center},
      ui::button({
          .out = &m_previousButton,
          .glyph = Style::rtl() ? "chevron-right" : "chevron-left",
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.calendar.previous-month"),
          .minWidth = kCalendarNavButtonSize * scale,
          .minHeight = kCalendarNavButtonSize * scale,
          .onClick = [this]() { changeMonthBy(-1); },
      })
  ));
  header->addChild(ui::column(
      {.out = &m_monthWrap, .align = FlexAlign::Center, .justify = FlexJustify::Center, .flexGrow = 1.0F},
      ui::label({
          .out = &m_monthLabel,
          .fontSize = (Style::fontSizeTitle + Style::spaceXs) * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
      })
  ));
  header->addChild(ui::row(
      {.out = &m_nextSlot, .align = FlexAlign::Center, .justify = FlexJustify::Center},
      ui::button({
          .out = &m_nextButton,
          .glyph = Style::rtl() ? "chevron-left" : "chevron-right",
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.calendar.next-month"),
          .minWidth = kCalendarNavButtonSize * scale,
          .minHeight = kCalendarNavButtonSize * scale,
          .onClick = [this]() { changeMonthBy(1); },
      })
  ));
  calendarCard->addChild(std::move(header));

  auto gridViewport = ui::column({
      .out = &m_gridViewport,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
      .configure = [](Flex& viewport) { viewport.setClipChildren(true); },
  });
  gridViewport->addChild(ui::column({
      .out = &m_grid,
      .align = FlexAlign::Stretch,
      .gap = kCalendarGridGap * scale,
      .configure = [](Flex& layer) { layer.setParticipatesInLayout(false); },
  }));
  calendarCard->addChild(std::move(gridViewport));
  calendarArea->addChild(std::move(calendarCard));
  monthHost->addChild(std::move(calendarArea));
  tab->addChild(std::move(monthHost));

  auto eventsHost = ui::column({
      .out = &m_eventsHost,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
      .configure = [scale](Flex& host) { applyContentCardStyle(host, scale); },
  });
  eventsHost->addChild(ui::scrollView({
      .out = &m_eventsScroll,
      .contentScale = scale,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = 1.0F,
      .configure =
          [](ScrollView& scrollView) {
            scrollView.clearFill();
            scrollView.clearBorder();
          },
  }));
  tab->addChild(std::move(eventsHost));

  auto accountsHost = ui::column({
      .out = &m_accountsHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
      .configure = [scale](Flex& host) { applyContentCardStyle(host, scale); },
  });
  buildAccountsCard(*accountsHost, scale);
  tab->addChild(std::move(accountsHost));

  syncCatalog();
  return m_settings->wrapLive(std::move(tab), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> CalendarTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void CalendarTab::buildAccountsCard(Flex& parent, float scale) {
  auto card = ui::column({
      .out = &m_accountsCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
  });
  card->addChild(
      ui::label({
          .out = &m_accountsEmpty,
          .text = i18n::tr("control-center.calendar.accounts.empty"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
      })
  );
  auto listScroll = ui::scrollView({
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure =
          [](ScrollView& scrollView) {
            scrollView.clearFill();
            scrollView.clearBorder();
          },
  });
  m_accountsList = listScroll->content();
  if (m_accountsList != nullptr) {
    m_accountsList->setDirection(FlexDirection::Vertical);
    m_accountsList->setAlign(FlexAlign::Stretch);
    m_accountsList->setGap(Style::spaceXs * scale);
  }
  card->addChild(std::move(listScroll));
  parent.addChild(std::move(card));
  rebuildAccounts();
  syncAccounts();
}

void CalendarTab::rebuildAccounts() {
  if (m_accountsList == nullptr || m_config == nullptr) {
    return;
  }
  const auto& calendar = m_config->config().calendar;
  const std::string key = calendarAccountsKey(calendar, m_calendar);
  if (key == m_lastAccountsKey) {
    return;
  }

  uiAssertNotRendering("CalendarTab::rebuildAccounts");
  while (!m_accountsList->children().empty()) {
    m_accountsList->removeChild(m_accountsList->children().front().get());
  }

  const float scale = contentScale();
  for (const auto& account : calendar.accounts) {
    if (!isManagedCalendarAccount(account)) {
      continue;
    }
    const bool credentialLocked =
        account.type == "google" && m_calendar != nullptr && m_calendar->googleAccountCredentialLocked(account.id);
    const bool reconnectRequired =
        account.type == "google" && m_calendar != nullptr && m_calendar->googleAccountNeedsReconnect(account.id);
    const std::string_view statusKey = credentialLocked ? "settings.schema.services.calendar-edit.description-locked"
        : reconnectRequired                             ? "settings.schema.services.calendar-edit.description-reconnect"
                                                        : "";
    const std::string_view buttonKey = credentialLocked ? "settings.schema.services.calendar-edit.button-retry"
        : reconnectRequired                             ? "settings.schema.services.calendar-edit.button-reconnect"
                                                        : "settings.schema.services.calendar-edit.button";
    const std::string glyph = credentialLocked ? "key" : reconnectRequired ? "brand-google" : "edit";

    auto item = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * 0.5F * scale,
    });
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .minHeight = Style::controlHeightSm * scale,
    });
    auto text = ui::column({
        .align = FlexAlign::Stretch,
        .gap = 0.0F,
        .flexGrow = 1.0F,
    });
    text->addChild(
        ui::label({
            .text = account.displayName.empty() ? account.id : account.displayName,
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
        })
    );
    text->addChild(
        ui::label({
            .text = statusKey.empty() ? calendarAccountTypeLabel(account) : i18n::tr(statusKey),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(credentialLocked || reconnectRequired ? ColorRole::Error : ColorRole::OnSurfaceVariant),
            .maxLines = 2,
        })
    );
    row->addChild(std::move(text));
    row->addChild(
        ui::button({
            .glyph = glyph,
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr(buttonKey),
            .minWidth = Style::controlHeightSm * scale,
            .minHeight = Style::controlHeightSm * scale,
            .padding = Style::spaceXs * scale,
            .onClick =
                [this, id = account.id, credentialLocked]() {
                  if (credentialLocked && m_calendar != nullptr) {
                    m_calendar->requestRefresh();
                    return;
                  }
                  openAccount(id);
                },
        })
    );
    item->addChild(std::move(row));
    m_accountsList->addChild(std::move(item));
  }

  m_lastAccountsKey = key;
  syncAccounts();
}

void CalendarTab::syncAccounts() {
  const bool enabled = m_calendar != nullptr && m_calendar->enabled();
  const bool hasAccounts = m_accountsList != nullptr && !m_accountsList->children().empty();
  const bool layoutDirty =
      setShown(m_accountsList, enabled && hasAccounts) || setShown(m_accountsEmpty, enabled && !hasAccounts);
  if (layoutDirty) {
    requestPanelLayout();
  }
}

void CalendarTab::openAccount(std::optional<std::string> accountId) {
  if (m_settings != nullptr) {
    m_settings->openCalendarAccount(std::move(accountId));
  }
}

void CalendarTab::setCalendarEnabled(bool enabled) {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"calendar", "enabled"}, enabled);
  m_lastAccountsKey.clear();
  rebuildAccounts();
  syncCatalog();
  PanelManager::instance().refresh();
}

void CalendarTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool CalendarTab::configureMode() const { return m_settings->configureMode(); }

void CalendarTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

SettingsPane* CalendarTab::settingsPane() { return m_settings.get(); }

CalendarTab::Catalog CalendarTab::catalogFromState(std::string_view value) noexcept {
  if (value == "events") {
    return Catalog::Events;
  }
  if (value == "accounts") {
    return Catalog::Accounts;
  }
  return Catalog::Month;
}

std::string_view CalendarTab::catalogState(Catalog catalog) noexcept {
  switch (catalog) {
  case Catalog::Events:
    return "events";
  case Catalog::Accounts:
    return "accounts";
  case Catalog::Month:
    break;
  }
  return "month";
}

std::size_t CalendarTab::indexFromCatalog(Catalog catalog) noexcept {
  switch (catalog) {
  case Catalog::Events:
    return 1;
  case Catalog::Accounts:
    return 2;
  case Catalog::Month:
    break;
  }
  return 0;
}

CalendarTab::Catalog CalendarTab::catalogFromIndex(std::size_t index) noexcept {
  switch (index) {
  case 1:
    return Catalog::Events;
  case 2:
    return Catalog::Accounts;
  default:
    return Catalog::Month;
  }
}

bool CalendarTab::syncDetailLabel() {
  if (m_detailLabel == nullptr) {
    return false;
  }
  const bool show = m_catalog == Catalog::Month || m_catalog == Catalog::Events;
  if (show) {
    m_detailLabel->setText(formatSelectedDay(m_config, m_selectedYear, m_selectedMonth, m_selectedDay));
  }
  return setShown(m_detailLabel, show);
}

void CalendarTab::persistCatalog() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("calendar_tab", "catalog", std::string(catalogState(m_pinnedCatalog)));
}

void CalendarTab::applyCatalog(std::size_t index, bool persist) {
  if (m_catalogSyncing) {
    return;
  }
  const Catalog next = catalogFromIndex(index);
  if (persist) {
    m_pinnedCatalog = next;
    persistCatalog();
  }
  if (next == m_catalog) {
    syncCatalog();
    return;
  }
  m_catalog = next;
  m_eventsDirty = true;
  m_lastInnerWidth = -1.0F;
  m_lastInnerHeight = -1.0F;
  syncCatalog();
  requestPanelLayout();
}

void CalendarTab::syncCatalog() {
  const bool enabled = m_calendar != nullptr && m_calendar->enabled();
  const bool month = m_catalog == Catalog::Month;
  const bool events = m_catalog == Catalog::Events;
  const bool accounts = m_catalog == Catalog::Accounts;
  bool layoutDirty = setShown(m_monthHost, month) || setShown(m_eventsHost, events) || setShown(m_accountsHost, accounts)
      || setShown(m_enableRow, !enabled) || setShown(m_accountsAddButton, accounts && enabled);
  layoutDirty = syncDetailLabel() || layoutDirty;
  if (m_enableToggle != nullptr) {
    m_enableToggle->setChecked(enabled);
  }
  if (m_catalogSwitch != nullptr && m_catalogSwitch->selectedIndex() != indexFromCatalog(m_pinnedCatalog)) {
    m_catalogSyncing = true;
    m_catalogSwitch->setSelectedIndex(indexFromCatalog(m_pinnedCatalog));
    m_catalogSyncing = false;
    layoutDirty = true;
  }
  if (layoutDirty) {
    requestPanelLayout();
  }
}

void CalendarTab::onPanelCardOpacityChanged(float opacity) {
  if (m_catalogSwitch != nullptr) {
    m_catalogSwitch->setSurfaceOpacity(opacity);
  }
}

void CalendarTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  if (m_config != nullptr) {
    const bool weekNumbers = m_config->config().controlCenter.calendarTab.showWeekNumbers;
    if (weekNumbers != m_showWeekNumbers) {
      m_showWeekNumbers = weekNumbers;
      m_lastInnerWidth = -1.0F;
      m_lastInnerHeight = -1.0F;
    }
  }

  syncCatalog();
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);

  if (m_catalog == Catalog::Month && m_card != nullptr && m_calendarArea != nullptr) {
    m_card->setSize(m_calendarArea->width(), m_calendarArea->height());
    m_card->layout(renderer);

    const float innerWidth = std::max(0.0F, m_card->width() - (m_card->paddingLeft() + m_card->paddingRight()));
    const float innerHeight = std::max(0.0F, m_card->height() - (m_card->paddingTop() + m_card->paddingBottom()));
    if (innerWidth < 1.0F || innerHeight < 1.0F) {
      m_rootLayout->layout(renderer);
      return;
    }
    const calendar_view::State state = calendar_view::stateForOffset(m_monthOffset);

    if (m_selectedDay < 0) {
      m_selectedYear = state.current.year;
      m_selectedMonth = state.current.month;
      m_selectedDay = state.current.day;
    }

    const bool sizeChanged = std::abs(innerWidth - m_lastInnerWidth) >= kCalendarLayoutEpsilon
        || std::abs(innerHeight - m_lastInnerHeight) >= kCalendarLayoutEpsilon;
    const bool displayChanged = state.displayYear != m_lastDisplayYear || state.displayMonth != m_lastDisplayMonth;
    const bool todayChanged = state.current.year != m_lastCurrentYear || state.current.month != m_lastCurrentMonth
        || state.current.day != m_lastToday;

    if (m_monthSlideAnimId != 0 && !m_startMonthSlideIn) {
      m_rootLayout->layout(renderer);
      calendar_view::layoutEventLinkOverlays(m_eventListState);
      return;
    }

    if (sizeChanged || displayChanged || todayChanged || m_eventsDirty || m_startMonthSlideIn) {
      m_eventsDirty = false;
      m_lastInnerWidth = innerWidth;
      m_lastInnerHeight = innerHeight;
      m_lastDisplayYear = state.displayYear;
      m_lastDisplayMonth = state.displayMonth;
      m_lastCurrentYear = state.current.year;
      m_lastCurrentMonth = state.current.month;
      m_lastToday = state.current.day;

      rebuild();
      if (m_grid != nullptr) {
        if (m_monthSlideAnimId == 0 && !m_startMonthSlideIn) {
          m_grid->setPosition(0.0F, 0.0F);
          m_grid->setOpacity(1.0F);
        }
        m_grid->layout(renderer);
      }
      if (m_startMonthSlideIn && m_gridViewport != nullptr) {
        m_startMonthSlideIn = false;
        beginSlideIn();
      }
    }
  } else if (m_eventsDirty || m_catalog == Catalog::Events) {
    m_eventsDirty = false;
    rebuildEventList(contentScale());
  }

  m_rootLayout->layout(renderer);
  calendar_view::layoutEventLinkOverlays(m_eventListState);
}

void CalendarTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_todayLabel != nullptr) {
    m_todayLabel->setText(formatShellDate(m_config));
  }
  (void)syncDetailLabel();
  rebuildAccounts();
}

void CalendarTab::setActive(bool active) {
  if (!active) {
    m_eventFadeTimer.stop();
    return;
  }
  if (m_config != nullptr) {
    if (const std::optional<std::string> catalog = m_config->stateString("calendar_tab", "catalog")) {
      m_catalog = catalogFromState(*catalog);
      m_pinnedCatalog = m_catalog;
      syncCatalog();
    }
  }
  focusToday();
  if (!m_eventFadeTimer.active()) {
    m_eventFadeTimer.startRepeating(std::chrono::minutes{1}, [this]() {
      m_eventsDirty = true;
      PanelManager::instance().refresh();
    });
  }
}

void CalendarTab::focusToday() {
  cancelMonthSlide();
  const calendar_view::State state = calendar_view::stateForOffset(0);
  m_monthOffset = 0;
  m_selectedYear = state.current.year;
  m_selectedMonth = state.current.month;
  m_selectedDay = state.current.day;
  m_lastDisplayYear = std::numeric_limits<int>::min();
  m_lastDisplayMonth = -1;
  m_eventsDirty = true;
}

void CalendarTab::onClose() {
  m_settings->onClose();
  m_eventFadeTimer.stop();
  cancelMonthSlide();
  m_rootLayout = nullptr;
  m_compactCard = nullptr;
  m_catalogSwitch = nullptr;
  m_enableRow = nullptr;
  m_enableToggle = nullptr;
  m_monthHost = nullptr;
  m_eventsHost = nullptr;
  m_accountsHost = nullptr;
  m_calendarArea = nullptr;
  m_card = nullptr;
  m_header = nullptr;
  m_previousSlot = nullptr;
  m_nextSlot = nullptr;
  m_monthWrap = nullptr;
  m_todayLabel = nullptr;
  m_detailLabel = nullptr;
  m_monthLabel = nullptr;
  m_previousButton = nullptr;
  m_nextButton = nullptr;
  m_gridViewport = nullptr;
  m_grid = nullptr;
  m_eventsScroll = nullptr;
  m_accountsCard = nullptr;
  m_accountsAddButton = nullptr;
  m_accountsList = nullptr;
  m_accountsEmpty = nullptr;
  m_lastAccountsKey.clear();
  m_eventListState.linkOverlays.clear();
  m_selectedYear = std::numeric_limits<int>::min();
  m_selectedMonth = -1;
  m_selectedDay = -1;
  focusToday();
  m_eventsDirty = false;
  m_lastInnerWidth = -1.0F;
  m_lastInnerHeight = -1.0F;
  m_lastCurrentYear = std::numeric_limits<int>::min();
  m_lastCurrentMonth = -1;
  m_lastToday = -1;
}

void CalendarTab::changeMonthBy(int delta) {
  if (delta == 0) {
    return;
  }

  cancelMonthSlide();

  AnimationManager* animations = m_gridViewport != nullptr ? m_gridViewport->animationManager() : nullptr;
  if (animations == nullptr || m_grid == nullptr) {
    m_monthOffset += delta;
    m_lastDisplayYear = std::numeric_limits<int>::min();
    m_lastDisplayMonth = -1;
    PanelManager::instance().refresh();
    return;
  }

  beginSlideOut(delta);
}

void CalendarTab::cancelMonthSlide() {
  if (m_monthSlideAnimId != 0 && m_gridViewport != nullptr) {
    if (AnimationManager* animations = m_gridViewport->animationManager(); animations != nullptr) {
      animations->cancel(m_monthSlideAnimId);
    }
    m_monthSlideAnimId = 0;
  }
  m_pendingMonthDelta = 0;
  m_startMonthSlideIn = false;
  if (m_grid != nullptr) {
    m_grid->setPosition(0.0F, 0.0F);
    m_grid->setOpacity(1.0F);
  }
}

void CalendarTab::applyMonthSlide(float progress, bool slidingIn) {
  if (m_grid == nullptr || m_gridViewport == nullptr) {
    return;
  }

  const float travel = m_gridViewport->width();
  if (travel <= 0.0F) {
    return;
  }

  const auto direction = static_cast<float>(m_monthSlideDirection);
  if (slidingIn) {
    m_grid->setPosition(direction * travel * (1.0F - progress), 0.0F);
    m_grid->setOpacity(0.7F + 0.3F * progress);
  } else {
    m_grid->setPosition(-direction * travel * progress, 0.0F);
    m_grid->setOpacity(1.0F - 0.3F * progress);
  }
}

void CalendarTab::beginSlideOut(int delta) {
  AnimationManager* animations = m_gridViewport != nullptr ? m_gridViewport->animationManager() : nullptr;
  if (animations == nullptr || m_grid == nullptr) {
    m_monthOffset += delta;
    m_lastDisplayYear = std::numeric_limits<int>::min();
    m_lastDisplayMonth = -1;
    PanelManager::instance().refresh();
    return;
  }

  m_monthSlideDirection = delta > 0 ? 1 : -1;
  m_pendingMonthDelta = delta;

  PanelManager::instance().requestFrameTick();
  m_monthSlideAnimId = animations->animate(
      0.0F, 1.0F, static_cast<float>(Style::animFast), Easing::EaseOutCubic,
      [this](float progress) {
        applyMonthSlide(progress, false);
        PanelManager::instance().requestRedraw();
      },
      [this]() {
        m_monthSlideAnimId = 0;
        m_monthOffset += m_pendingMonthDelta;
        m_pendingMonthDelta = 0;
        m_lastDisplayYear = std::numeric_limits<int>::min();
        m_lastDisplayMonth = -1;
        m_startMonthSlideIn = true;
        PanelManager::instance().refresh();
      },
      m_gridViewport
  );
}

void CalendarTab::beginSlideIn() {
  AnimationManager* animations = m_gridViewport != nullptr ? m_gridViewport->animationManager() : nullptr;
  if (animations == nullptr || m_grid == nullptr) {
    return;
  }

  applyMonthSlide(0.0F, true);
  PanelManager::instance().requestFrameTick();
  m_monthSlideAnimId = animations->animate(
      0.0F, 1.0F, static_cast<float>(Style::animFast), Easing::EaseOutCubic,
      [this](float progress) {
        applyMonthSlide(progress, true);
        PanelManager::instance().requestRedraw();
      },
      [this]() {
        m_monthSlideAnimId = 0;
        if (m_grid != nullptr) {
          m_grid->setPosition(0.0F, 0.0F);
          m_grid->setOpacity(1.0F);
        }
      },
      m_gridViewport
  );
}

void CalendarTab::rebuild() {
  uiAssertNotRendering("CalendarTab::rebuild");
  if (m_grid == nullptr || m_monthLabel == nullptr || m_card == nullptr) {
    return;
  }

  const float scale = contentScale();
  const float innerWidth = std::max(0.0F, m_card->width() - (m_card->paddingLeft() + m_card->paddingRight()));
  const float innerHeight = std::max(0.0F, m_card->height() - (m_card->paddingTop() + m_card->paddingBottom()));
  const float navWidth = kCalendarNavButtonSize * scale * 2.0F + Style::spaceSm * scale * 2.0F;
  const float monthWidth = std::max(0.0F, innerWidth - navWidth);
  const float gridHeightAvailable =
      std::max(0.0F, innerHeight - kCalendarHeaderHeight * scale - kCalendarGridGap * scale);
  const float weekdayHeight = kCalendarWeekdayRowHeight * scale;
  const float dayCellHeight = std::clamp(
      (gridHeightAvailable - weekdayHeight - kCalendarGridGap * scale * 6.0F) / 6.0F, kCalendarCellSizeMin * scale,
      kCalendarCellSizeMax * scale
  );

  // The week-number column takes its share of the row before the day columns split the rest. It is
  // inset from the divider by one grid gap, which reads as balanced against the card padding on its
  // other side without spending a full card padding's worth of width on a two-digit number.
  const float weekLaneInset = m_showWeekNumbers ? kCalendarGridGap * scale : 0.0F;
  const float weekDividerWidth = m_showWeekNumbers ? std::round(1.0F * scale) : 0.0F;
  const float weekLaneOverhead = m_showWeekNumbers ? weekLaneInset + weekDividerWidth + kCalendarGridGap * scale : 0.0F;
  // Solve for a day column that leaves the week column its fraction, then floor the week column at the
  // width of its text and give the day grid whatever is actually left.
  const float provisionalDayColumn = std::max(
      0.0F,
      (innerWidth - kCalendarGridGap * scale * 6.0F - weekLaneOverhead)
          / (m_showWeekNumbers ? 7.0F + kCalendarWeekColumnRatio : 7.0F)
  );
  const float weekColumnWidth = m_showWeekNumbers
      ? std::max(
            std::round(provisionalDayColumn * kCalendarWeekColumnRatio),
            std::round(Style::fontSizeCaption * scale * kCalendarWeekColumnMinFontScale)
        )
      : 0.0F;
  const float dayGridWidth = std::max(0.0F, innerWidth - weekColumnWidth - weekLaneOverhead);
  const float dayColumnWidth = std::max(0.0F, (dayGridWidth - kCalendarGridGap * scale * 6.0F) / 7.0F);
  // Reserve a fixed strip under each day number for event indicator dots so all cells stay aligned.
  const float dotDiameter = std::round(5.0F * scale);
  const float dotGap = std::round(2.0F * scale);
  const float dotStripHeight = dotDiameter;
  const float buttonBudget = std::max(0.0F, dayCellHeight - dotStripHeight - dotGap);
  const float dayButtonSize = std::floor(std::min({buttonBudget, dayColumnWidth, kCalendarDayButtonSizeMax * scale}));

  if (m_header != nullptr) {
    m_header->setSize(innerWidth, kCalendarHeaderHeight * scale);
  }
  if (m_previousSlot != nullptr) {
    m_previousSlot->setSize(kCalendarNavButtonSize * scale, kCalendarHeaderHeight * scale);
  }
  if (m_nextSlot != nullptr) {
    m_nextSlot->setSize(kCalendarNavButtonSize * scale, kCalendarHeaderHeight * scale);
  }
  if (m_previousButton != nullptr) {
    m_previousButton->setSize(kCalendarNavButtonSize * scale, kCalendarNavButtonSize * scale);
  }
  if (m_nextButton != nullptr) {
    m_nextButton->setSize(kCalendarNavButtonSize * scale, kCalendarNavButtonSize * scale);
  }
  if (m_monthWrap != nullptr) {
    m_monthWrap->setSize(monthWidth, kCalendarHeaderHeight * scale);
  }
  m_monthLabel->setMaxWidth(monthWidth);
  if (m_todayLabel != nullptr) {
    m_todayLabel->setText(formatShellDate(m_config));
  }

  calendar_view::rebuildMonth({
      .grid = *m_grid,
      .monthLabel = *m_monthLabel,
      .snapshot = m_calendar != nullptr ? &m_calendar->snapshot() : nullptr,
      .selected = {.year = m_selectedYear, .month = m_selectedMonth, .day = m_selectedDay},
      .monthOffset = m_monthOffset,
      .showWeekNumbers = m_showWeekNumbers,
      .scale = scale,
      .layout =
          {
              .width = innerWidth,
              .weekdayHeight = weekdayHeight,
              .dayCellHeight = dayCellHeight,
              .dayButtonSize = dayButtonSize,
              .gap = kCalendarGridGap * scale,
              .dotDiameter = dotDiameter,
              .dotGap = dotGap,
              .weekColumnWidth = weekColumnWidth,
              .weekLaneInset = weekLaneInset,
              .weekDividerWidth = weekDividerWidth,
              .weekDaysGap = m_showWeekNumbers ? kCalendarGridGap * scale : 0.0F,
          },
      .onDateSelected =
          [this](calendar_view::Date date, int monthShift) {
            m_selectedYear = date.year;
            m_selectedMonth = date.month;
            m_selectedDay = date.day;
            m_eventsDirty = true;
            if (monthShift != 0) {
              changeMonthBy(monthShift);
            } else {
              applyCatalog(indexFromCatalog(Catalog::Events), true);
              PanelManager::instance().refresh();
            }
          },
      .onDateRightClicked =
          [](calendar_view::Date) {
            if (desktop_entry_launch::launchDefaultForMimeType("text/calendar")) {
              PanelManager::instance().closePanel();
            }
          },
  });

  if (m_gridViewport != nullptr) {
    m_gridViewport->setSize(innerWidth, m_grid->height());
  }
}

void CalendarTab::rebuildEventList(float scale) {
  if (m_eventsScroll == nullptr || m_config == nullptr) {
    return;
  }
  const auto& calendarConfig = m_config->config().calendar;
  const bool hasAccount = std::ranges::any_of(calendarConfig.accounts, isManagedCalendarAccount);
  std::string emptyText;
  if (!calendarConfig.enabled) {
    emptyText = i18n::tr("control-center.calendar.accounts.disabled-events");
  } else if (!hasAccount) {
    emptyText = i18n::tr("control-center.calendar.accounts.empty");
  }
  calendar_view::rebuildEventList({
      .scroll = *m_eventsScroll,
      .title = nullptr,
      .snapshot = m_calendar != nullptr ? &m_calendar->snapshot() : nullptr,
      .selected = {.year = m_selectedYear, .month = m_selectedMonth, .day = m_selectedDay},
      .scale = scale,
      .dateFormat = calendarConfig.eventDateFormat,
      .timeFormat = calendarConfig.eventTimeFormat,
      .emptyText = emptyText,
      .state = &m_eventListState,
      .requestRedraw = []() { PanelManager::instance().requestRedraw(); },
  });
}
