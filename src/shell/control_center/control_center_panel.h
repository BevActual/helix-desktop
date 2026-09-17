#pragma once

#include "render/animation/animation_manager.h"
#include "shell/control_center/control_center_services.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel.h"
#include "shell/settings/settings_hosted_editors.h"
#include "shell/settings/settings_modal_host.h"
#include "ui/controls/scroll_view.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class AsyncTextureCache;
class Button;
class CompositorPlatform;
class ConfigService;
class DependencyService;
class Flex;
class HttpClient;
class Input;
class InputArea;
class Label;
class Box;
class MprisService;
class Node;
class RovingListNavHost;
class NotificationManager;
class ClipboardService;
class FileWatcher;

namespace scripting {
  class ScriptApiContext;
  class PluginManager;
}

class ControlCenterPanel : public Panel {
public:
  explicit ControlCenterPanel(const ControlCenterServices& services);

  // Tabs the user may hide from the Settings GUI (every tab except Home).
  // Keys match the [control_center] hidden_tabs entries: built-in ids like
  // "monitor" and plugin entry ids like "author/plugin:displays".
  struct TabCatalogEntry {
    std::string key;
    std::string title;
  };
  [[nodiscard]] static std::vector<TabCatalogEntry> hideableTabCatalog();

  void create() override;
  void onFrameTick(float deltaMs) override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  [[nodiscard]] bool dismissTransientUi() override;
  [[nodiscard]] bool isContextActive(std::string_view context) const override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;
  [[nodiscard]] bool deferExternalRefresh() const override;
  [[nodiscard]] bool deferPointerRelayout() const override;
  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override { return scaled(520.0F); }
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;
  [[nodiscard]] float cornerRadius() const noexcept override;
  [[nodiscard]] bool showsSidebar() const noexcept { return m_showSidebar; }

  void queuePluginSettings(std::string pluginId);
  void queueBarWidget(std::string barName, std::string widgetName);
  void invalidatePluginSource(const std::string& sourceName);

  enum class TabGroup : std::uint8_t { Home, Appearance, Layout, Status, Services, Behavior, Advanced };

private:
  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;

  struct TabSlot {
    std::string id;
    std::string title;
    std::string glyph;
    std::unique_ptr<Tab> tab;
    Button* button = nullptr;
    Flex* container = nullptr;
    Flex* headerActions = nullptr;
    TabGroup group = TabGroup::Status;
    bool hideable = true;
    bool plugin = false;
  };

  void appendBuiltinSlots(const ControlCenterServices& services);
  void rebuildPluginSlots();
  void ensureTabUi(TabSlot& slot);
  void selectTab(std::string_view id, bool animated = false);
  void selectAdjacentVisibleTab(int direction);
  [[nodiscard]] TabSlot* slotById(std::string_view id);
  [[nodiscard]] const TabSlot* slotById(std::string_view id) const;
  void wireSidebarScroll(InputArea* area);
  void scrollFocusedInputIntoView(InputArea* area) override;
  void scrollSidebarNodeIntoView(const Node* node);
  void scheduleMprisRefreshFor(std::string_view id);
  void updateTabChrome(std::string_view id);
  void applyTabContainerVisibility(std::string_view activeId);
  void layoutTabContainers(float bodyWidth, float bodyHeight);
  void layoutFullSidebarWidth(Renderer& renderer);
  void resetTabContainerTransforms();
  void startTabTransition(std::string_view from, std::string_view to);
  void finishTabTransition();
  void applyTabTransitionLayout();
  [[nodiscard]] int visibleTabOrdinal(std::string_view id) const;
  void syncTabVisibility();
  void addSidebarTabButton(class RovingListNavHost& nav, TabSlot& slot, float scale);
  void addSidebarGroupHeader(class RovingListNavHost& nav, TabGroup group, float scale);
  void syncPackagesBadge();
  void applySettingsSearch(std::string value);
  void syncActiveTabSearch();
  [[nodiscard]] std::unique_ptr<Input> createSettingsSearchInput(float scale);
  [[nodiscard]] bool isTabSearchActive() const;
  [[nodiscard]] std::string bestTabSearchId() const;
  [[nodiscard]] bool groupHasShownTab(TabGroup group) const;
  [[nodiscard]] bool isTabFeatureAvailable(std::string_view id) const;
  [[nodiscard]] bool isTabVisible(std::string_view id) const;
  [[nodiscard]] bool isTabShown(std::string_view id) const;
  [[nodiscard]] std::string firstVisibleTab() const;
  [[nodiscard]] bool isDirectSectionOpenContext(std::string_view context) const;
  [[nodiscard]] ControlCenterSidebarMode sidebarModeForOpen(std::string_view context) const;

  std::vector<TabSlot> m_slots;
  std::string m_activeTabId = "home";
  bool m_activeTabForced = false;
  scripting::ScriptApiContext* m_scriptApi = nullptr;
  FileWatcher* m_fileWatcher = nullptr;
  HttpClient* m_httpClient = nullptr;
  ClipboardService* m_clipboard = nullptr;
  CompositorPlatform* m_platform = nullptr;

  Flex* m_rootLayout = nullptr;
  Flex* m_sidebar = nullptr;
  ScrollView* m_sidebarScrollView = nullptr;
  ScrollViewState m_sidebarScrollState{};
  RovingListNavHost* m_sidebarNav = nullptr;
  InputArea* m_sidebarScrollArea = nullptr;
  Node* m_appearanceHeader = nullptr;
  Node* m_layoutHeader = nullptr;
  Node* m_statusHeader = nullptr;
  Node* m_servicesHeader = nullptr;
  Node* m_behaviorHeader = nullptr;
  Node* m_advancedHeader = nullptr;
  Input* m_searchInput = nullptr;
  std::string m_searchQuery;
  Flex* m_content = nullptr;
  InputArea* m_contentDismissArea = nullptr;
  Flex* m_contentHeader = nullptr;
  Flex* m_contentHeaderActions = nullptr;
  Label* m_contentTitle = nullptr;
  Button* m_closeButton = nullptr;
  Flex* m_tabBodies = nullptr;
  Box* m_sidebarEdgeFade = nullptr;
  Box* m_contentEdgeFade = nullptr;
  ConfigService* m_config = nullptr;
  MprisService* m_mpris = nullptr;
  NotificationManager* m_notificationManager = nullptr;
  DependencyService* m_dependencies = nullptr;
  bool m_compact = false;
  bool m_showSidebar = true;
  bool m_mprisRefreshScheduled = false;
  std::chrono::steady_clock::time_point m_lastMprisRefreshAt;
  AnimationManager::Id m_tabTransitionAnimId = 0;
  std::string m_tabTransitionOutgoingId;
  float m_tabTransitionProgress = 1.0F;
  int m_tabTransitionDirection = 1;
  bool m_tabTransitionActive = false;
  bool m_firstOpenAfterCreate = false;
  bool m_modalAttached = false;
  settings::SettingsModalHost m_modalHost;
  std::unique_ptr<settings::HostedEditors> m_hostedEditors;
  scripting::PluginManager* m_pluginManager = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;

  void attachModalHost(Renderer& renderer, float width, float height);
  void maybeOpenPendingEditor();

  std::string m_pendingOpenPluginSettingsId;
  std::string m_pendingOpenBarName;
  std::string m_pendingOpenWidgetInspectorName;
};
