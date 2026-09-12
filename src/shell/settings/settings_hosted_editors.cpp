#include "shell/settings/settings_hosted_editors.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "net/url_open.h"
#include "notification/notification_filter.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "scripting/plugin_catalog.h"
#include "scripting/plugin_manager.h"
#include "scripting/plugin_registry.h"
#include "shell/settings/bar_widget_editor.h"
#include "shell/settings/plugin_store_content.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_control_factory.h"
#include "shell/settings/settings_modal_host.h"
#include "shell/settings/widget_settings_registry.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/search_picker.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace settings {
  namespace {

    std::string idleBehaviorTitle(const IdleBehaviorConfig& row) {
      IdleBehaviorConfig norm = row;
      normalizeIdleBehaviorAction(norm);
      if (norm.action == "lock") {
        return i18n::tr("settings.idle.behavior.kind.lock");
      }
      if (norm.action == "screen_off") {
        return i18n::tr("settings.idle.behavior.kind.screen-off");
      }
      if (norm.action == "suspend") {
        return i18n::tr("settings.idle.behavior.kind.suspend");
      }
      if (norm.action == "lock_and_suspend") {
        return i18n::tr("settings.idle.behavior.kind.lock-and-suspend");
      }
      if (!StringUtils::trim(row.name).empty()) {
        return row.name;
      }
      return i18n::tr("settings.idle.behavior.unnamed");
    }

    std::string notificationFilterTitle(const NotificationFilterConfig& row) {
      if (!row.match.empty()) {
        return row.match;
      }
      if (!StringUtils::trim(row.name).empty()) {
        return row.name;
      }
      return i18n::tr("settings.notifications.filter.unnamed");
    }

    bool isBarWidgetListPath(const std::vector<std::string>& path) {
      if (path.size() < 3 || path.front() != "bar") {
        return false;
      }
      const auto& key = path.back();
      return key == "start" || key == "center" || key == "end";
    }

    std::vector<std::string> barWidgetItemsForPath(const Config& cfg, const std::vector<std::string>& path) {
      if (!isBarWidgetListPath(path) || path.size() < 3) {
        return {};
      }
      const auto* bar = findBar(cfg, path[1]);
      if (bar == nullptr) {
        return {};
      }
      if (path.back() == "start") {
        return bar->startWidgets;
      }
      if (path.back() == "center") {
        return bar->centerWidgets;
      }
      return bar->endWidgets;
    }

    void collectWidgetReferenceNames(const std::vector<std::string>& widgets, std::unordered_set<std::string>& seen) {
      for (const auto& widget : widgets) {
        seen.insert(widget);
      }
    }

    bool widgetReferenceNameExists(const Config& cfg, std::string_view name) {
      const std::string key(name);
      if (isBuiltInWidgetType(name) || cfg.widgets.contains(key)) {
        return true;
      }
      std::unordered_set<std::string> seen;
      for (const auto& bar : cfg.bars) {
        collectWidgetReferenceNames(bar.startWidgets, seen);
        collectWidgetReferenceNames(bar.centerWidgets, seen);
        collectWidgetReferenceNames(bar.endWidgets, seen);
        for (const auto& ovr : bar.monitorOverrides) {
          if (ovr.startWidgets.has_value()) {
            collectWidgetReferenceNames(*ovr.startWidgets, seen);
          }
          if (ovr.centerWidgets.has_value()) {
            collectWidgetReferenceNames(*ovr.centerWidgets, seen);
          }
          if (ovr.endWidgets.has_value()) {
            collectWidgetReferenceNames(*ovr.endWidgets, seen);
          }
        }
      }
      return seen.contains(key);
    }

    bool isValidWidgetInstanceId(std::string_view id) {
      if (id.empty()) {
        return false;
      }
      for (char c : id) {
        const auto uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') {
          return false;
        }
      }
      return true;
    }

    std::string normalizedWidgetInstanceBase(std::string_view type) {
      std::string out;
      out.reserve(type.size());
      bool lastUnderscore = false;
      for (char c : type) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
          out.push_back(static_cast<char>(std::tolower(uc)));
          lastUnderscore = false;
        } else if (!lastUnderscore && !out.empty()) {
          out.push_back('_');
          lastUnderscore = true;
        }
      }
      while (!out.empty() && out.back() == '_') {
        out.pop_back();
      }
      return out.empty() ? std::string("widget") : out;
    }

    std::string nextWidgetInstanceId(const Config& cfg, std::string_view type) {
      const std::string base = normalizedWidgetInstanceBase(type);
      for (std::size_t index = 2; index < 10000; ++index) {
        const std::string candidate = base + "_" + std::to_string(index);
        if (!widgetReferenceNameExists(cfg, candidate)) {
          return candidate;
        }
      }
      return base + "_custom";
    }

    std::string laneLabel(std::string_view lane) {
      if (lane == "start") {
        return i18n::tr("settings.entities.widget.lanes.start");
      }
      if (lane == "center") {
        return i18n::tr("settings.entities.widget.lanes.center");
      }
      if (lane == "end") {
        return i18n::tr("settings.entities.widget.lanes.end");
      }
      return std::string(lane);
    }

  } // namespace

  HostedEditors::HostedEditors() = default;
  HostedEditors::~HostedEditors() = default;

  void HostedEditors::initialize(SettingsModalHost& host, std::function<void()> dismissSelect) {
    m_host = &host;
    m_dismissSelect = std::move(dismissSelect);
  }

  void HostedEditors::bind(HostedEditorBindings bindings) { m_b = std::move(bindings); }

  void HostedEditors::attachSheet(SettingsSheetModal& sheet) { m_sheet = &sheet; }

  bool HostedEditors::isOpen() const noexcept { return m_sheet != nullptr && m_sheet->isOpen(); }

  void HostedEditors::ensureSheet() {
    if (m_sheet != nullptr) {
      return;
    }
    if (m_host == nullptr) {
      return;
    }
    m_ownedSheet = std::make_unique<SettingsSheetModal>();
    m_ownedSheet->initialize(*m_host, m_dismissSelect);
    m_sheet = m_ownedSheet.get();
  }

  void HostedEditors::prepareOpen() {
    if (m_b.dismissPopups) {
      m_b.dismissPopups();
    }
    ensureSheet();
    if (m_sheet != nullptr && m_sheet->isOpen()) {
      m_sheet->close();
    }
  }

  float HostedEditors::uiScale() const { return m_b.scale ? m_b.scale() : 1.0F; }

  void HostedEditors::close() {
    if (m_sheet != nullptr) {
      m_sheet->close();
    }
  }

  void HostedEditors::openSessionActionEntry(std::size_t index) {
    if (m_b.config == nullptr) {
      return;
    }
    prepareOpen();
    if (m_sheet == nullptr) {
      return;
    }
    const Config& cfg = m_b.config->config();
    if (index >= cfg.shell.session.actions.size()) {
      return;
    }
    auto rowState = std::make_shared<SessionPanelActionConfig>(cfg.shell.session.actions[index]);
    const auto persist = [this, rowState, index]() {
      if (m_b.config == nullptr || !m_b.setOverride) {
        return;
      }
      auto next = m_b.config->config().shell.session.actions;
      if (index >= next.size()) {
        return;
      }
      next[index] = *rowState;
      if (m_b.sessionActionsEditState != nullptr
          && *m_b.sessionActionsEditState != nullptr
          && index < (*m_b.sessionActionsEditState)->size()) {
        (**m_b.sessionActionsEditState)[index] = *rowState;
      }
      if (m_b.syncSessionActionSummary) {
        m_b.syncSessionActionSummary(index, *rowState);
      }
      m_b.setOverride({"shell", "session", "actions"}, next);
      if (m_sheet != nullptr && m_sheet->isOpen()) {
        m_sheet->setSheetTitle(sessionActionDisplayTitle(*rowState));
        m_sheet->requestLayout();
      }
    };
    const auto removeRow = [this, index]() {
      if (m_b.config == nullptr || !m_b.setOverride) {
        return;
      }
      auto next = m_b.config->config().shell.session.actions;
      if (index >= next.size()) {
        return;
      }
      next.erase(next.begin() + static_cast<std::ptrdiff_t>(index));
      m_b.setOverride({"shell", "session", "actions"}, next);
      if (m_sheet != nullptr) {
        m_sheet->close();
      }
      if (m_b.requestRebuild) {
        m_b.requestRebuild();
      }
    };
    if (!m_b.makeContext) {
      return;
    }
    auto ctx = m_b.makeContext();
    ctx.openSessionActionEntryEditor = {};
    ctx.openIdleBehaviorEntryEditor = {};
    ctx.closeHostedEditor = [this]() {
      if (m_sheet != nullptr) {
        m_sheet->close();
      }
    };
    m_sheet->open(
        SettingsSheetRequest{
            .sheetTitle = sessionActionDisplayTitle(*rowState),
            .removeAction = removeRow,
            .populateSheetBody =
                [ctx, rowState, persist](Flex& body) mutable {
                  buildSessionActionEntryDetailContent(body, ctx, *rowState, persist);
                },
            .scale = uiScale(),
        }
    );
  }

  void HostedEditors::openIdleBehaviorEntry(std::size_t index) {
    if (m_b.config == nullptr) {
      return;
    }
    prepareOpen();
    if (m_sheet == nullptr) {
      return;
    }
    const Config& cfg = m_b.config->config();
    if (index >= cfg.idle.behaviors.size()) {
      return;
    }
    auto rowState = std::make_shared<IdleBehaviorConfig>(cfg.idle.behaviors[index]);
    auto rowKey = std::make_shared<std::string>(rowState->name);
    normalizeIdleBehaviorAction(*rowState);
    const auto persist = [this, rowState, rowKey, index]() {
      if (m_b.config == nullptr || !m_b.setOverride) {
        return;
      }
      normalizeIdleBehaviorAction(*rowState);
      auto next = m_b.config->config().idle.behaviors;
      auto target = std::ranges::find(next, *rowKey, &IdleBehaviorConfig::name);
      if (target == next.end() && index < next.size()) {
        target = next.begin() + static_cast<std::ptrdiff_t>(index);
      }
      if (target == next.end()) {
        return;
      }
      const auto targetIndex = static_cast<std::size_t>(std::distance(next.begin(), target));
      next[targetIndex] = *rowState;
      normalizeIdleBehaviorNames(next);
      *rowState = next[targetIndex];
      *rowKey = rowState->name;
      m_b.setOverride({"idle", "behavior"}, next);
      if (m_b.requestRebuild) {
        m_b.requestRebuild();
      }
      if (m_sheet != nullptr && m_sheet->isOpen()) {
        m_sheet->requestLayout();
      }
    };
    const auto removeRow = [this, index]() {
      if (m_b.config == nullptr || !m_b.setOverride) {
        return;
      }
      auto next = m_b.config->config().idle.behaviors;
      if (index >= next.size()) {
        return;
      }
      next.erase(next.begin() + static_cast<std::ptrdiff_t>(index));
      normalizeIdleBehaviorNames(next);
      m_b.setOverride({"idle", "behavior"}, next);
      if (m_sheet != nullptr) {
        m_sheet->close();
      }
      if (m_b.requestRebuild) {
        m_b.requestRebuild();
      }
    };
    if (!m_b.makeContext) {
      return;
    }
    auto ctx = m_b.makeContext();
    ctx.openSessionActionEntryEditor = {};
    ctx.openIdleBehaviorEntryEditor = {};
    ctx.closeHostedEditor = [this]() {
      if (m_sheet != nullptr) {
        m_sheet->close();
      }
    };
    m_sheet->open(
        SettingsSheetRequest{
            .sheetTitle = idleBehaviorTitle(*rowState),
            .removeAction = removeRow,
            .populateSheetBody =
                [ctx, rowState, persist](Flex& body) mutable {
                  buildIdleBehaviorEntryDetailContent(body, ctx, *rowState, persist);
                },
            .scale = uiScale(),
        }
    );
  }

  void HostedEditors::openIdleBehaviorCreate() {
    if (m_b.config == nullptr) {
      return;
    }
    prepareOpen();
    if (m_sheet == nullptr) {
      return;
    }
    auto rowState = std::make_shared<IdleBehaviorConfig>(IdleBehaviorConfig{
        .name = "idle-behavior",
        .enabled = false,
        .timeoutSeconds = 600,
        .action = "command",
        .command = "",
        .resumeCommand = "",
    });
    const auto persistDraft = [this]() {
      if (m_sheet != nullptr && m_sheet->isOpen()) {
        m_sheet->requestLayout();
      }
    };
    if (!m_b.makeContext) {
      return;
    }
    auto ctx = m_b.makeContext();
    ctx.openSessionActionEntryEditor = {};
    ctx.openIdleBehaviorEntryEditor = {};
    ctx.afterIdleBehaviorApply = [this, rowState]() {
      if (m_b.config == nullptr || !m_b.setOverride) {
        return;
      }
      normalizeIdleBehaviorAction(*rowState);
      auto next = m_b.config->config().idle.behaviors;
      next.push_back(*rowState);
      normalizeIdleBehaviorNames(next);
      m_b.setOverride({"idle", "behavior"}, next);
      if (m_b.requestRebuild) {
        m_b.requestRebuild();
      }
    };
    ctx.closeHostedEditor = [this]() {
      if (m_sheet != nullptr) {
        m_sheet->close();
      }
    };
    m_sheet->open(
        SettingsSheetRequest{
            .sheetTitle = idleBehaviorTitle(*rowState),
            .removeAction = nullptr,
            .populateSheetBody =
                [ctx, rowState, persistDraft](Flex& body) mutable {
                  buildIdleBehaviorEntryDetailContent(body, ctx, *rowState, persistDraft);
                },
            .scale = uiScale(),
        }
    );
  }

  void HostedEditors::openNotificationFilterEntry(std::size_t index) {
    if (m_b.config == nullptr) {
      return;
    }
    prepareOpen();
    if (m_sheet == nullptr) {
      return;
    }
    const Config& cfg = m_b.config->config();
    if (index >= cfg.notification.filters.size()) {
      return;
    }
    auto rowState = std::make_shared<NotificationFilterConfig>(cfg.notification.filters[index]);
    auto rowKey = std::make_shared<std::string>(rowState->name);
    const auto persist = [this, rowState, rowKey, index]() {
      if (m_b.config == nullptr || !m_b.setOverride) {
        return;
      }
      auto next = m_b.config->config().notification.filters;
      auto target = std::ranges::find(next, *rowKey, &NotificationFilterConfig::name);
      if (target == next.end() && index < next.size()) {
        target = next.begin() + static_cast<std::ptrdiff_t>(index);
      }
      if (target == next.end()) {
        return;
      }
      const auto targetIndex = static_cast<std::size_t>(std::distance(next.begin(), target));
      next[targetIndex] = *rowState;
      normalizeNotificationFilterNames(next);
      *rowState = next[targetIndex];
      *rowKey = rowState->name;
      m_b.setOverride({"notification", "filter"}, next);
      if (m_b.requestRebuild) {
        m_b.requestRebuild();
      }
      if (m_sheet != nullptr && m_sheet->isOpen()) {
        m_sheet->requestLayout();
      }
    };
    const auto removeRow = [this, index]() {
      if (m_b.config == nullptr || !m_b.setOverride) {
        return;
      }
      auto next = m_b.config->config().notification.filters;
      if (index >= next.size()) {
        return;
      }
      next.erase(next.begin() + static_cast<std::ptrdiff_t>(index));
      normalizeNotificationFilterNames(next);
      m_b.setOverride({"notification", "filter"}, next);
      if (m_sheet != nullptr) {
        m_sheet->close();
      }
      if (m_b.requestRebuild) {
        m_b.requestRebuild();
      }
    };
    if (!m_b.makeContext) {
      return;
    }
    auto ctx = m_b.makeContext();
    ctx.openNotificationFilterEntryEditor = {};
    ctx.afterNotificationFilterApply = [persist]() { persist(); };
    ctx.closeHostedEditor = [this]() {
      if (m_sheet != nullptr) {
        m_sheet->close();
      }
    };
    m_sheet->open(
        SettingsSheetRequest{
            .sheetTitle = notificationFilterTitle(*rowState),
            .removeAction = removeRow,
            .populateSheetBody =
                [ctx, rowState, persist](Flex& body) mutable {
                  buildNotificationFilterEntryDetailContent(body, ctx, *rowState, persist);
                },
            .scale = uiScale(),
        }
    );
  }

  void HostedEditors::openNotificationFilterCreate() {
    if (m_b.config == nullptr) {
      return;
    }
    prepareOpen();
    if (m_sheet == nullptr) {
      return;
    }
    auto rowState = std::make_shared<NotificationFilterConfig>(NotificationFilterConfig{
        .name = "filter",
        .enabled = true,
        .match = {},
        .showToast = true,
        .saveHistory = true,
        .playSound = true,
        .bypassDnd = false,
        .allowPermanent = true,
        .allowedUrgencies = {},
    });
    const auto persistDraft = [this]() {
      if (m_sheet != nullptr && m_sheet->isOpen()) {
        m_sheet->requestLayout();
      }
    };
    if (!m_b.makeContext) {
      return;
    }
    auto ctx = m_b.makeContext();
    ctx.openNotificationFilterEntryEditor = {};
    ctx.afterNotificationFilterApply = [this, rowState]() {
      if (m_b.config == nullptr || !m_b.setOverride || rowState->match.empty()) {
        return;
      }
      auto next = m_b.config->config().notification.filters;
      next.push_back(*rowState);
      normalizeNotificationFilterNames(next);
      m_b.setOverride({"notification", "filter"}, next);
      if (m_b.requestRebuild) {
        m_b.requestRebuild();
      }
    };
    ctx.closeHostedEditor = [this]() {
      if (m_sheet != nullptr) {
        m_sheet->close();
      }
    };
    m_sheet->open(
        SettingsSheetRequest{
            .sheetTitle = i18n::tr("settings.notifications.filter.add-title"),
            .removeAction = nullptr,
            .populateSheetBody =
                [ctx, rowState, persistDraft](Flex& body) mutable {
                  buildNotificationFilterEntryDetailContent(body, ctx, *rowState, persistDraft);
                },
            .scale = uiScale(),
        }
    );
  }

  void HostedEditors::openBarWidgetAdd(const std::vector<std::string>& lanePath) {
    if (m_b.config == nullptr) {
      return;
    }
    prepareOpen();
    if (m_sheet == nullptr) {
      return;
    }

    const Config& cfg = m_b.config->config();
    const float scale = uiScale();
    const auto pickerEntries = widgetPickerEntries(cfg);
    auto normalOptions = std::make_shared<std::vector<SearchPickerOption>>();
    auto instanceOptions = std::make_shared<std::vector<SearchPickerOption>>();
    auto pluginEntries = std::make_shared<std::unordered_set<std::string>>();
    auto instanceMode = std::make_shared<bool>(false);
    auto createType = std::make_shared<std::string>();
    auto createVisible = std::make_shared<bool>(false);

    for (const auto& entry : pickerEntries) {
      normalOptions->push_back(
          SearchPickerOption{
              .value = entry.value,
              .label = entry.label,
              .description = entry.description,
              .enabled = true,
              .icon = entry.icon,
          }
      );
      if (entry.kind == WidgetReferenceKind::Plugin) {
        pluginEntries->insert(entry.value);
      }
      if (entry.kind != WidgetReferenceKind::BuiltIn) {
        continue;
      }
      for (const auto& spec : widgetTypeSpecs()) {
        if (spec.type != entry.value || !spec.supportsMultipleInstances) {
          continue;
        }
        instanceOptions->push_back(
            SearchPickerOption{
                .value = entry.value,
                .label = entry.label,
                .description = i18n::tr("settings.entities.widget.picker.instance-description", "type", entry.value),
                .enabled = true,
                .icon = entry.icon,
            }
        );
        break;
      }
    }

    const auto applySelection = [this, lanePath](
                                     const std::string& value, const std::string& newInstanceType,
                                     const std::string& newInstanceId
                                 ) {
      if (value.empty() || m_b.config == nullptr || !m_b.setOverride) {
        return;
      }
      auto laneItems = barWidgetItemsForPath(m_b.config->config(), lanePath);
      if (!newInstanceType.empty() && !newInstanceId.empty() && m_b.setOverrides) {
        laneItems.push_back(newInstanceId);
        std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides = {
            {{"widget", newInstanceId, "type"}, newInstanceType},
        };
        overrides.emplace_back(lanePath, laneItems);
        m_b.setOverrides(std::move(overrides));
      } else {
        laneItems.push_back(value);
        m_b.setOverride(lanePath, laneItems);
      }
      if (m_sheet != nullptr) {
        m_sheet->close();
      }
      if (m_b.requestRebuild) {
        m_b.requestRebuild();
      }
    };

    const std::string lane = laneLabel(lanePath.empty() ? "" : lanePath.back());
    m_sheet->open(
        SettingsSheetRequest{
            .sheetTitle = i18n::tr("settings.entities.widget.inspector.add-title", "lane", lane),
            .populateSheetBody =
                [this, scale, normalOptions, instanceOptions, pluginEntries, instanceMode, createType, createVisible,
                 applySelection](Flex& body) {
                  body.setGap(Style::spaceSm * scale);
                  auto modeRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
                  modeRow->addChild(
                      ui::label({
                          .text = i18n::tr("settings.entities.widget.picker.instance-toggle"),
                          .fontSize = Style::fontSizeCaption * scale,
                          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                          .flexGrow = 1.0F,
                      })
                  );
                  modeRow->addChild(
                      ui::toggle({
                          .checked = *instanceMode,
                          .scale = scale,
                          .onChange =
                              [this, instanceMode](bool value) {
                                *instanceMode = value;
                                if (m_sheet != nullptr) {
                                  m_sheet->rebuildBody();
                                }
                              },
                      })
                  );
                  body.addChild(std::move(modeRow));

                  SearchPicker* picker = nullptr;
                  Input* instanceInput = nullptr;
                  body.addChild(
                      ui::searchPicker({
                          .out = &picker,
                          .placeholder = i18n::tr("settings.entities.widget.picker.placeholder"),
                          .emptyText = i18n::tr("settings.entities.widget.picker.empty"),
                          .options = *instanceMode ? *instanceOptions : *normalOptions,
                          .flexGrow = 1.0F,
                          .visible = !*createVisible,
                          .onActivated =
                              [this, pluginEntries, instanceMode, createType, createVisible, applySelection,
                               instanceInputPtr = &instanceInput](const SearchPickerOption& option) {
                                if (option.value.empty() || m_b.config == nullptr) {
                                  return;
                                }
                                if (pluginEntries->contains(option.value)) {
                                  std::string base = option.value;
                                  if (const auto pos = base.find_last_of("/:"); pos != std::string::npos) {
                                    base = base.substr(pos + 1);
                                  }
                                  const std::string instanceId =
                                      !widgetReferenceNameExists(m_b.config->config(), base)
                                      ? base
                                      : nextWidgetInstanceId(m_b.config->config(), base);
                                  applySelection(option.value, option.value, instanceId);
                                  return;
                                }
                                if (*instanceMode || widgetTypeRequiresNamedConfig(option.value)) {
                                  *createType = option.value;
                                  *createVisible = true;
                                  if (m_sheet != nullptr) {
                                    m_sheet->rebuildBody();
                                  }
                                  return;
                                }
                                applySelection(option.value, {}, {});
                              },
                      })
                  );

                  const bool showCreate = *createVisible && !createType->empty();
                  body.addChild(
                      ui::label({
                          .text = i18n::tr("settings.entities.widget.instance.id-description"),
                          .fontSize = Style::fontSizeCaption * scale,
                          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                          .maxLines = 2,
                          .visible = showCreate,
                          .participatesInLayout = showCreate,
                      })
                  );
                  body.addChild(
                      ui::input({
                          .out = &instanceInput,
                          .value = showCreate ? std::optional<std::string>{nextWidgetInstanceId(
                                                    m_b.config->config(), *createType
                                                )}
                                              : std::nullopt,
                          .placeholder = i18n::tr("settings.entities.widget.instance.id-placeholder"),
                          .fontSize = Style::fontSizeBody * scale,
                          .controlHeight = Style::controlHeight * scale,
                          .visible = showCreate,
                          .participatesInLayout = showCreate,
                          .onSubmit =
                              [this, createType, applySelection, instanceInputPtr = &instanceInput](
                                  const std::string& /*value*/
                              ) {
                                if (*instanceInputPtr == nullptr || m_b.config == nullptr) {
                                  return;
                                }
                                const std::string id = StringUtils::trim((*instanceInputPtr)->value());
                                if (!isValidWidgetInstanceId(id)
                                    || widgetReferenceNameExists(m_b.config->config(), id)) {
                                  (*instanceInputPtr)->setInvalid(true);
                                  return;
                                }
                                applySelection(id, *createType, id);
                              },
                      })
                  );
                  if (showCreate) {
                    body.addChild(
                        ui::button({
                            .text = i18n::tr("settings.entities.bar.create"),
                            .onClick =
                                [this, createType, applySelection, instanceInput]() {
                                  if (instanceInput == nullptr || m_b.config == nullptr) {
                                    return;
                                  }
                                  const std::string id = StringUtils::trim(instanceInput->value());
                                  if (!isValidWidgetInstanceId(id)
                                      || widgetReferenceNameExists(m_b.config->config(), id)) {
                                    instanceInput->setInvalid(true);
                                    return;
                                  }
                                  applySelection(id, *createType, id);
                                },
                        })
                    );
                  }
                },
            .scale = scale,
            .minWidth = 480.0F,
            .maxWidth = 640.0F,
            .parentFraction = 0.7F,
            .fillParentHeight = true,
            .scrollableBody = false,
        }
    );
  }

  void HostedEditors::openBarWidgetEditorSheet(
      std::string title, std::function<void(Flex&)> populate, std::function<void()> removeAction
  ) {
    if (m_b.config == nullptr) {
      return;
    }
    prepareOpen();
    if (m_sheet == nullptr) {
      return;
    }
    if (!m_b.makeContext) {
      return;
    }
    auto ctx = m_b.makeContext();
    ctx.requestRebuild = [this]() {
      if (m_sheet != nullptr) {
        m_sheet->rebuildBody();
      }
    };
    ctx.closeHostedEditor = [this]() { DeferredCall::callLater([this]() { close(); }); };
    ctx.renameWidgetInstance =
        [this](
            std::string oldName, std::string newName,
            std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> referenceOverrides
        ) {
          std::string updatedTitle = newName;
          if (m_b.renameWidget) {
            m_b.renameWidget(std::move(oldName), std::move(newName), std::move(referenceOverrides));
          } else if (m_b.setOverrides) {
            m_b.setOverrides(std::move(referenceOverrides));
          }
          if (m_b.config != nullptr) {
            const std::string display = widgetReferenceInfo(m_b.config->config(), updatedTitle).title;
            if (!display.empty()) {
              updatedTitle = display;
            }
          }
          if (m_sheet != nullptr) {
            m_sheet->setSheetTitle(updatedTitle);
            m_sheet->rebuildBody();
          }
        };
    if (m_b.editorFactory != nullptr) {
      *m_b.editorFactory = std::make_unique<SettingsControlFactory>(ctx);
    } else {
      m_factory = std::make_unique<SettingsControlFactory>(ctx);
    }
    m_sheet->open(
        SettingsSheetRequest{
            .sheetTitle = std::move(title),
            .removeAction = std::move(removeAction),
            .populateSheetBody = std::move(populate),
            .scale = uiScale(),
        }
    );
  }

  void HostedEditors::openWidgetInspector(std::vector<std::string> lanePath, std::string widgetName) {
    DeferredCall::callLater([this, lanePath = std::move(lanePath), widgetName = std::move(widgetName)]() mutable {
      if (m_b.editingWidgetName != nullptr) {
        *m_b.editingWidgetName = widgetName;
      }
      if (m_b.editingCapsuleGroupId != nullptr) {
        m_b.editingCapsuleGroupId->clear();
      }
      if (m_b.renamingWidgetName != nullptr) {
        m_b.renamingWidgetName->clear();
      }
      if (m_b.pendingDeleteWidgetName != nullptr) {
        m_b.pendingDeleteWidgetName->clear();
      }
      if (m_b.pendingDeleteWidgetSettingPath != nullptr) {
        m_b.pendingDeleteWidgetSettingPath->clear();
      }
      if (m_b.editorListPath != nullptr) {
        *m_b.editorListPath = lanePath;
      } else {
        m_listPath = lanePath;
      }
      std::string title = widgetName;
      if (m_b.config != nullptr) {
        const std::string display = widgetReferenceInfo(m_b.config->config(), widgetName).title;
        if (!display.empty()) {
          title = display;
        }
      }
      openBarWidgetEditorSheet(std::move(title), [this](Flex& body) {
        SettingsControlFactory* factory =
            m_b.editorFactory != nullptr ? m_b.editorFactory->get() : m_factory.get();
        const std::vector<std::string>* listPath =
            m_b.editorListPath != nullptr ? m_b.editorListPath : &m_listPath;
        if (factory == nullptr || listPath == nullptr) {
          return;
        }
        auto ctx = makeBarWidgetEditorContext(*factory);
        buildWidgetInspectorBody(body, *listPath, ctx);
      });
    });
  }

  void HostedEditors::openCapsuleGroup(std::vector<std::string> lanePath, std::string groupId) {
    DeferredCall::callLater([this, lanePath = std::move(lanePath), groupId = std::move(groupId)]() mutable {
      if (m_b.editingCapsuleGroupId != nullptr) {
        *m_b.editingCapsuleGroupId = groupId;
      }
      if (m_b.editingWidgetName != nullptr) {
        m_b.editingWidgetName->clear();
      }
      if (m_b.renamingWidgetName != nullptr) {
        m_b.renamingWidgetName->clear();
      }
      if (m_b.pendingDeleteWidgetName != nullptr) {
        m_b.pendingDeleteWidgetName->clear();
      }
      if (m_b.pendingDeleteWidgetSettingPath != nullptr) {
        m_b.pendingDeleteWidgetSettingPath->clear();
      }
      if (m_b.editorListPath != nullptr) {
        *m_b.editorListPath = std::move(lanePath);
      } else {
        m_listPath = std::move(lanePath);
      }
      openBarWidgetEditorSheet(i18n::tr("settings.entities.widget.group.title"), [this](Flex& body) {
        SettingsControlFactory* factory =
            m_b.editorFactory != nullptr ? m_b.editorFactory->get() : m_factory.get();
        const std::vector<std::string>* listPath =
            m_b.editorListPath != nullptr ? m_b.editorListPath : &m_listPath;
        if (factory == nullptr || listPath == nullptr) {
          return;
        }
        auto ctx = makeBarWidgetEditorContext(*factory);
        buildCapsuleGroupBody(body, *listPath, ctx);
      });
    });
  }

  void HostedEditors::openPluginStore() {
    if (m_b.config == nullptr || m_b.pluginManager == nullptr) {
      return;
    }
    auto* manager = m_b.pluginManager;
    PluginsConfig pluginsSnapshot = m_b.config->config().plugins;
    std::thread([this, manager, pluginsSnapshot = std::move(pluginsSnapshot)]() mutable {
      manager->fetchStaleCatalogs(pluginsSnapshot);

      std::vector<StoreCatalogEntry> catalog;
      for (const auto& source : pluginsSnapshot.sources) {
        if (!source.enabled) {
          continue;
        }
        auto result = scripting::discoverCatalog(source, scripting::CatalogAccess::Network);
        if (!result.ok) {
          continue;
        }
        for (auto& entry : result.entries) {
          catalog.push_back(
              StoreCatalogEntry{
                  .entry = std::move(entry),
                  .source = source.name,
                  .sourceConfig = source,
              }
          );
        }
      }

      DeferredCall::callLater([this, catalog = std::move(catalog)]() mutable {
        if (m_b.config == nullptr || m_b.pluginManager == nullptr) {
          return;
        }
        prepareOpen();
        if (m_sheet == nullptr) {
          return;
        }
        const float scale = uiScale();
        auto catalogLookup = std::make_shared<std::unordered_map<std::string, scripting::CatalogEntry>>();
        for (const auto& entry : catalog) {
          catalogLookup->emplace(entry.entry.id, entry.entry);
        }

        auto storeContent = std::make_shared<PluginStoreContent>(
            std::move(catalog), m_b.config,
            PluginStoreCallbacks{
                .setEnabled =
                    [this, catalogLookup](std::string id, bool enable) {
                      if (m_b.pluginManager == nullptr) {
                        return;
                      }
                      if (enable) {
                        (void)m_b.pluginManager->enable(id);
                      } else {
                        m_b.pluginManager->disable(id);
                      }
                      scripting::CatalogEntry* catalogEntry = nullptr;
                      if (auto it = catalogLookup->find(id); it != catalogLookup->end()) {
                        catalogEntry = &it->second;
                      }
                      if (m_b.onPluginToggled) {
                        m_b.onPluginToggled(id, enable, catalogEntry);
                      }
                      if (m_b.requestRebuild) {
                        m_b.requestRebuild();
                      }
                      if (m_sheet != nullptr) {
                        m_sheet->rebuildBody();
                      }
                    },
                .isEnabling =
                    [this](const std::string& id) {
                      return m_b.pluginManager != nullptr && m_b.pluginManager->isEnabling(id);
                    },
                .isInstalled =
                    [this](const std::string& id) {
                      return m_b.pluginManager != nullptr && m_b.pluginManager->isMaterialized(id);
                    },
                .scale = scale,
            },
            m_b.pluginFileCache != nullptr ? m_b.pluginFileCache : &m_fileCache,
            m_b.pluginStoreScroll != nullptr ? m_b.pluginStoreScroll : &m_pluginStoreScroll
        );

        auto* fileCache = m_b.pluginFileCache != nullptr ? m_b.pluginFileCache : &m_fileCache;
        fileCache->setOnReady([storeContent](
                                   const std::string& pluginId, const std::string& filename, const std::string& path
                               ) { storeContent->onFileReady(pluginId, filename, path); });
        storeContent->setOnRebuildNeeded([this]() {
          if (m_sheet != nullptr) {
            m_sheet->rebuildBody();
          }
        });

        m_sheet->open(
            SettingsSheetRequest{
                .sheetTitle = i18n::tr("settings.plugins.store.title"),
                .removeAction = nullptr,
                .createLeadingAction =
                    [storeContent, scale]() -> std::unique_ptr<Node> {
                      if (!storeContent->isDetailView()) {
                        return nullptr;
                      }
                      return ui::button({
                          .glyph = Style::rtl() ? "chevron-right" : "chevron-left",
                          .glyphSize = Style::fontSizeBody * scale,
                          .variant = ButtonVariant::Ghost,
                          .tooltip = i18n::tr("settings.plugins.store.back-to-catalog"),
                          .minWidth = Style::controlHeightSm * scale,
                          .minHeight = Style::controlHeightSm * scale,
                          .padding = Style::spaceXs * scale,
                          .radius = Style::scaledRadiusMd(scale),
                          .onClick = [storeContent]() { storeContent->closeDetail(); },
                      });
                    },
                .createHeaderAction =
                    [storeContent, scale]() -> std::unique_ptr<Node> {
                      const auto pageUrl = storeContent->detailPageUrl();
                      const auto sourceUrl = storeContent->detailSourceUrl();
                      if (!pageUrl.has_value() && !sourceUrl.has_value()) {
                        return nullptr;
                      }
                      auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
                      if (pageUrl.has_value()) {
                        actions->addChild(
                            ui::button({
                                .glyph = "external-link",
                                .glyphSize = Style::fontSizeBody * scale,
                                .variant = ButtonVariant::Ghost,
                                .tooltip = i18n::tr("settings.plugins.store.open-page"),
                                .minWidth = Style::controlHeightSm * scale,
                                .minHeight = Style::controlHeightSm * scale,
                                .padding = Style::spaceXs * scale,
                                .radius = Style::scaledRadiusMd(scale),
                                .onClick = [url = *pageUrl]() { (void)net::openInBrowser(url); },
                            })
                        );
                      }
                      if (sourceUrl.has_value()) {
                        actions->addChild(
                            ui::button({
                                .glyph = "brand-git",
                                .glyphSize = Style::fontSizeBody * scale,
                                .variant = ButtonVariant::Ghost,
                                .tooltip = i18n::tr("settings.plugins.store.open-source"),
                                .minWidth = Style::controlHeightSm * scale,
                                .minHeight = Style::controlHeightSm * scale,
                                .padding = Style::spaceXs * scale,
                                .radius = Style::scaledRadiusMd(scale),
                                .onClick = [url = *sourceUrl]() { (void)net::openInBrowser(url); },
                            })
                        );
                      }
                      return actions;
                    },
                .populateSheetBody =
                    [this, storeContent, scale](Flex& body) {
                      Renderer* renderer = m_b.renderer ? m_b.renderer() : m_layoutRenderer;
                      if (renderer == nullptr) {
                        return;
                      }
                      if (m_b.config != nullptr && m_b.config->config().shell.offlineMode) {
                        body.addChild(
                            makeOfflineModeNotice(scale, i18n::tr("settings.window.offline-mode-notice.plugin-store"))
                        );
                      }
                      storeContent->populateBody(body, *renderer, m_b.asyncTextures);
                    },
                .scale = scale,
                .minWidth = 800.0F,
                .maxWidth = 1100.0F,
                .parentFraction = 0.85F,
                .fillParentHeight = true,
                .scrollableBody = false,
                .onCloseRequested =
                    [storeContent]() -> bool {
                      if (storeContent->isDetailView()) {
                        storeContent->closeDetail();
                        return true;
                      }
                      return false;
                    },
                .preDispatchKeyboard =
                    [this, storeContent](const KeyboardEvent& event) {
                      InputArea* focused = m_sheet != nullptr ? m_sheet->focusedArea() : nullptr;
                      return storeContent->handleKeyEvent(
                          event.sym, event.modifiers, event.pressed, event.preedit, focused
                      );
                    },
                .onClosed =
                    [this, storeContent, fileCache]() {
                      storeContent->detachGrid();
                      fileCache->setOnReady(nullptr);
                      if (m_b.pluginStoreOpen != nullptr) {
                        *m_b.pluginStoreOpen = false;
                      }
                    },
            }
        );
        if (m_b.pluginStoreOpen != nullptr) {
          *m_b.pluginStoreOpen = m_sheet->isOpen();
        }
      });
    }).detach();
  }

} // namespace settings
