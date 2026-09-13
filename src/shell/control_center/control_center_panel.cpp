#include "scripting/plugin_i18n.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_registry.h"
#include "scripting/plugin_runtime_context.h"
#include "shell/control_center/control_center_panel.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/control_center/plugin_control_center_tab.h"
#include "shell/control_center/tabs/audio_tab.h"
#include "shell/control_center/tabs/bluetooth_tab.h"
#include "shell/control_center/tabs/calendar_tab.h"
#include "shell/control_center/tabs/home_tab.h"
#include "shell/control_center/tabs/media_tab.h"
#include "shell/control_center/tabs/monitor_tab.h"
#include "shell/control_center/tabs/network_tab.h"
#include "shell/control_center/tabs/notifications_tab.h"
#include "shell/control_center/tabs/power_tab.h"
#include "shell/control_center/tabs/screen_time_tab.h"
#include "shell/control_center/tabs/settings_tab.h"
#include "shell/control_center/tabs/system_tab.h"
#include "shell/control_center/tabs/wallpaper_tab.h"
#include "shell/control_center/tabs/weather_tab.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "shell/settings/settings_content_plugins.h"
#include "shell/panel/panel_manager.h"
#include "wayland/wayland_seat.h"
#include "system/dependency_service.h"
#include "system/easyeffects_service.h"
#include "system/screen_time_service.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/roving_list_nav.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/separator.h"
#include "ui/scroll_into_view.h"
#include "ui/split_pane_focus.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <wayland-client-protocol.h>

using namespace control_center;

namespace {

  constexpr auto kMprisRefreshMinInterval = std::chrono::milliseconds(750);
  constexpr float kTabViewportClipInset = 1.0F;

  float tabContentHeight(float viewportHeight) {
    return std::max(1.0F, std::floor(viewportHeight - kTabViewportClipInset));
  }

  using TabGroup = ControlCenterPanel::TabGroup;

  struct BuiltinTabSpec {
    const char* id;
    const char* titleKey;
    const char* glyph;
    bool hideable;
    TabGroup group;
  };

  constexpr BuiltinTabSpec kBuiltinTabs[] = {
      {"home", "control-center.tabs.home", "home", false, TabGroup::Home},
      {"appearance", "settings.navigation.sections.appearance", "adjustments-horizontal", true, TabGroup::Look},
      {"wallpaper", "settings.navigation.sections.wallpaper", "paint", true, TabGroup::Look},
      {"templates", "settings.navigation.sections.templates", "color-swatch", true, TabGroup::Look},
      {"bar", "settings.navigation.sections.bar", "crop-3-2", true, TabGroup::Look},
      {"media", "control-center.tabs.media", "disc-filled", true, TabGroup::Status},
      {"audio", "control-center.tabs.audio", "volume", true, TabGroup::Status},
      {"monitor", "control-center.tabs.monitor", "device-desktop", true, TabGroup::Status},
      {"system", "control-center.tabs.system", "activity-heartbeat", true, TabGroup::Status},
      {"power", "control-center.tabs.power", "battery-charging-2", true, TabGroup::Status},
      {"network", "control-center.tabs.network", "wifi", true, TabGroup::Status},
      {"bluetooth", "control-center.tabs.bluetooth", "bluetooth", true, TabGroup::Status},
      {"weather", "control-center.tabs.weather", "weather-cloud-sun", true, TabGroup::Status},
      {"calendar", "control-center.tabs.calendar", "calendar-event", true, TabGroup::Status},
      {"notifications", "control-center.tabs.notifications", "bell", true, TabGroup::Status},
      {"screen-time", "control-center.tabs.screen-time", "hourglass", true, TabGroup::Status},
      {"desktop", "settings.navigation.sections.desktop", "layout-board", true, TabGroup::Setup},
      {"dock", "settings.navigation.sections.dock", "layout-bottombar-inactive", true, TabGroup::Setup},
      {"panels", "settings.navigation.sections.panels", "layout-bottombar", true, TabGroup::Setup},
      {"launcher", "settings.navigation.sections.launcher", "rocket", true, TabGroup::Setup},
      {"control-center", "settings.navigation.sections.control-center", "adjustments", true, TabGroup::Setup},
      {"osd", "settings.navigation.sections.osd", "message-circle", true, TabGroup::Setup},
      {"screenshot", "settings.navigation.sections.screenshot", "screenshot", true, TabGroup::Setup},
      {"shell", "settings.navigation.sections.shell", "app-window", true, TabGroup::Setup},
      {"keybinds", "settings.navigation.sections.keybinds", "keyboard", true, TabGroup::Setup},
      {"security", "settings.navigation.sections.security", "shield-lock", true, TabGroup::Setup},
      {"hooks", "settings.navigation.sections.hooks", "link", true, TabGroup::Setup},
      {"niri", "settings.navigation.sections.niri", "niri", true, TabGroup::Setup},
      {"plugins", "settings.navigation.sections.plugins", "puzzle", true, TabGroup::Setup},
  };

  std::string groupCaption(TabGroup group) {
    switch (group) {
    case TabGroup::Look:
      return i18n::tr("control-center.sidebar.look");
    case TabGroup::Status:
      return i18n::tr("control-center.sidebar.status");
    case TabGroup::Setup:
      return i18n::tr("control-center.sidebar.setup");
    case TabGroup::Home:
      break;
    }
    return {};
  }

  struct PluginTabChrome {
    std::string title;
    std::string glyph;
  };

  PluginTabChrome pluginTabChrome(const scripting::ResolvedPluginEntry& resolved) {
    PluginTabChrome chrome;
    chrome.title = resolved.manifest != nullptr ? resolved.manifest->name : std::string{};
    if (resolved.entry != nullptr && !resolved.entry->tabTitleKey.empty()) {
      scripting::PluginTranslationCatalog catalog;
      catalog.load(resolved.pluginDir);
      if (catalog.has(resolved.entry->tabTitleKey)) {
        chrome.title = catalog.translate(resolved.entry->tabTitleKey);
      }
    }
    if (resolved.entry != nullptr && !resolved.entry->tabGlyph.empty()) {
      chrome.glyph = resolved.entry->tabGlyph;
    } else if (resolved.manifest != nullptr && !resolved.manifest->icon.empty()) {
      chrome.glyph = resolved.manifest->icon;
    } else {
      chrome.glyph = "puzzle";
    }
    return chrome;
  }

  const BuiltinTabSpec* specById(std::string_view id) {
    for (const auto& spec : kBuiltinTabs) {
      if (id == spec.id) {
        return &spec;
      }
    }
    return nullptr;
  }

  std::string builtinTitle(const BuiltinTabSpec& spec) { return i18n::tr(spec.titleKey); }

