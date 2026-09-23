#include "shell/control_center/tabs/clock_tab.h"

#include "config/config_service.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/search_picker.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace control_center;

namespace {

  [[nodiscard]] bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return changed;
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

  // "Tomorrow · +16:00": the day relation and offset from local time, the way world clocks read
  // at a glance. Empty when the zone cannot be resolved.
  [[nodiscard]] std::string timezoneRelativeText(std::string_view tzName) {
    try {
      using namespace std::chrono;
      const auto now = system_clock::now();
      const time_zone* zone = locate_zone(tzName);
      const time_zone* local = current_zone();
      if (zone == nullptr || local == nullptr) {
        return {};
      }
      const auto zoneLocal = zone->to_local(now);
      const auto homeLocal = local->to_local(now);
      const int dayDelta = static_cast<int>((floor<days>(zoneLocal) - floor<days>(homeLocal)).count());
      const char* dayKey = dayDelta > 0   ? "control-center.clock.relative.tomorrow"
                           : dayDelta < 0 ? "control-center.clock.relative.yesterday"
                                          : "control-center.clock.relative.today";
      const seconds diff = zone->get_info(now).offset - local->get_info(now).offset;
      if (diff.count() == 0) {
        return i18n::tr(dayKey) + " · " + i18n::tr("control-center.clock.relative.same-time");
      }
      const auto absDiff = diff.count() < 0 ? -diff : diff;
      const auto h = duration_cast<hours>(absDiff);
      const auto m = duration_cast<minutes>(absDiff - h);
      return std::format(
          "{} · {}{}:{:02}", i18n::tr(dayKey), diff.count() < 0 ? "−" : "+", h.count(), m.count()
      );
    } catch (...) {
      return {};
    }
  }

  // Leading marker keeps an empty list distinct from "never built", so the empty state shows.
  [[nodiscard]] std::string timezoneListKey(const std::vector<std::string>& timezones) {
    std::string key = "#";
    key.reserve(timezones.size() * 24 + 1);
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

  [[nodiscard]] std::string localZoneName() {
    try {
      if (const auto* zone = std::chrono::current_zone(); zone != nullptr) {
        return std::string(zone->name());
      }
    } catch (...) {
    }
    return {};
  }

  [[nodiscard]] std::string localDateText(const ConfigService* config) {
    const char* format = config != nullptr ? config->config().shell.dateFormat.c_str() : "%A, %x";
    return formatLocalTime(format);
  }

  [[nodiscard]] std::string localTimeText(const ConfigService* config) {
    const char* format = config != nullptr ? config->config().shell.timeFormat.c_str() : "{:%H:%M}";
    return formatLocalTime(format);
  }

} // namespace

ClockTab::ClockTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "clock", .includedGroups = {"clock"}}
      )) {}

