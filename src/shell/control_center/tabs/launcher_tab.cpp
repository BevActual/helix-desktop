#include "shell/control_center/tabs/launcher_tab.h"

#include "compositors/compositor_detect.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "shell/dock/pinned_apps.h"
#include "shell/panel/panel_manager.h"
#include "system/desktop_entry.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/list_editor.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  struct ProviderSpec {
    std::string_view name;
    bool globalByDefault = false;
    std::string_view labelKey;
  };

  constexpr auto kProviders = std::to_array<ProviderSpec>({
      {.name = "calculator", .globalByDefault = true, .labelKey = "settings.schema.panels.launcher-global-calculator.label"},
      {.name = "emoji", .labelKey = "settings.schema.panels.launcher-global-emoji.label"},
      {.name = "session", .labelKey = "settings.schema.panels.launcher-global-session.label"},
      {.name = "wallpaper", .labelKey = "settings.schema.panels.launcher-global-wallpaper.label"},
      {.name = "windows", .labelKey = "settings.schema.panels.launcher-global-windows.label"},
  });

  static_assert(kProviders.size() == 5);

  [[nodiscard]] bool providerGlobal(
      const ShellConfig::LauncherConfig& launcher, std::string_view name, bool fallback
  ) {
    const auto it = std::ranges::find_if(launcher.providers, [&](const LauncherProviderConfig& provider) {
      return provider.name == name;
    });
    if (it == launcher.providers.end()) {
      return fallback;
    }
    return it->global.value_or(fallback);
  }

  [[nodiscard]] std::vector<ListEditorOption> pinnedAppOptions(const std::vector<std::string>& pinned) {
    std::vector<ListEditorOption> options;
    for (const auto& entry : desktopEntries()) {
      if (entry.noDisplay || entry.hidden) {
        continue;
      }
      options.push_back(ListEditorOption{.value = entry.id, .label = entry.name.empty() ? entry.id : entry.name});
    }
    for (const auto& resolved : shell::dock::pinned_apps::resolveEntries(pinned)) {
      const bool known = std::ranges::any_of(options, [&](const ListEditorOption& opt) {
        return opt.value == resolved.id;
      });
      if (!known) {
        options.push_back(ListEditorOption{
            .value = resolved.id,
            .label = resolved.name.empty() ? resolved.id : resolved.name,
        });
      }
    }
    return options;
  }

} // namespace