  constexpr std::string_view kSettingsTabPrefix = "settings:";

  struct SettingsOpenTarget {
    bool isSettings = false;
    std::string section;
  };

  SettingsOpenTarget parseSettingsOpenContext(std::string_view context) {
    if (context == "settings") {
      return {.isSettings = true};
    }
    if (context.starts_with(kSettingsTabPrefix)) {
      return {.isSettings = true, .section = std::string(context.substr(kSettingsTabPrefix.size()))};
    }
    return {};
  }

  bool isSettingsEditorTab(std::string_view id) {
    return id == "appearance"
        || id == "templates"
        || id == "bar"
        || id == "launcher"
        || id == "dock"
        || id == "desktop"
        || id == "panels"
        || id == "osd"
        || id == "screenshot"
        || id == "shell"
        || id == "keybinds"
        || id == "security"
        || id == "hooks"
        || id == "niri"
        || id == "plugins"
        || id == "control-center";
  }

  bool isLiveSettingsHost(std::string_view id) {
    return id == "weather"
        || id == "notifications"
        || id == "power"
        || id == "system"
        || id == "calendar"
        || id == "audio"
        || id == "monitor"
        || id == "wallpaper"
        || id == "media"
        || id == "screen-time";
  }

  std::string tabIdForSettingsSection(std::string_view section) {
    if (section.empty()) {
      return "appearance";
    }
    if (section == "location") {
      return "weather";
    }
    if (section == "services") {
      return "calendar";
    }
    return std::string(section);
  }

} // namespace

ControlCenterPanel::ControlCenterPanel(const ControlCenterServices& services) {
  m_config = services.config;
  m_mpris = services.mpris;
  m_notificationManager = services.notifications;
  m_dependencies = services.dependencies;
  m_scriptApi = services.scriptApi;
  m_fileWatcher = services.fileWatcher;
  m_httpClient = services.httpClient;
  m_clipboard = services.clipboard;
  m_platform = services.platform;
  m_pluginManager = services.pluginManager;
  m_asyncTextures = services.asyncTextures;
  m_hostedEditors = std::make_unique<settings::HostedEditors>();
  ControlCenterServices tabServices = services;
  tabServices.hostedEditors = m_hostedEditors.get();
  tabServices.modalHost = &m_modalHost;
  appendBuiltinSlots(tabServices);
}

void ControlCenterPanel::appendBuiltinSlots(const ControlCenterServices& services) {
  WaylandConnection* wayland = services.platform != nullptr ? &services.platform->wayland() : nullptr;

  auto add = [this](std::string_view id, std::unique_ptr<Tab> tab) {
    const auto* spec = specById(id);
    if (spec == nullptr) {
      return;
    }
    TabSlot slot;
    slot.id = spec->id;
    slot.title = builtinTitle(*spec);
    slot.glyph = spec->glyph;
    slot.tab = std::move(tab);
    slot.hideable = spec->hideable;
    slot.group = spec->group;
    slot.plugin = false;
    m_slots.push_back(std::move(slot));
  };

  add("home", std::make_unique<HomeTab>(services));
  add("appearance", std::make_unique<SettingsTab>(services, "appearance"));
  add("wallpaper", std::make_unique<WallpaperTab>(services));
  add("bar", std::make_unique<SettingsTab>(services, "bar"));
  add(
      "media",
      std::make_unique<MediaTab>(
          services.mpris, services.httpClient, services.spectrum, services.config, wayland,
          PanelManager::instance().renderContext(), services
      )
  );
  add(
      "audio",
      std::make_unique<AudioTab>(
          services.audio, services.easyEffects, services.mpris, services.config, wayland,
          PanelManager::instance().renderContext(), services
      )
  );
  add("monitor", std::make_unique<MonitorTab>(services.brightness, services.config, services.displayConfig, services));
  add("system", std::make_unique<SystemTab>(services.sysmon, services));
  add("power", std::make_unique<PowerTab>(services.upower, services.powerProfiles, &services));
  add(
      "network",
      std::make_unique<NetworkTab>(services.network, services.networkSecrets, services.externalIp, services.modem)
  );
  add("bluetooth", std::make_unique<BluetoothTab>(services.bluetooth, services.bluetoothAgent));
  add("weather", std::make_unique<WeatherTab>(services.weather, services.config, services));
  add("calendar", std::make_unique<CalendarTab>(services.config, services.calendar, services));
  add("notifications", std::make_unique<NotificationsTab>(services.notifications, services.platform, services));
  add("screen-time", std::make_unique<ScreenTimeTab>(services.screenTime, services));
  add("templates", std::make_unique<SettingsTab>(services, "templates"));
  add("launcher", std::make_unique<SettingsTab>(services, "launcher"));
  add("dock", std::make_unique<SettingsTab>(services, "dock"));
  add("desktop", std::make_unique<SettingsTab>(services, "desktop"));
  add("panels", std::make_unique<SettingsTab>(services, "panels"));
  add("osd", std::make_unique<SettingsTab>(services, "osd"));
  add("screenshot", std::make_unique<SettingsTab>(services, "screenshot"));
  add("shell", std::make_unique<SettingsTab>(services, "shell"));
  add("keybinds", std::make_unique<SettingsTab>(services, "keybinds"));
  add("security", std::make_unique<SettingsTab>(services, "security"));
  add("hooks", std::make_unique<SettingsTab>(services, "hooks"));
  add("niri", std::make_unique<SettingsTab>(services, "niri"));
  add("plugins", std::make_unique<SettingsTab>(services, "plugins"));
  add("control-center", std::make_unique<SettingsTab>(services, "control-center"));
}

float ControlCenterPanel::preferredWidth() const {
  const float fullSize = m_config != nullptr ? static_cast<float>(m_config->config().controlCenter.width)
                                             : static_cast<float>(ControlCenterConfig::kDefaultWidth);
  switch (sidebarModeForOpen(pendingOpenContext())) {
  case ControlCenterSidebarMode::Full:
    return fullSize * m_contentScale;
  case ControlCenterSidebarMode::None:
    return fullSize * 0.75F * m_contentScale;
  default:
  case ControlCenterSidebarMode::Compact:
    return fullSize * 0.85F * m_contentScale;
  }
}

PanelPlacement ControlCenterPanel::panelPlacement() const noexcept {
  return m_config == nullptr ? PanelPlacement::Attached : m_config->config().shell.panel.controlCenterPlacement;
}

bool ControlCenterPanel::dismissTransientUi() {
  if (m_modalHost.isOpen()) {
    m_modalHost.requestCloseTop();
    return true;
  }
  const auto* slot = slotById(m_activeTabId);
  return slot != nullptr && slot->tab != nullptr && slot->tab->dismissTransientUi();
}

