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
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "system/desktop_entry_launch.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/calendar_view.h"
#include "ui/controls/flex.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/toggle.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <wayland-client-protocol.h>

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

  [[nodiscard]] std::string timezoneDisplayName(std::string_view tzName) {
    std::string name(tzName);
    if (const auto slash = name.rfind('/'); slash != std::string::npos) {
      name = name.substr(slash + 1);
    }
    std::replace(name.begin(), name.end(), '_', ' ');
    return name;
  }

  [[nodiscard]] std::string timezoneTimeText(const ConfigService* config, std::string_view tzName) {
    const char* format = config != nullptr ? config->config().shell.timeFormat.c_str() : "{:%H:%M}";
    return formatTimezoneTime(format, tzName);
  }

  [[nodiscard]] std::string timezoneListKey(const std::vector<std::string>& timezones) {
    std::string key;
    key.reserve(timezones.size() * 24);
    for (const auto& zone : timezones) {
      key += zone;
      key.push_back('\n');
    }
    return key;
  }

  [[nodiscard]] SearchPickerOption makeTimezoneOption(std::string name) {
    SearchPickerOption option;
    option.value = std::move(name);
    option.label = timezoneDisplayName(option.value);
    if (option.label != option.value) {
      option.description = option.value;
    }
    return option;
  }

  [[nodiscard]] const std::vector<SearchPickerOption>& timezoneCatalog() {
    static const std::vector<SearchPickerOption> catalog = [] {
      std::vector<SearchPickerOption> options;
      std::unordered_set<std::string> seen;
      try {
        const auto& db = std::chrono::get_tzdb();
        options.reserve(db.zones.size() + db.links.size());
        for (const auto& zone : db.zones) {
          std::string name(zone.name());
          if (!seen.insert(name).second) {
            continue;
          }
          options.push_back(makeTimezoneOption(std::move(name)));
        }
        for (const auto& link : db.links) {
          std::string name(link.name());
          if (!seen.insert(name).second) {
            continue;
          }
          options.push_back(makeTimezoneOption(std::move(name)));
        }
      } catch (...) {
      }
      return options;
    }();
    return catalog;
  }

  [[nodiscard]] std::vector<SearchPickerOption> availableTimezoneOptions(const std::vector<std::string>& selected) {
    std::vector<SearchPickerOption> options;
    const auto& catalog = timezoneCatalog();
    options.reserve(catalog.size());
    for (const auto& option : catalog) {
      if (!std::ranges::contains(selected, option.value)) {
        options.push_back(option);
      }
    }
    return options;
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
} // namespace

CalendarTab::CalendarTab(ConfigService* config, CalendarService* calendar, const ControlCenterServices& services)
    : m_config(config), m_calendar(calendar),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "services", .includedGroups = {"calendar"}}
      )) {}