std::unique_ptr<Flex> ClockTab::create() {
  const float scale = contentScale();

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
  compactCard->addChild(
      ui::label({
          .out = &m_localTime,
          .text = localTimeText(m_config),
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
      })
  );
  compactCard->addChild(
      ui::label({
          .out = &m_localDate,
          .text = localDateText(m_config),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      })
  );
  compactCard->addChild(
      ui::label({
          .out = &m_localZone,
          .text = timezoneDisplayName(localZoneName()),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      })
  );
  tab->addChild(std::move(compactCard));

  // Leftover: the timezone search sits on top so typing reveals matches right under the field;
  // the comparison list (or its empty state) fills the rest of the well.
  auto leftover = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
      .configure = [scale](Flex& host) { applyContentCardStyle(host, scale); },
  });
  leftover->addChild(
      ui::searchPicker({
          .out = &m_timezonePicker,
          .placeholder = i18n::tr("control-center.clock.placeholder"),
          .emptyText = i18n::tr("control-center.clock.no-match"),
          .scale = scale,
          .height = Style::controlHeight * 4.5F * scale,
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
                picker.setClipChildren(true);
              },
      })
  );
  leftover->addChild(
      ui::label({
          .out = &m_timezoneError,
          .text = "",
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Error),
          .visible = false,
          .participatesInLayout = false,
      })
  );
  {
    auto empty = control_center::makeEmptyState(
        scale,
        {
            .glyph = "world-search",
            .title = i18n::tr("control-center.clock.empty-title"),
            .body = i18n::tr("control-center.clock.empty"),
        }
    );
    m_emptyState = empty.get();
    empty->setJustify(FlexJustify::Center);
    empty->setFlexGrow(1.0F);
    empty->setVisible(false);
    empty->setParticipatesInLayout(false);
    leftover->addChild(std::move(empty));
  }
  auto listScroll = ui::scrollView({
      .out = &m_zoneScroll,
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
  m_zoneList = listScroll->content();
  if (m_zoneList != nullptr) {
    m_zoneList->setDirection(FlexDirection::Vertical);
    m_zoneList->setAlign(FlexAlign::Stretch);
    applyGroupedListStyle(*m_zoneList, scale);
    m_zoneList->setPadding(Style::spaceXs * scale);
  }
  leftover->addChild(std::move(listScroll));
  tab->addChild(std::move(leftover));

  rebuildZones();
  return m_settings->wrapLive(std::move(tab), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> ClockTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void ClockTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool ClockTab::configureMode() const { return m_settings->configureMode(); }

void ClockTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

SettingsPane* ClockTab::settingsPane() { return m_settings.get(); }

void ClockTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  rebuildZones();
  if (m_timezonePicker != nullptr) {
    // Matches drop down only while there is a query; an idle field stays a single row so the
    // world-clock list (or its empty state) owns the well.
    const float scale = contentScale();
    const float target = m_timezonePicker->filter().empty() ? Style::controlHeight * scale
                                                            : Style::controlHeight * 4.5F * scale;
    if (std::abs(m_timezonePicker->height() - target) > 0.5F) {
      m_timezonePicker->setSize(m_timezonePicker->width(), target);
    }
  }
  m_rootLayout->layout(renderer);
}

void ClockTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  syncLocalClock();
  rebuildZones();
  syncZoneTimes();
}

void ClockTab::setActive(bool active) {
  if (!active) {
    m_clockTimer.stop();
    return;
  }
  if (!m_clockTimer.active()) {
    m_clockTimer.startRepeating(std::chrono::milliseconds(1000), [this]() { PanelManager::instance().refresh(); });
  }
}

void ClockTab::onClose() {
  m_settings->onClose();
  m_clockTimer.stop();
  m_rootLayout = nullptr;
  m_compactCard = nullptr;
  m_localTime = nullptr;
  m_localDate = nullptr;
  m_localZone = nullptr;
  m_zoneList = nullptr;
  m_emptyState = nullptr;
  m_zoneScroll = nullptr;
  m_timezoneError = nullptr;
  m_timezonePicker = nullptr;
  m_zoneRows.clear();
  m_lastTimezoneKey.clear();
}

void ClockTab::syncLocalClock() {
  if (m_localTime != nullptr) {
    m_localTime->setText(localTimeText(m_config));
  }
  if (m_localDate != nullptr) {
    m_localDate->setText(localDateText(m_config));
  }
  if (m_localZone != nullptr) {
    m_localZone->setText(timezoneDisplayName(localZoneName()));
  }
}

void ClockTab::rebuildZones() {
  if (m_zoneList == nullptr) {
    return;
  }
  const std::vector<std::string> timezones =
      m_config != nullptr ? m_config->config().controlCenter.clockTab.timezones : std::vector<std::string>{};
  const std::string key = timezoneListKey(timezones);
  if (key == m_lastTimezoneKey) {
    return;
  }

  uiAssertNotRendering("ClockTab::rebuildZones");
  while (!m_zoneList->children().empty()) {
    m_zoneList->removeChild(m_zoneList->children().front().get());
  }
  m_zoneRows.clear();

  const float scale = contentScale();
  for (const auto& zone : timezones) {
    ZoneRow row;
    row.zone = zone;
    auto item = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .configure = [scale](Flex& host) { applyGroupedListRowStyle(host, scale, GroupedListRowEmphasis::None); },
    });
    item->addChild(ui::column(
        {.align = FlexAlign::Start, .flexGrow = 1.0F},
        ui::label({
            .text = timezoneDisplayName(zone),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
        }),
        ui::label({
            .out = &row.relativeLabel,
            .text = timezoneRelativeText(zone),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
        })
    ));
    item->addChild(
        ui::label({
            .out = &row.timeLabel,
            .text = timezoneTimeText(m_config, zone),
            .fontSize = Style::fontSizeTitle * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    const std::size_t index = m_zoneRows.size();
    item->addChild(
        ui::button({
            .glyph = "trash",
            .glyphSize = Style::fontSizeBody * scale,
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr("control-center.clock.remove"),
            .minWidth = Style::controlHeightSm * scale,
            .minHeight = Style::controlHeightSm * scale,
            .padding = Style::spaceXs * scale,
            .onClick = [this, index]() { removeTimezone(index); },
        })
    );
    m_zoneRows.push_back(row);
    m_zoneList->addChild(std::move(item));
  }

  m_lastTimezoneKey = key;
  (void)setShown(m_emptyState, timezones.empty());
  (void)setShown(m_zoneScroll, !timezones.empty());
  syncPicker();
}

void ClockTab::syncPicker() {
  if (m_timezonePicker == nullptr) {
    return;
  }
  const std::vector<std::string> timezones =
      m_config != nullptr ? m_config->config().controlCenter.clockTab.timezones : std::vector<std::string>{};
  m_timezonePicker->setOptions(availableTimezoneOptions(timezones));
}

void ClockTab::syncZoneTimes() {
  for (auto& row : m_zoneRows) {
    if (row.timeLabel != nullptr) {
      row.timeLabel->setText(timezoneTimeText(m_config, row.zone));
    }
    if (row.relativeLabel != nullptr) {
      row.relativeLabel->setText(timezoneRelativeText(row.zone));
    }
  }
}

void ClockTab::addTimezone(std::string_view raw) {
  const std::string zone = StringUtils::trim(raw);
  if (zone.empty()) {
    return;
  }
  if (!isValidTimezone(zone)) {
    setTimezoneError(i18n::tr("control-center.clock.invalid"));
    return;
  }

  std::vector<std::string> timezones =
      m_config != nullptr ? m_config->config().controlCenter.clockTab.timezones : std::vector<std::string>{};
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
  rebuildZones();
  if (m_timezonePicker != nullptr) {
    m_timezonePicker->setFilter("");
  }
  PanelManager::instance().refresh();
}

void ClockTab::removeTimezone(std::size_t index) {
  std::vector<std::string> timezones =
      m_config != nullptr ? m_config->config().controlCenter.clockTab.timezones : std::vector<std::string>{};
  if (index >= timezones.size()) {
    return;
  }
  timezones.erase(timezones.begin() + static_cast<std::ptrdiff_t>(index));
  persistTimezones(timezones);
  rebuildZones();
  PanelManager::instance().refresh();
}

void ClockTab::persistTimezones(const std::vector<std::string>& timezones) {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"control_center", "clock", "timezones"}, timezones);
}

void ClockTab::setTimezoneError(std::string_view message) {
  if (m_timezoneError == nullptr) {
    return;
  }
  m_timezoneError->setText(std::string(message));
  (void)setShown(m_timezoneError, !message.empty());
}