void ControlCenterPanel::create() {
  const float scale = contentScale();
  const ControlCenterSidebarMode sidebarMode = sidebarModeForOpen(pendingOpenContext());
  m_compact = sidebarMode == ControlCenterSidebarMode::Compact;
  m_showSidebar = sidebarMode != ControlCenterSidebarMode::None;

  rebuildPluginSlots();

  for (auto& slot : m_slots) {
    if (slot.tab != nullptr) {
      slot.tab->setContentScale(scale);
      slot.tab->setPanelCardOpacity(panelCardOpacity());
    }
    if (!slot.plugin) {
      for (const auto& spec : kBuiltinTabs) {
        if (slot.id == spec.id) {
          slot.title = builtinTitle(spec);
          break;
        }
      }
    }
  }

  auto rootLayout = ui::row({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::panelPadding * scale,
      .padding = 0.0F,
  });

  if (m_showSidebar) {
    auto sidebar = ui::column({
        .out = &m_sidebar,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .padding = Style::spaceMd * scale,
        .fillWidth = false,
        .fillHeight = true,
        .configure = [this, scale](Flex& column) {
          column.setFill(colorSpecFromRole(ColorRole::SurfaceVariant, panelCardOpacity()));
          column.setRadius(Style::scaledRadiusXl(scale));
        },
    });

    auto sidebarScrollArea = ui::inputArea({});
    sidebarScrollArea->setParticipatesInLayout(false);
    sidebarScrollArea->setZIndex(-1);
    m_sidebarScrollArea = sidebarScrollArea.get();
    wireSidebarScroll(m_sidebarScrollArea);
    sidebar->addChild(std::move(sidebarScrollArea));
    sidebar->addChild(createSettingsSearchInput(scale));

    const std::optional<float> sidebarScrollWidth =
        m_compact ? std::optional<float>{Style::controlHeightSm * scale} : std::nullopt;

    auto sidebarScroll = ui::scrollView({
        .out = &m_sidebarScrollView,
        .state = &m_sidebarScrollState,
        .contentScale = scale,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .fillWidth = false,
        .fillHeight = true,
        .width = sidebarScrollWidth,
        .flexGrow = 1.0F,
        .configure = [](ScrollView& scrollView) {
          scrollView.clearFill();
          scrollView.clearBorder();
        },
    });

    auto sidebarNav = std::make_unique<RovingListNavHost>(RovingListNavController::Options{
        .axis = RovingListNavAxis::Vertical,
        .mode = RovingListNavMode::FollowFocus,
        .keepItemsInTabOrder = false,
        .wrap = true,
        .scrollIntoView = [this](const Node* node) { scrollSidebarNodeIntoView(node); },
        .syncIndexFromSelection = {},
    });
    sidebarNav->setTabFocusKey("control-center.sidebar");
    if (!m_compact) {
      sidebarNav->setAlign(FlexAlign::Stretch);
      sidebarNav->setFillWidth(true);
    } else {
      sidebarNav->setAlign(FlexAlign::Center);
    }
    sidebarNav->setGap(Style::spaceXs * scale);
    m_sidebarNav = sidebarNav.get();
    m_lookHeader = nullptr;
    m_statusHeader = nullptr;
    m_setupHeader = nullptr;

    TabGroup lastHeader = TabGroup::Home;
    for (auto& slot : m_slots) {
      if (slot.group != TabGroup::Home && slot.group != lastHeader) {
        addSidebarGroupHeader(*sidebarNav, slot.group, scale);
        lastHeader = slot.group;
      }
      addSidebarTabButton(*sidebarNav, slot, scale);
    }

    if (sidebarScroll->content() != nullptr) {
      if (!m_compact) {
        sidebarScroll->content()->setAlign(FlexAlign::Stretch);
      }
      sidebarScroll->content()->addChild(std::move(sidebarNav));
    }
    sidebar->addChild(std::move(sidebarScroll));
    rootLayout->addChild(std::move(sidebar));
  }

  auto content = ui::column({
      .out = &m_content,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .clipChildren = true,
      .flexGrow = 4.0F,
  });

  auto dismissArea = ui::inputArea({});
  dismissArea->setParticipatesInLayout(false);
  dismissArea->setZIndex(-1);
  dismissArea->setFocusable(false);
  dismissArea->setTabStop(false);
  dismissArea->setOnPress([this](const InputArea::PointerData&) {
    if (dismissTransientUi()) {
      PanelManager::instance().refresh();
    }
  });
  m_contentDismissArea = static_cast<InputArea*>(content->addChild(std::move(dismissArea)));

  auto header = ui::row({
      .out = &m_contentHeader,
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceSm * scale,
  });

  header->addChild(
      ui::label({
          .out = &m_contentTitle,
          .text = i18n::tr("control-center.tabs.home"),
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Primary),
          .flexGrow = 1.0F,
      })
  );

  auto headerActions = ui::row({
      .out = &m_contentHeaderActions,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });

  for (auto& slot : m_slots) {
    if (slot.tab == nullptr) {
      continue;
    }
    auto actions = slot.tab->createHeaderActions();
    slot.headerActions = actions.get();
    if (actions != nullptr) {
      actions->setVisible(false);
      m_contentHeaderActions->addChild(std::move(actions));
    }
  }

  m_contentHeaderActions->addChild(
      ui::button({
          .out = &m_closeButton,
          .glyph = "close",
          .onClick = []() { PanelManager::instance().close(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  header->addChild(std::move(headerActions));

  content->addChild(std::move(header));
  if (!m_showSidebar) {
    content->addChild(createSettingsSearchInput(scale));
  }

  auto bodies = ui::column({
      .out = &m_tabBodies,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .clipChildren = true,
      .flexGrow = 1.0F,
  });

  for (auto& slot : m_slots) {
    if (slot.tab == nullptr) {
      continue;
    }
    auto container = slot.tab->create();
    container->setFlexGrow(1.0F);
    container->setParticipatesInLayout(false);
    container->setVisible(false);
    slot.container = container.get();
    m_tabBodies->addChild(std::move(container));
  }

  content->addChild(std::move(bodies));
  rootLayout->addChild(std::move(content));
  setRoot(std::move(rootLayout));

  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  m_modalHost.initialize(
      PanelManager::instance().inputDispatcher(),
      []() { PanelManager::instance().requestLayout(); },
      []() { PanelManager::instance().requestUpdateOnly(); }
  );
  if (m_hostedEditors != nullptr) {
    m_hostedEditors->initialize(m_modalHost, []() {});
  }
  m_modalAttached = false;

  syncTabVisibility();
  m_firstOpenAfterCreate = true;
  selectTab(m_activeTabId);
}

void ControlCenterPanel::onPanelCardOpacityChanged(float opacity) {
  for (auto& slot : m_slots) {
    if (slot.tab != nullptr) {
      slot.tab->setPanelCardOpacity(opacity);
    }
  }
  if (m_sidebar != nullptr) {
    m_sidebar->setFill(colorSpecFromRole(ColorRole::SurfaceVariant, opacity));
  }
}

void ControlCenterPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr || m_content == nullptr || m_tabBodies == nullptr) {
    return;
  }

  if (m_showSidebar) {
    layoutFullSidebarWidth(renderer);
  }

  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);

  if (m_showSidebar && m_sidebarScrollView != nullptr) {
    const float widthBefore = m_sidebarScrollView->width();
    layoutFullSidebarWidth(renderer);
    if (std::abs(m_sidebarScrollView->width() - widthBefore) > 0.5F) {
      m_rootLayout->layout(renderer);
    }
  }

  const float contentInnerWidth =
      std::max(0.0F, m_content->width() - (m_content->paddingLeft() + m_content->paddingRight()));
  const float bodyWidth = m_tabBodies->width();
  const float bodyHeight = m_tabBodies->height();
  const float bodyContentHeight = tabContentHeight(bodyHeight);

  if (m_sidebarScrollArea != nullptr && m_sidebar != nullptr) {
    m_sidebarScrollArea->setPosition(0.0F, 0.0F);
    m_sidebarScrollArea->setSize(m_sidebar->width(), m_sidebar->height());
  }

  if (m_contentDismissArea != nullptr) {
    m_contentDismissArea->setPosition(0.0F, 0.0F);
    m_contentDismissArea->setFrameSize(m_content->width(), m_content->height());
  }

  if (m_contentHeader != nullptr) {
    m_contentHeader->setSize(contentInnerWidth, 0.0F);
  }

  if (m_contentTitle != nullptr) {
    const float actionsWidth = m_contentHeaderActions != nullptr ? m_contentHeaderActions->width() : 0.0F;
    const float headerGap = m_contentHeader != nullptr ? m_contentHeader->gap() : 0.0F;
    const float titleWidth = std::max(0.0F, contentInnerWidth - actionsWidth - headerGap);
    m_contentTitle->setMaxWidth(titleWidth);
  }

  for (auto& slot : m_slots) {
    if (slot.container != nullptr && slot.container->visible()) {
      slot.container->setSize(bodyWidth, bodyContentHeight);
    }
  }

  layoutTabContainers(bodyWidth, bodyHeight);

  const auto layoutSlot = [this, bodyWidth, bodyContentHeight, &renderer](std::string_view id) {
    auto* slot = slotById(id);
    if (slot == nullptr || slot->tab == nullptr || slot->container == nullptr || !slot->container->visible()) {
      return;
    }
    slot->tab->layout(renderer, bodyWidth, bodyContentHeight);
  };

  if (m_tabTransitionActive) {
    layoutSlot(m_tabTransitionOutgoingId);
  }
  layoutSlot(m_activeTabId);
  attachModalHost(renderer, width, height);
}

void ControlCenterPanel::attachModalHost(Renderer& renderer, float width, float height) {
  Node* sceneRoot = root();
  if (sceneRoot == nullptr) {
    return;
  }
  if (!m_modalAttached) {
    m_modalHost.attach(*sceneRoot, m_content, renderer, width, height);
    m_modalAttached = true;
  } else {
    m_modalHost.resize(width, height);
  }
  if (m_hostedEditors != nullptr) {
    m_hostedEditors->setLayoutRenderer(&renderer);
  }
}

void ControlCenterPanel::doUpdate(Renderer& renderer) {
  if (!isTabFeatureAvailable(m_activeTabId) || (!m_activeTabForced && !isTabVisible(m_activeTabId))) {
    selectTab(firstVisibleTab());
  } else {
    syncTabVisibility();
  }
  if (auto* slot = slotById(m_activeTabId); slot != nullptr && slot->tab != nullptr) {
    slot->tab->update(renderer);
  }
  if (m_modalHost.isOpen()) {
    m_modalHost.update(renderer);
  }
}

void ControlCenterPanel::onFrameTick(float deltaMs) {
  if (auto* slot = slotById(m_activeTabId); slot != nullptr && slot->tab != nullptr) {
    slot->tab->onFrameTick(deltaMs);
  }
}

void ControlCenterPanel::onOpen(std::string_view context) {
  if (m_dependencies != nullptr) {
    m_dependencies->rescan();
  }
  const bool animateTabSwitch = !m_firstOpenAfterCreate;
  m_firstOpenAfterCreate = false;
  const auto settingsTarget = parseSettingsOpenContext(context);
  const std::string tabId = settingsTarget.isSettings ? tabIdForSettingsSection(settingsTarget.section)
                                                     : (context.empty() ? "home" : std::string(context));
  selectTab(tabId, animateTabSwitch);
  if (auto* slot = slotById(tabId); slot != nullptr && slot->tab != nullptr) {
    if (settingsTarget.isSettings) {
      slot->tab->setConfigureMode(true);
    } else if (isLiveSettingsHost(tabId)) {
      slot->tab->setConfigureMode(false);
    }
  }
  maybeOpenPendingEditor();
}

void ControlCenterPanel::queuePluginSettings(std::string pluginId) {
  m_pendingOpenPluginSettingsId = std::move(pluginId);
  m_pendingOpenWidgetInspectorName.clear();
  m_pendingOpenBarName.clear();
}

void ControlCenterPanel::queueBarWidget(std::string barName, std::string widgetName) {
  m_pendingOpenBarName = std::move(barName);
  m_pendingOpenWidgetInspectorName = std::move(widgetName);
  m_pendingOpenPluginSettingsId.clear();
}

void ControlCenterPanel::invalidatePluginSource(const std::string& sourceName) {
  if (m_hostedEditors != nullptr) {
    m_hostedEditors->invalidatePluginSource(sourceName);
  }
}

void ControlCenterPanel::maybeOpenPendingEditor() {
  if (!m_pendingOpenPluginSettingsId.empty()) {
    std::string pluginId = std::move(m_pendingOpenPluginSettingsId);
    m_pendingOpenPluginSettingsId.clear();
    if (auto* slot = slotById("plugins"); slot != nullptr && slot->tab != nullptr) {
      if (auto* pane = slot->tab->settingsPane(); pane != nullptr) {
        pane->openPluginSettings(std::move(pluginId));
      }
    }
    return;
  }
  if (!m_pendingOpenWidgetInspectorName.empty()) {
    std::string barName = std::move(m_pendingOpenBarName);
    std::string widgetName = std::move(m_pendingOpenWidgetInspectorName);
    m_pendingOpenBarName.clear();
    m_pendingOpenWidgetInspectorName.clear();
    if (auto* slot = slotById("bar"); slot != nullptr && slot->tab != nullptr) {
      if (auto* pane = slot->tab->settingsPane(); pane != nullptr) {
        pane->openWidgetInspector(std::move(barName), std::move(widgetName));
      }
    }
  }
}

bool ControlCenterPanel::isContextActive(std::string_view context) const {
  const auto settingsTarget = parseSettingsOpenContext(context);
  if (settingsTarget.isSettings) {
    if (settingsTarget.section.empty()) {
      return isSettingsEditorTab(m_activeTabId);
    }
    const std::string tabId = tabIdForSettingsSection(settingsTarget.section);
    if (m_activeTabId != tabId) {
      return false;
    }
    if (isLiveSettingsHost(tabId)) {
      if (const auto* slot = slotById(tabId); slot != nullptr && slot->tab != nullptr) {
        return slot->tab->configureMode();
      }
      return false;
    }
    return true;
  }
  return (context.empty() ? "home" : context) == m_activeTabId;
}

bool ControlCenterPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  KeyboardEvent event;
  event.sym = sym;
  event.modifiers = modifiers;
  event.pressed = pressed;
  event.preedit = preedit;
  if (m_modalHost.onKeyboardEvent(event)) {
    return true;
  }
  if (!m_showSidebar || m_sidebarNav == nullptr || m_sidebarScrollView == nullptr || m_content == nullptr) {
    return false;
  }

  const SplitPaneFocusConfig panes{
      .sidebarFocus = m_sidebarNav->focusArea(),
      .sidebarRoot = m_sidebarScrollView,
      .contentRoot = m_content,
      .headerFocus = nullptr,
  };
  auto& dispatcher = PanelManager::instance().inputDispatcher();
  const SplitPaneFocusResult splitResult =
      handleSplitPaneFocusNavigation(dispatcher, panes, sym, modifiers, pressed, preedit);
  return splitResult == SplitPaneFocusResult::Consumed;
}