LauncherTab::LauncherTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "launcher"})) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> LauncherTab::create() {
  const float scale = contentScale();

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
  });

  auto scroll = ui::scrollView({
      .contentScale = scale,
      .scrollbarVisible = true,
      .flexGrow = 1.0F,
      .configure =
          [](ScrollView& scrollView) {
            scrollView.clearFill();
            scrollView.clearBorder();
          },
  });
  auto* content = scroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Style::spaceMd * scale);

  auto addToggleRow = [scale, this](Flex& parent, const std::string& text, Toggle** out, bool checked,
                                    std::vector<std::string> path) {
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .minHeight = Style::controlHeightSm * scale,
    });
    row->addChild(
        ui::label({
            .text = text,
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
    row->addChild(
        ui::toggle({
            .out = out,
            .checkedImmediate = checked,
            .toggleSize = ToggleSize::Medium,
            .scale = scale,
            .onChange =
                [this, path = std::move(path)](bool value) {
                  applyBool(path, value);
                },
        })
    );
    parent.addChild(std::move(row));
  };

  const ShellConfig::LauncherConfig launcher =
      m_config != nullptr ? m_config->config().shell.launcher : ShellConfig::LauncherConfig{};

  auto appearanceCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applyCompactCardStyle(section, scale, opacity, false); },
  });
  appearanceCard->addChild(
      ui::button({
          .text = i18n::tr("control-center.launcher.open"),
          .glyph = "rocket",
          .fontSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Primary,
          .onClick = []() { PanelManager::instance().togglePanel("launcher"); },
      })
  );
  content->addChild(std::move(appearanceCard));

  if (compositors::isNiri()) {
    auto overviewCard = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
    });
    addTitle(*overviewCard, i18n::tr("settings.navigation.groups.overview"), scale);
    overviewCard->addChild(
        ui::label({
            .text = i18n::tr("settings.schema.shell.niri-overview-type-to-launch.description"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 3,
        })
    );
    addToggleRow(
        *overviewCard, i18n::tr("settings.schema.shell.niri-overview-type-to-launch.label"), &m_typeToLaunch,
        m_config != nullptr && m_config->config().shell.niriOverviewTypeToLaunchEnabled,
        {"shell", "niri_overview_type_to_launch_enabled"}
    );
    content->addChild(std::move(overviewCard));
  }

  auto providersCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  addTitle(*providersCard, i18n::tr("control-center.launcher.providers"), scale);
  for (std::size_t i = 0; i < kProviders.size(); ++i) {
    const auto& spec = kProviders[i];
    addToggleRow(
        *providersCard, i18n::tr(spec.labelKey), &m_providerGlobal[i],
        providerGlobal(launcher, spec.name, spec.globalByDefault),
        {"shell", "launcher", "providers", std::string(spec.name), "global"}
    );
  }
  content->addChild(std::move(providersCard));

  auto pinnedCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  addTitle(*pinnedCard, i18n::tr("settings.schema.panels.launcher-pinned-apps.label"), scale);
  auto pinnedList = std::make_unique<ListEditor>();
  m_pinnedList = pinnedList.get();
  m_pinnedList->setScale(scale);
  m_pinnedList->setAddPlaceholder(i18n::tr("control-center.launcher.add-app"));
  m_pinnedList->setOnAddRequested([this](std::string value) {
    if (m_syncing || m_config == nullptr || value.empty()) {
      return;
    }
    auto items = m_config->config().shell.launcher.pinned;
    items.push_back(std::move(value));
    commitPinned(std::move(items));
  });
  m_pinnedList->setOnRemoveRequested([this](std::size_t index) {
    if (m_syncing || m_config == nullptr) {
      return;
    }
    auto items = m_config->config().shell.launcher.pinned;
    if (index >= items.size()) {
      return;
    }
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
    commitPinned(std::move(items));
  });
  m_pinnedList->setOnMoveRequested([this](std::size_t from, std::size_t to) {
    if (m_syncing || m_config == nullptr || from == to) {
      return;
    }
    auto items = m_config->config().shell.launcher.pinned;
    if (from >= items.size() || to >= items.size()) {
      return;
    }
    const std::string item = items[from];
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(from));
    items.insert(items.begin() + static_cast<std::ptrdiff_t>(to), item);
    commitPinned(std::move(items));
  });
  pinnedCard->addChild(std::move(pinnedList));
  content->addChild(std::move(pinnedCard));

  live->addChild(std::move(scroll));
  auto wrapped = m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
}

std::unique_ptr<Flex> LauncherTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void LauncherTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_typeToLaunch = nullptr;
  m_providerGlobal = {};
  m_pinnedList = nullptr;
}

void LauncherTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool LauncherTab::configureMode() const { return m_settings->configureMode(); }

void LauncherTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

SettingsPane* LauncherTab::settingsPane() { return m_settings.get(); }

void LauncherTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void LauncherTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
}

void LauncherTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }

  const auto& launcher = m_config->config().shell.launcher;
  m_syncing = true;
  if (m_typeToLaunch != nullptr) {
    m_typeToLaunch->setCheckedImmediate(m_config->config().shell.niriOverviewTypeToLaunchEnabled);
  }
  for (std::size_t i = 0; i < kProviders.size(); ++i) {
    if (m_providerGlobal[i] != nullptr) {
      m_providerGlobal[i]->setCheckedImmediate(
          providerGlobal(launcher, kProviders[i].name, kProviders[i].globalByDefault)
      );
    }
  }
  if (m_pinnedList != nullptr) {
    m_pinnedList->setSuggestedOptions(pinnedAppOptions(launcher.pinned));
    m_pinnedList->setItems(launcher.pinned);
  }
  m_syncing = false;
}

void LauncherTab::applyBool(std::vector<std::string> path, bool value) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride(std::move(path), value);
  PanelManager::instance().refresh();
}

void LauncherTab::commitPinned(std::vector<std::string> items) {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "launcher", "pinned"}, std::move(items));
  PanelManager::instance().refresh();
}