std::unique_ptr<Flex> CalendarTab::create() {
  const float scale = contentScale();

  if (m_calendar != nullptr && !m_changeCallbackRegistered) {
    m_changeCallbackRegistered = true;
    (void)m_calendar->addChangeCallback([this]() {
      m_eventsDirty = true;
      PanelManager::instance().refresh();
    });
  }

  if (m_config != nullptr) {
    m_showEventsCard = m_config->config().controlCenter.calendarTab.showEventsCard;
    m_showWeekNumbers = m_config->config().controlCenter.calendarTab.showWeekNumbers;
  }

  auto tab = ui::row({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto calendarArea = ui::inputArea({});
  calendarArea->setFlexGrow(3.0F);
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
      .gap = Style::spaceMd * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) {
        control_center::applySectionCardStyle(card, scale, opacity);
      },
  });

  calendarCard->addChild(
      ui::label({
          .out = &m_todayLabel,
          .text = formatShellDate(m_config),
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Medium,
          .color = colorSpecFromRole(ColorRole::Secondary),
          .maxLines = 1,
          .configure = [this](Label& label) {
            label.setHitTestVisible(true);
            label.setOnClick([this](const InputArea::PointerData&) {
              const calendar_view::State state = calendar_view::stateForOffset(m_monthOffset);
              const bool focusedOnToday = m_monthOffset == 0
                  && m_selectedYear == state.current.year
                  && m_selectedMonth == state.current.month
                  && m_selectedDay == state.current.day;
              if (focusedOnToday) {
                return;
              }
              focusToday();
              PanelManager::instance().refresh();
            });
          },
      })
  );

  auto header = ui::row({
      .out = &m_header,
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceSm * scale,
      .minHeight = kCalendarHeaderHeight * scale,
  });

  auto previousSlot = ui::row(
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
  );
  header->addChild(std::move(previousSlot));

  auto monthWrap = ui::column(
      {.out = &m_monthWrap, .align = FlexAlign::Center, .justify = FlexJustify::Center, .flexGrow = 1.0F},
      ui::label({
          .out = &m_monthLabel,
          .fontSize = (Style::fontSizeTitle + Style::spaceXs) * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
      })
  );
  header->addChild(std::move(monthWrap));

  auto nextSlot = ui::row(
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
  );
  header->addChild(std::move(nextSlot));

  calendarCard->addChild(std::move(header));

  auto gridViewport = ui::column({
      .out = &m_gridViewport,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
      .configure = [](Flex& viewport) { viewport.setClipChildren(true); },
  });

  auto grid = ui::column({
      .out = &m_grid,
      .align = FlexAlign::Stretch,
      .gap = kCalendarGridGap * scale,
      .configure = [](Flex& layer) { layer.setParticipatesInLayout(false); },
  });
  gridViewport->addChild(std::move(grid));
  calendarCard->addChild(std::move(gridViewport));
  calendarArea->addChild(std::move(calendarCard));
  tab->addChild(std::move(calendarArea));

  auto eventsCard = ui::column(
      {.out = &m_eventsCard,
       .gap = Style::spaceSm * scale,
       .flexGrow = 1.0F,
       .configure = [scale, opacity = panelCardOpacity()](
                        Flex& card
                    ) { control_center::applySectionCardStyle(card, scale, opacity); }},
      ui::label({
          .out = &m_eventsTitle,
          .text = i18n::tr("control-center.calendar.events"),
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
      }),
      ui::scrollView({
          .out = &m_eventsScroll,
          .contentScale = scale,
          .fillWidth = true,
          .fillHeight = true,
          .flexGrow = 1.0F,
      })
  );
  eventsCard->setVisible(m_showEventsCard);

  auto side = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 2.0F,
  });
  buildAccountsCard(*side, scale);
  buildWorldClockCard(*side, scale);
  side->addChild(std::move(eventsCard));
  tab->addChild(std::move(side));

  return m_settings->wrapLive(std::move(tab), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> CalendarTab::createHeaderActions() {
  const float scale = contentScale();
  if (m_config != nullptr) {
    m_showEventsCard = m_config->config().controlCenter.calendarTab.showEventsCard;
  }
  auto live = ui::row(
      {
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
      },
      ui::button({
          .out = &m_toggleEventsCardButton,
          .glyph = m_showEventsCard ? "calendar-event" : "calendar-off",
          .selected = m_showEventsCard,
          .tooltip = i18n::tr("control-center.calendar.toggle-events-card"),
          .onClick = [this]() { toggleEventsCard(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  return m_settings->embedHeader(std::move(live), scale);
}

void CalendarTab::buildAccountsCard(Flex& parent, float scale) {
  auto card = ui::column({
      .out = &m_accountsCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { control_center::applySectionCardStyle(section, scale, opacity); },
  });

  auto header = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .minHeight = Style::controlHeightSm * scale,
  });
  header->addChild(
      ui::label({
          .text = i18n::tr("control-center.calendar.accounts.title"),
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0F,
      })
  );
  header->addChild(
      ui::button({
          .out = &m_accountsAddButton,
          .glyph = "plus",
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.calendar.accounts.add"),
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .onClick = [this]() { openAccount(std::nullopt); },
      })
  );
  card->addChild(std::move(header));

  auto disabledRow = ui::row({
      .out = &m_accountsDisabledRow,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .visible = false,
  });
  disabledRow->addChild(
      ui::label({
          .text = i18n::tr("control-center.calendar.accounts.disabled"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .flexGrow = 1.0F,
      })
  );
  disabledRow->addChild(
      ui::toggle({
          .out = &m_accountsEnableToggle,
          .checked = m_calendar != nullptr && m_calendar->enabled(),
          .scale = scale,
          .onChange = [this](bool value) { setCalendarEnabled(value); },
      })
  );
  card->addChild(std::move(disabledRow));

  card->addChild(
      ui::label({
          .out = &m_accountsEmpty,
          .text = i18n::tr("control-center.calendar.accounts.empty"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
      })
  );
  card->addChild(
      ui::column({
          .out = &m_accountsList,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
      })
  );
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
            .text = i18n::tr(buttonKey),
            .glyph = glyph,
            .fontSize = Style::fontSizeCaption * scale,
            .variant = credentialLocked ? ButtonVariant::Secondary : ButtonVariant::Outline,
            .minHeight = Style::controlHeightSm * scale,
            .paddingV = Style::spaceXs * scale,
            .paddingH = Style::spaceSm * scale,
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
  const bool layoutDirty = setShown(m_accountsDisabledRow, !enabled) || setShown(m_accountsAddButton, enabled)
      || setShown(m_accountsList, enabled) || setShown(m_accountsEmpty, enabled && !hasAccounts);
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
  if (m_accountsEnableToggle != nullptr) {
    m_accountsEnableToggle->setChecked(enabled);
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
  PanelManager::instance().refresh();
}

void CalendarTab::buildWorldClockCard(Flex& parent, float scale) {
  auto card = ui::column({
      .out = &m_worldClockCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { control_center::applySectionCardStyle(section, scale, opacity); },
  });
  card->addChild(control_center::makeCardHeaderRow(i18n::tr("control-center.calendar.world-clock.title"), scale));
  card->addChild(
      ui::column({
          .out = &m_timezoneList,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
      })
  );
  card->addChild(
      ui::label({
          .out = &m_timezoneError,
          .text = "",
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Error),
          .visible = false,
      })
  );
  card->addChild(
      ui::searchPicker({
          .out = &m_timezonePicker,
          .placeholder = i18n::tr("control-center.calendar.world-clock.placeholder"),
          .emptyText = i18n::tr("control-center.calendar.world-clock.empty"),
          .height = Style::controlHeight * 6.5F * scale,
          .onActivated =
              [this](const SearchPickerOption& option) {
                if (!option.value.empty()) {
                  addTimezone(option.value);
                }
              },
          .configure =
              [](SearchPicker& picker) {
                picker.clearFill();
                picker.clearBorder();
                picker.setRadius(0.0F);
                picker.setPadding(0.0F);
              },
      })
  );
  parent.addChild(std::move(card));
  rebuildWorldClock();
  syncTimezonePicker();
}

void CalendarTab::rebuildWorldClock() {
  if (m_timezoneList == nullptr) {
    return;
  }
  const std::vector<std::string> timezones =
      m_config != nullptr ? m_config->config().controlCenter.calendarTab.timezones : std::vector<std::string>{};
  const std::string key = timezoneListKey(timezones);
  if (key == m_lastTimezoneKey) {
    return;
  }

  uiAssertNotRendering("CalendarTab::rebuildWorldClock");
  while (!m_timezoneList->children().empty()) {
    m_timezoneList->removeChild(m_timezoneList->children().front().get());
  }
  m_timezoneRows.clear();

  const float scale = contentScale();
  for (const auto& zone : timezones) {
    TimezoneRow row;
    row.zone = zone;
    auto item = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .minHeight = Style::controlHeightSm * scale,
    });
    item->addChild(
        ui::label({
            .text = timezoneDisplayName(zone),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
    item->addChild(
        ui::label({
            .out = &row.timeLabel,
            .text = timezoneTimeText(m_config, zone),
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    const std::size_t index = m_timezoneRows.size();
    item->addChild(
        ui::button({
            .glyph = "trash",
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr("control-center.calendar.world-clock.remove"),
            .minWidth = Style::controlHeightSm * scale,
            .minHeight = Style::controlHeightSm * scale,
            .padding = Style::spaceXs * scale,
            .onClick = [this, index]() { removeTimezone(index); },
        })
    );
    m_timezoneRows.push_back(row);
    m_timezoneList->addChild(std::move(item));
  }

  m_lastTimezoneKey = key;
  syncTimezonePicker();
}

void CalendarTab::syncTimezonePicker() {
  if (m_timezonePicker == nullptr) {
    return;
  }
  const std::vector<std::string> timezones =
      m_config != nullptr ? m_config->config().controlCenter.calendarTab.timezones : std::vector<std::string>{};
  m_timezonePicker->setOptions(availableTimezoneOptions(timezones));
}

void CalendarTab::syncWorldClockTimes() {
  for (auto& row : m_timezoneRows) {
    if (row.timeLabel != nullptr) {
      row.timeLabel->setText(timezoneTimeText(m_config, row.zone));
    }
  }
}

void CalendarTab::addTimezone(std::string_view raw) {
  const std::string zone = StringUtils::trim(raw);
  if (zone.empty()) {
    return;
  }
  if (!isValidTimezone(zone)) {
    setTimezoneError(i18n::tr("control-center.calendar.world-clock.invalid"));
    return;
  }

  std::vector<std::string> timezones =
      m_config != nullptr ? m_config->config().controlCenter.calendarTab.timezones : std::vector<std::string>{};
  if (std::ranges::contains(timezones, zone)) {
    setTimezoneError({});
    if (m_timezonePicker != nullptr) {
      m_timezonePicker->setFilter("");
    }
    return;
  }

  timezones.push_back(zone);
  persistTimezones(timezones);
  setTimezoneError({});
  rebuildWorldClock();
  if (m_timezonePicker != nullptr) {
    m_timezonePicker->setFilter("");
  }
  PanelManager::instance().refresh();
}

void CalendarTab::removeTimezone(std::size_t index) {
  std::vector<std::string> timezones =
      m_config != nullptr ? m_config->config().controlCenter.calendarTab.timezones : std::vector<std::string>{};
  if (index >= timezones.size()) {
    return;
  }
  timezones.erase(timezones.begin() + static_cast<std::ptrdiff_t>(index));
  persistTimezones(timezones);
  rebuildWorldClock();
  PanelManager::instance().refresh();
}

void CalendarTab::persistTimezones(const std::vector<std::string>& timezones) {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"control_center", "calendar", "timezones"}, timezones);
}

void CalendarTab::setTimezoneError(std::string_view message) {
  if (m_timezoneError == nullptr) {
    return;
  }
  m_timezoneError->setText(std::string(message));
  m_timezoneError->setVisible(!message.empty());
}

void CalendarTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool CalendarTab::configureMode() const { return m_settings->configureMode(); }

void CalendarTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

void CalendarTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr || m_card == nullptr || m_calendarArea == nullptr) {
    return;
  }

  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  m_card->setSize(m_calendarArea->width(), m_calendarArea->height());
  m_card->layout(renderer);

  const float innerWidth = std::max(0.0F, m_card->width() - (m_card->paddingLeft() + m_card->paddingRight()));
  const float innerHeight = std::max(0.0F, m_card->height() - (m_card->paddingTop() + m_card->paddingBottom()));
  const calendar_view::State state = calendar_view::stateForOffset(m_monthOffset);

  // Default the selection to today until the user picks a day.
  if (m_selectedDay < 0) {
    m_selectedYear = state.current.year;
    m_selectedMonth = state.current.month;
    m_selectedDay = state.current.day;
  }

  const bool sizeChanged = std::abs(innerWidth - m_lastInnerWidth) >= kCalendarLayoutEpsilon
      || std::abs(innerHeight - m_lastInnerHeight) >= kCalendarLayoutEpsilon;
  const bool displayChanged = state.displayYear != m_lastDisplayYear || state.displayMonth != m_lastDisplayMonth;
  const bool todayChanged = state.current.year != m_lastCurrentYear
      || state.current.month != m_lastCurrentMonth
      || state.current.day != m_lastToday;

  if (m_monthSlideAnimId != 0 && !m_startMonthSlideIn) {
    m_rootLayout->layout(renderer);
    calendar_view::layoutEventLinkOverlays(m_eventListState);
    return;
  }

  if (!sizeChanged && !displayChanged && !todayChanged && !m_eventsDirty && !m_startMonthSlideIn) {
    return;
  }
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
  m_rootLayout->layout(renderer);
  calendar_view::layoutEventLinkOverlays(m_eventListState);
}

void CalendarTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_todayLabel != nullptr) {
    m_todayLabel->setText(formatShellDate(m_config));
  }
  rebuildWorldClock();
  syncWorldClockTimes();
  rebuildAccounts();
}

void CalendarTab::setActive(bool active) {
  if (!active) {
    m_eventFadeTimer.stop();
    m_clockTimer.stop();
    return;
  }
  focusToday();
  if (!m_eventFadeTimer.active()) {
    m_eventFadeTimer.startRepeating(std::chrono::minutes{1}, [this]() {
      m_eventsDirty = true;
      PanelManager::instance().refresh();
    });
  }
  if (!m_clockTimer.active()) {
    m_clockTimer.startRepeating(std::chrono::milliseconds(1000), [this]() { PanelManager::instance().refresh(); });
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
  m_clockTimer.stop();
  cancelMonthSlide();
  m_rootLayout = nullptr;
  m_calendarArea = nullptr;
  m_card = nullptr;
  m_header = nullptr;
  m_previousSlot = nullptr;
  m_nextSlot = nullptr;
  m_monthWrap = nullptr;
  m_todayLabel = nullptr;
  m_monthLabel = nullptr;
  m_previousButton = nullptr;
  m_nextButton = nullptr;
  m_toggleEventsCardButton = nullptr;
  m_gridViewport = nullptr;
  m_grid = nullptr;
  m_eventsCard = nullptr;
  m_eventsTitle = nullptr;
  m_eventsScroll = nullptr;
  m_accountsCard = nullptr;
  m_accountsAddButton = nullptr;
  m_accountsDisabledRow = nullptr;
  m_accountsList = nullptr;
  m_accountsEmpty = nullptr;
  m_accountsEnableToggle = nullptr;
  m_worldClockCard = nullptr;
  m_timezoneList = nullptr;
  m_timezoneError = nullptr;
  m_timezonePicker = nullptr;
  m_timezoneRows.clear();
  m_lastTimezoneKey.clear();
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
    m_todayLabel->setMaxWidth(innerWidth);
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
  rebuildEventList(scale);
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
      .title = m_eventsTitle,
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

void CalendarTab::toggleEventsCard() {
  m_showEventsCard = !m_showEventsCard;
  if (m_config != nullptr) {
    m_config->setOverride({"control_center", "calendar", "show_events_card"}, m_showEventsCard);
  }
  if (m_toggleEventsCardButton != nullptr) {
    m_toggleEventsCardButton->setGlyph(m_showEventsCard ? "calendar-event" : "calendar-off");
    m_toggleEventsCardButton->setSelected(m_showEventsCard);
  }
  if (m_eventsCard != nullptr) {
    m_eventsCard->setVisible(m_showEventsCard);
  }
}