void ControlCenterPanel::onClose() {
  if (m_tabTransitionAnimId != 0 && m_animations != nullptr) {
    m_animations->cancel(m_tabTransitionAnimId);
    m_tabTransitionAnimId = 0;
  }
  m_tabTransitionActive = false;
  m_activeTabId = "home";
  m_activeTabForced = false;
  for (auto& slot : m_slots) {
    if (slot.tab != nullptr) {
      slot.tab->setActive(false);
      slot.tab->onClose();
    }
    slot.button = nullptr;
    slot.container = nullptr;
    slot.headerActions = nullptr;
  }
  std::erase_if(m_slots, [](const TabSlot& slot) { return slot.plugin; });
  m_rootLayout = nullptr;
  m_sidebar = nullptr;
  m_sidebarScrollView = nullptr;
  m_sidebarScrollState = {};
  m_sidebarNav = nullptr;
  m_sidebarScrollArea = nullptr;
  m_lookHeader = nullptr;
  m_statusHeader = nullptr;
  m_setupHeader = nullptr;
  m_searchInput = nullptr;
  m_searchQuery.clear();
  m_content = nullptr;
  m_contentDismissArea = nullptr;
  m_contentHeader = nullptr;
  m_contentHeaderActions = nullptr;
  m_contentTitle = nullptr;
  m_closeButton = nullptr;
  m_tabBodies = nullptr;
  if (m_hostedEditors != nullptr) {
    m_hostedEditors->close();
  }
  m_modalHost.closeAll();
  m_modalHost.detach();
  m_modalAttached = false;
  clearReleasedRoot();
}

bool ControlCenterPanel::deferExternalRefresh() const {
  if (m_activeTabId != "audio") {
    return false;
  }
  const auto* slot = slotById("audio");
  const auto* audioTab = slot != nullptr ? dynamic_cast<const AudioTab*>(slot->tab.get()) : nullptr;
  return audioTab != nullptr && audioTab->dragging();
}

bool ControlCenterPanel::deferPointerRelayout() const { return deferExternalRefresh(); }

bool ControlCenterPanel::isTabFeatureAvailable(std::string_view id) const {
  if (id == "niri") {
    return compositors::isNiri();
  }
  return true;
}

bool ControlCenterPanel::isTabVisible(std::string_view id) const {
  if (!isTabFeatureAvailable(id)) {
    return false;
  }
  const auto* slot = slotById(id);
  if (slot == nullptr) {
    return false;
  }
  if (!slot->hideable || m_config == nullptr) {
    return true;
  }
  return !std::ranges::contains(m_config->config().controlCenter.hiddenTabs, id);
}

bool ControlCenterPanel::isTabShown(std::string_view id) const {
  if (id == m_activeTabId && m_activeTabForced) {
    return isTabFeatureAvailable(id);
  }
  return isTabVisible(id);
}

std::vector<ControlCenterPanel::TabCatalogEntry> ControlCenterPanel::hideableTabCatalog() {
  std::vector<TabCatalogEntry> out;
  out.reserve(std::size(kBuiltinTabs) - 1);
  for (const auto& spec : kBuiltinTabs) {
    if (!spec.hideable) {
      continue;
    }
    out.push_back({.key = spec.id, .title = builtinTitle(spec)});
  }
  auto& registry = scripting::PluginRegistry::instance();
  registry.ensureScanned();
  for (const auto& resolved : registry.entriesOfKind(scripting::PluginEntryKind::ControlCenterTab)) {
    if (resolved.manifest == nullptr || resolved.entry == nullptr) {
      continue;
    }
    const auto chrome = pluginTabChrome(resolved);
    out.push_back({.key = resolved.fullId(), .title = chrome.title});
  }
  return out;
}

std::string ControlCenterPanel::firstVisibleTab() const {
  for (const auto& slot : m_slots) {
    if (isTabVisible(slot.id)) {
      return slot.id;
    }
  }
  return "home";
}

bool ControlCenterPanel::groupHasShownTab(TabGroup group) const {
  for (const auto& slot : m_slots) {
    if (slot.group == group && isTabShown(slot.id)) {
      return true;
    }
  }
  return false;
}

void ControlCenterPanel::addSidebarGroupHeader(RovingListNavHost& nav, TabGroup group, float scale) {
  Node** out = nullptr;
  switch (group) {
  case TabGroup::Look:
    out = &m_lookHeader;
    break;
  case TabGroup::Status:
    out = &m_statusHeader;
    break;
  case TabGroup::Setup:
    out = &m_setupHeader;
    break;
  case TabGroup::Home:
    return;
  }

  if (m_compact) {
    auto sep = ui::separator({.spacing = Style::spaceXs * scale});
    *out = sep.get();
    nav.addChild(std::move(sep));
    return;
  }

  auto header = ui::column({
      .paddingV = Style::spaceXs * scale,
      .paddingH = Style::spaceXs * scale,
      .fillWidth = true,
  });
  header->addChild(
      ui::label({
          .text = groupCaption(group),
          .fontSize = Style::fontSizeCaption * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
      })
  );
  *out = header.get();
  nav.addChild(std::move(header));
}

void ControlCenterPanel::addSidebarTabButton(RovingListNavHost& nav, TabSlot& slot, float scale) {
  const std::string id = slot.id;
  const auto onClick = [this, id]() {
    selectTab(id, true);
    PanelManager::instance().refresh();
  };
  nav.addChild(
      ui::button({
          .out = &slot.button,
          .text = m_compact ? std::optional<std::string>{} : std::optional<std::string>{slot.title},
          .glyph = slot.glyph,
          .glyphSize = 21.0F * scale,
          .contentAlign = m_compact ? ButtonContentAlign::Center : ButtonContentAlign::Start,
          .variant = ButtonVariant::Tab,
          .tooltip = m_compact ? slot.title : std::string{},
          .minWidth = m_compact ? std::optional<float>{Style::controlHeightSm * scale} : std::optional<float>{},
          .minHeight = Style::controlHeightSm * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = (m_compact ? Style::spaceXs : Style::spaceSm) * scale,
          .gap = Style::spaceSm * scale,
          .radius = Style::scaledRadiusLg(scale),
          .onClick = onClick,
          .configure = [this, scale](Button& button) {
            if (button.label() != nullptr) {
              button.label()->setFontWeight(FontWeight::Bold);
              button.label()->setFontSize(Style::fontSizeBody * scale);
            }
            wireSidebarScroll(button.inputArea());
          },
      })
  );
  nav.registerItem(slot.button, onClick);
}

void ControlCenterPanel::syncTabVisibility() {
  for (auto& slot : m_slots) {
    const bool visible = isTabShown(slot.id);
    if (slot.button != nullptr) {
      slot.button->setVisible(visible);
    }
    if (!visible) {
      if (slot.container != nullptr) {
        slot.container->setVisible(false);
      }
      if (slot.headerActions != nullptr) {
        slot.headerActions->setVisible(false);
      }
    }
  }

  const auto setHeaderVisible = [](Node* node, bool visible) {
    if (node == nullptr) {
      return;
    }
    node->setVisible(visible);
    node->setParticipatesInLayout(visible);
  };
  setHeaderVisible(m_lookHeader, groupHasShownTab(TabGroup::Look));
  setHeaderVisible(m_statusHeader, groupHasShownTab(TabGroup::Status));
  setHeaderVisible(m_setupHeader, groupHasShownTab(TabGroup::Setup));
}

void ControlCenterPanel::updateTabChrome(std::string_view id) {
  for (auto& slot : m_slots) {
    const bool tabEnabled = isTabShown(slot.id);
    const bool active = tabEnabled && slot.id == id;
    if (slot.tab != nullptr) {
      slot.tab->setActive(active);
      if (!active) {
        slot.tab->setConfigureMode(false);
      }
    }
    if (slot.button != nullptr) {
      slot.button->setVisible(tabEnabled);
      slot.button->setVariant(active ? ButtonVariant::TabActive : ButtonVariant::Tab);
    }
    if (active && m_contentTitle != nullptr) {
      m_contentTitle->setText(slot.title);
    }
    if (slot.headerActions != nullptr) {
      slot.headerActions->setVisible(active);
    }
  }

  if (m_contentTitle != nullptr) {
    m_contentTitle->setVisible(true);
  }
  if (m_contentHeaderActions != nullptr) {
    m_contentHeaderActions->setVisible(true);
  }
  if (m_sidebarNav != nullptr) {
    m_sidebarNav->notifyExternalSelectionChanged();
  }
}

void ControlCenterPanel::applyTabContainerVisibility(std::string_view activeId) {
  for (auto& slot : m_slots) {
    if (slot.container != nullptr) {
      slot.container->setVisible(isTabShown(slot.id) && slot.id == activeId);
    }
  }
}

void ControlCenterPanel::layoutTabContainers(float bodyWidth, float bodyHeight) {
  const float travel = bodyHeight > 0.0F ? bodyHeight : 0.0F;
  const float contentHeight = tabContentHeight(bodyHeight);
  for (auto& slot : m_slots) {
    auto* container = slot.container;
    if (container == nullptr || !container->visible()) {
      continue;
    }

    container->setSize(bodyWidth, contentHeight);

    float offsetY = 0.0F;
    float opacity = 1.0F;
    if (m_tabTransitionActive && travel > 0.0F) {
      const auto direction = static_cast<float>(m_tabTransitionDirection);
      if (slot.id == m_tabTransitionOutgoingId) {
        offsetY = -direction * travel * m_tabTransitionProgress;
        opacity = 1.0F - 0.3F * m_tabTransitionProgress;
      } else if (slot.id == m_activeTabId) {
        offsetY = direction * travel * (1.0F - m_tabTransitionProgress);
        opacity = 0.7F + 0.3F * m_tabTransitionProgress;
      }
    }

    container->setPosition(0.0F, offsetY);
    container->setOpacity(opacity);
    if (m_tabTransitionActive) {
      container->setZIndex(slot.id == m_activeTabId ? 1 : 0);
    } else {
      container->setZIndex(0);
    }
  }
}

void ControlCenterPanel::resetTabContainerTransforms() {
  for (auto& slot : m_slots) {
    if (slot.container == nullptr) {
      continue;
    }
    slot.container->setPosition(0.0F, 0.0F);
    slot.container->setOpacity(1.0F);
    slot.container->setZIndex(0);
  }
}

int ControlCenterPanel::visibleTabOrdinal(std::string_view id) const {
  int ordinal = 0;
  for (const auto& slot : m_slots) {
    if (!isTabShown(slot.id)) {
      continue;
    }
    if (slot.id == id) {
      return ordinal;
    }
    ++ordinal;
  }
  return 0;
}

void ControlCenterPanel::applyTabTransitionLayout() {
  if (m_tabBodies == nullptr) {
    return;
  }
  layoutTabContainers(m_tabBodies->width(), m_tabBodies->height());
}

void ControlCenterPanel::startTabTransition(std::string_view from, std::string_view to) {
  if (m_animations == nullptr || m_tabBodies == nullptr) {
    applyTabContainerVisibility(to);
    resetTabContainerTransforms();
    return;
  }

  m_tabTransitionActive = true;
  m_tabTransitionOutgoingId = std::string(from);
  m_tabTransitionProgress = 0.0F;

  const int fromOrdinal = visibleTabOrdinal(from);
  const int toOrdinal = visibleTabOrdinal(to);
  m_tabTransitionDirection = toOrdinal >= fromOrdinal ? 1 : -1;

  for (auto& slot : m_slots) {
    if (slot.container == nullptr) {
      continue;
    }
    slot.container->setVisible(slot.id == from || slot.id == to);
  }

  applyTabTransitionLayout();
  PanelManager::instance().requestLayout();
  PanelManager::instance().requestRedraw();
  PanelManager::instance().requestFrameTick();

  m_tabTransitionAnimId = m_animations->animate(
      0.0F, 1.0F, static_cast<float>(Style::animNormal), Easing::EaseOutCubic,
      [this](float progress) {
        m_tabTransitionProgress = progress;
        applyTabTransitionLayout();
        PanelManager::instance().requestRedraw();
      },
      [this]() {
        m_tabTransitionAnimId = 0;
        finishTabTransition();
        PanelManager::instance().requestLayout();
        PanelManager::instance().requestRedraw();
      },
      m_tabBodies
  );
}

void ControlCenterPanel::finishTabTransition() {
  m_tabTransitionActive = false;
  resetTabContainerTransforms();
  applyTabContainerVisibility(m_activeTabId);
}

void ControlCenterPanel::wireSidebarScroll(InputArea* area) {
  if (area == nullptr) {
    return;
  }
  area->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return false;
    }
    if (m_sidebarScrollView != nullptr && m_sidebarScrollView->scrollable()) {
      const float delta = data.scrollDelta(Style::scrollWheelStep);
      if (data.axisSource == WL_POINTER_AXIS_SOURCE_FINGER) {
        m_sidebarScrollView->setScrollOffset(m_sidebarScrollView->scrollOffset() + delta);
      } else {
        m_sidebarScrollView->scrollBy(delta);
      }
      PanelManager::instance().requestRedraw();
      return true;
    }
    const float steps = data.scrollSteps();
    if (steps == 0.0F) {
      return false;
    }
    selectAdjacentVisibleTab(steps > 0.0F ? 1 : -1);
    return true;
  });
}

void ControlCenterPanel::selectAdjacentVisibleTab(int direction) {
  if (direction == 0) {
    return;
  }

  std::vector<std::string> visible;
  for (const auto& slot : m_slots) {
    if (isTabShown(slot.id)) {
      visible.push_back(slot.id);
    }
  }
  if (visible.empty()) {
    return;
  }

  int activeOrdinal = 0;
  for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
    if (visible[static_cast<std::size_t>(i)] == m_activeTabId) {
      activeOrdinal = i;
      break;
    }
  }

  const int targetOrdinal = activeOrdinal + direction;
  if (targetOrdinal < 0 || targetOrdinal >= static_cast<int>(visible.size())) {
    return;
  }
  selectTab(visible[static_cast<std::size_t>(targetOrdinal)], true);
  PanelManager::instance().refresh();
}

void ControlCenterPanel::selectTab(std::string_view id, bool animated) {
  std::string target(id);
  if (slotById(target) == nullptr || !isTabFeatureAvailable(target)) {
    target = firstVisibleTab();
  }
  m_activeTabForced = !isTabVisible(target);

  const std::string previous = m_activeTabId;
  const bool tabChanged = target != previous;

  if (m_tabTransitionAnimId != 0 && m_animations != nullptr) {
    m_animations->cancel(m_tabTransitionAnimId);
    m_tabTransitionAnimId = 0;
    finishTabTransition();
  }

  m_activeTabId = target;
  if (target == "notifications" && m_notificationManager != nullptr) {
    m_notificationManager->markNotificationHistorySeen();
  }

  updateTabChrome(target);

  if (tabChanged && animated && m_animations != nullptr && m_tabBodies != nullptr) {
    startTabTransition(previous, target);
  } else {
    m_tabTransitionActive = false;
    applyTabContainerVisibility(target);
    resetTabContainerTransforms();
  }

  scheduleMprisRefreshFor(target);
  syncActiveTabSearch();
}

void ControlCenterPanel::scheduleMprisRefreshFor(std::string_view id) {
  if (m_mpris == nullptr || m_mprisRefreshScheduled || (id != "home" && id != "media")) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (m_lastMprisRefreshAt.time_since_epoch().count() != 0 && now - m_lastMprisRefreshAt < kMprisRefreshMinInterval) {
    return;
  }

  m_lastMprisRefreshAt = now;
  m_mprisRefreshScheduled = true;
  DeferredCall::callLater([this]() {
    m_mprisRefreshScheduled = false;
    if (m_mpris == nullptr || !PanelManager::instance().isOpenPanel("control-center")) {
      return;
    }
    m_mpris->refreshPlayers();
    PanelManager::instance().requestUpdateOnly();
    PanelManager::instance().requestRedraw();
  });
}

bool ControlCenterPanel::isDirectSectionOpenContext(std::string_view context) const {
  if (context.empty() || context == "home") {
    return false;
  }
  if (context == "settings" || context.find(':') != std::string_view::npos) {
    return true;
  }
  for (const auto& spec : kBuiltinTabs) {
    if (spec.hideable && context == spec.id) {
      return true;
    }
  }
  return false;
}

ControlCenterSidebarMode ControlCenterPanel::sidebarModeForOpen(std::string_view context) const {
  if (m_config == nullptr) {
    return ControlCenterSidebarMode::Compact;
  }
  const auto& cc = m_config->config().controlCenter;
  return isDirectSectionOpenContext(context) ? cc.sidebarSectionMode : cc.sidebarMode;
}

std::unique_ptr<Input> ControlCenterPanel::createSettingsSearchInput(float scale) {
  return ui::input({
      .out = &m_searchInput,
      .value = m_searchQuery,
      .placeholder = i18n::tr("control-center.settings.search-placeholder"),
      .fontSize = Style::fontSizeBody * scale,
      .controlHeight = Style::controlHeight * scale,
      .horizontalPadding = Style::spaceSm * scale,
      .clearButtonEnabled = true,
      .flexGrow = m_showSidebar ? 0.0F : 1.0F,
      .onChange = [this](const std::string& value) { applySettingsSearch(value); },
  });
}

void ControlCenterPanel::applySettingsSearch(std::string value) {
  if (m_searchQuery == value) {
    return;
  }
  m_searchQuery = std::move(value);
  const bool searching = !m_searchQuery.empty();
  if (searching) {
    auto* slot = slotById(m_activeTabId);
    const bool stayOnTab = isSettingsEditorTab(m_activeTabId)
        || (isLiveSettingsHost(m_activeTabId)
            && slot != nullptr
            && slot->tab != nullptr
            && slot->tab->configureMode());
    if (!stayOnTab) {
      selectTab("appearance", false);
    } else if (isLiveSettingsHost(m_activeTabId) && slot != nullptr && slot->tab != nullptr) {
      slot->tab->setConfigureMode(true);
    }
  }
  syncActiveTabSearch();
  PanelManager::instance().requestLayout();
}

void ControlCenterPanel::syncActiveTabSearch() {
  auto* slot = slotById(m_activeTabId);
  if (slot != nullptr && slot->tab != nullptr) {
    slot->tab->setSettingsSearchQuery(m_searchQuery);
  }
}

void ControlCenterPanel::layoutFullSidebarWidth(Renderer& renderer) {
  if (m_sidebarScrollView == nullptr || m_sidebarNav == nullptr) {
    return;
  }

  const float scale = contentScale();
  const float fontSize = Style::fontSizeBody * scale;
  const float paddingH = Style::spaceSm * scale * 2.0F;
  const float gap = Style::spaceSm * scale;
  const float glyphW = 21.0F * scale;

  float maxTabWidth = Style::controlHeightSm * scale;
  if (!m_compact) {
    for (const auto& slot : m_slots) {
      if (!isTabVisible(slot.id)) {
        continue;
      }
      const TextMetrics text = renderer.measureText(slot.title, fontSize, FontWeight::Bold);
      maxTabWidth = std::max(maxTabWidth, paddingH + glyphW + gap + text.width);
    }
  }
  if (m_searchInput != nullptr) {
    maxTabWidth = std::max(maxTabWidth, 168.0F * scale);
  }

  const float contentWidth = std::ceil(maxTabWidth);

  float targetWidth = contentWidth;
  const float scrollHeight = m_sidebarScrollView->height();
  if (scrollHeight > 0.0F) {
    LayoutConstraints navConstraints;
    navConstraints.setExactWidth(contentWidth);
    const float navHeight = m_sidebarNav->measure(renderer, navConstraints).height;
    if (navHeight > scrollHeight + 0.5F) {
      targetWidth = contentWidth + m_sidebarScrollView->scrollbarGutter();
    }
  }

  if (m_searchInput != nullptr) {
    const float searchHeight = Style::controlHeight * scale;
    if (std::abs(m_searchInput->width() - targetWidth) > 0.5F
        || std::abs(m_searchInput->height() - searchHeight) > 0.5F) {
      m_searchInput->setSize(targetWidth, searchHeight);
    }
  }

  if (std::abs(m_sidebarScrollView->width() - targetWidth) > 0.5F) {
    m_sidebarScrollView->setSize(targetWidth, m_sidebarScrollView->height());
  }
}

void ControlCenterPanel::scrollSidebarNodeIntoView(const Node* node) {
  if (node == nullptr || m_sidebarScrollView == nullptr) {
    return;
  }
  scrollNodeIntoScrollView(*m_sidebarScrollView, &m_sidebarScrollState, *node, Style::spaceXs * contentScale());
  PanelManager::instance().requestLayout();
}

void ControlCenterPanel::scrollFocusedInputIntoView(InputArea* area) {
  if (area == nullptr) {
    return;
  }

  if (m_sidebarScrollView != nullptr && m_sidebarScrollView->content() != nullptr) {
    for (const Node* node = area; node != nullptr; node = node->parent()) {
      if (node == m_sidebarScrollView->content()) {
        scrollNodeIntoScrollView(*m_sidebarScrollView, &m_sidebarScrollState, *area, Style::spaceXs * contentScale());
        PanelManager::instance().requestLayout();
        return;
      }
    }
  }

  if (ScrollView* scrollView = findEnclosingScrollView(area)) {
    scrollNodeIntoScrollView(*scrollView, nullptr, *area, Style::spaceMd * contentScale());
    PanelManager::instance().requestLayout();
  }
}

void ControlCenterPanel::rebuildPluginSlots() {
  const std::string previouslyActive = m_activeTabId;
  std::erase_if(m_slots, [](const TabSlot& slot) { return slot.plugin; });
  if (m_scriptApi == nullptr) {
    if (slotById(previouslyActive) == nullptr) {
      m_activeTabId = firstVisibleTab();
    }
    return;
  }

  auto& registry = scripting::PluginRegistry::instance();
  registry.ensureScanned();
  const auto& pluginSettings =
      m_config != nullptr ? m_config->config().plugins.pluginSettings
                          : std::unordered_map<std::string, std::unordered_map<std::string, WidgetSettingValue>>{};
  static const std::unordered_map<std::string, WidgetSettingValue> kNoOverrides;

  std::vector<TabSlot> pluginSlots;
  for (const auto& resolved : registry.entriesOfKind(scripting::PluginEntryKind::ControlCenterTab)) {
    if (resolved.entry == nullptr || resolved.manifest == nullptr) {
      continue;
    }
    const auto psIt = pluginSettings.find(resolved.manifest->id);
    const auto& overrides = psIt != pluginSettings.end() ? psIt->second : kNoOverrides;
    auto seeded = scripting::seedEntrySettings(*resolved.entry, overrides);
    scripting::mergePluginSettings(*resolved.manifest, overrides, seeded);

    const auto chrome = pluginTabChrome(resolved);

    TabSlot slot;
    slot.id = resolved.fullId();
    slot.title = chrome.title;
    slot.glyph = chrome.glyph;
    slot.hideable = true;
    slot.plugin = true;
    slot.group = TabGroup::Status;
    slot.tab = std::make_unique<PluginControlCenterTab>(scripting::PluginRuntimeContext{
        .entryId = slot.id,
        .sourcePath = resolved.sourcePath,
        .pluginDir = resolved.pluginDir,
        .settings = std::move(seeded),
        .scriptApi = *m_scriptApi,
        .fileWatcher = m_fileWatcher,
        .httpClient = m_httpClient,
        .clipboard = m_clipboard,
        .platform = m_platform,
    });
    pluginSlots.push_back(std::move(slot));
  }

  auto insertAt = std::ranges::find_if(m_slots, [](const TabSlot& slot) { return slot.group == TabGroup::Setup; });
  m_slots.insert(
      insertAt, std::make_move_iterator(pluginSlots.begin()), std::make_move_iterator(pluginSlots.end())
  );

  if (slotById(previouslyActive) == nullptr) {
    m_activeTabId = firstVisibleTab();
  }
}

ControlCenterPanel::TabSlot* ControlCenterPanel::slotById(std::string_view id) {
  return const_cast<TabSlot*>(std::as_const(*this).slotById(id));
}

const ControlCenterPanel::TabSlot* ControlCenterPanel::slotById(std::string_view id) const {
  for (const auto& slot : m_slots) {
    if (slot.id == id) {
      return &slot;
    }
  }
  return nullptr;
}
