#pragma once

#include "core/timer_manager.h"
#include "scripting/plugin_manager.h"
#include "shell/control_center/control_center_services.h"
#include "shell/settings/config_export_dialog_modal.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/settings_runtime.h"
#include "ui/controls/scroll_view.h"
#include "ui/signal.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class AsyncTextureCache;
class AccountsService;
class Button;
class ClipboardService;
class CompositorPlatform;
class ConfigService;
class DependencyService;
class Flex;
class IpcService;
class Label;
class Renderer;
class ScrollView;
class Select;
class Toggle;
class UPowerService;
class ContextMenuPopup;
class CalendarService;

namespace scripting {
  class PluginManager;
}

namespace settings {
  class HostedEditors;
  class SettingsModalHost;
}

struct SettingsPaneOptions {
  std::string section;
  std::unordered_set<std::string> includedGroups;
  bool compact = false;
  std::function<void()> openStore;
  bool showConfigureButton = true;
  // The tab shows its settings in the page (createInline), macOS-pane style, so the header
  // Settings button is hidden. Search and `settings:` contexts still open the full view.
  bool inlineSettings = false;
};

class SettingsPane {
public:
  SettingsPane(const ControlCenterServices& services, SettingsPaneOptions options);
  ~SettingsPane();

  [[nodiscard]] std::unique_ptr<Flex> create(float scale, float panelCardOpacity);
  [[nodiscard]] std::unique_ptr<Flex> createHeaderActions(float scale);
  // The pane's settings rows in a plain (non-scrolling) column, for tabs that show their
  // settings under the live content. Rebuilt with the full view.
  [[nodiscard]] std::unique_ptr<Flex> createInline(float scale, float panelCardOpacity);
  [[nodiscard]] std::unique_ptr<Flex> wrapLive(std::unique_ptr<Flex> live, float scale, float panelCardOpacity);
  [[nodiscard]] std::unique_ptr<Flex> embedHeader(std::unique_ptr<Flex> liveActions, float scale);

  void onClose();
  void layout(Renderer& renderer, float contentWidth, float bodyHeight);
  [[nodiscard]] bool layoutEmbedded(Renderer& renderer, float contentWidth, float bodyHeight);

  void setConfigureMode(bool show);
  [[nodiscard]] bool configureMode() const noexcept { return m_configureMode; }
  [[nodiscard]] const std::string& section() const noexcept { return m_selectedSection; }
  void setSection(std::string section);
  void setSearchQuery(std::string query);
  void openCalendarAccount(std::optional<std::string> accountId);
  void openSearchPicker(settings::SearchPickerOpenRequest request);
  void openUserTemplateEditor(std::string id);
  void setIncludedGroups(std::unordered_set<std::string> groups);
  void onExternalChanged();
  void openPluginSettings(std::string pluginId);
  void openWidgetInspector(std::string barName, std::string widgetName);
  void setSelectedBarName(std::string name);

private:
  void rebuildContent();
  void queueRebuild();
  void fillContent(Flex& content);
  void rebuildInto(Flex& content);
  [[nodiscard]] settings::RegistryEnvironment buildRegistryEnvironment() const;
  [[nodiscard]] std::vector<settings::SelectOption> batteryDeviceOptions() const;
  [[nodiscard]] std::vector<settings::GestureActionOption> gestureActionCatalog() const;
  [[nodiscard]] settings::SettingsContentContext makeContentContext(
      const Config& cfg, const BarConfig* selectedBar, const BarMonitorOverride* selectedMonitorOverride
  );
  void persistNiriBlurInclude();
  void persistNiriOverviewInclude();
  void persistNiriLayoutInclude();
  void persistNiriWindowRulesInclude();
  void setSettingOverride(std::vector<std::string> path, ConfigOverrideValue value);
  void setSettingOverrides(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides);
  void clearSettingOverride(std::vector<std::string> path);
  void clearSettingOverrides(std::vector<std::vector<std::string>> paths);
  void bindHostedEditors();
  void syncBarSelect();
  void applyConfigureVisibility();
  void ensureEmbeddedEditor();
  void refreshPluginListIfNeeded();
  void markPluginListDirty();
  void resetBarLane(std::vector<std::string> lanePath);
  void renameWidgetInstance(
      std::string oldName, std::string newName,
      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides
  );
  void createBar(std::string name);
  void renameBar(std::string oldName, std::string newName);
  void deleteBar(std::string name);
  void moveBar(std::string name, int direction);
  void createMonitorOverride(std::string barName, std::string match);
  void renameMonitorOverride(std::string barName, std::string oldMatch, std::string newMatch);
  void deleteMonitorOverride(std::string barName, std::string match);
  void openActionsMenu();
  void saveSupportReport();
  void saveConfigExport(settings::ConfigExportMode mode);
  void openConfigExportDialog();
  [[nodiscard]] settings::RuntimeActions makeRuntimeActions();
  [[nodiscard]] const BarMonitorOverride* selectedMonitorOverride(const BarConfig* selectedBar) const;

  ConfigService* m_config = nullptr;
  ClipboardService* m_clipboard = nullptr;
  CompositorPlatform* m_platform = nullptr;
  DependencyService* m_dependencies = nullptr;
  UPowerService* m_upower = nullptr;
  AccountsService* m_accounts = nullptr;
  IpcService* m_ipc = nullptr;
  settings::HostedEditors* m_hostedEditors = nullptr;
  scripting::PluginManager* m_pluginManager = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;
  CalendarService* m_calendar = nullptr;
  settings::SettingsModalHost* m_modalHost = nullptr;
  settings::RuntimeActions m_actions;
  std::function<void()> m_openStore;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  std::string m_selectedSection;
  std::unordered_set<std::string> m_includedGroups;
  bool m_compact = false;
  bool m_showConfigureButton = true;
  bool m_inlineSettings = false;
  std::string m_searchQuery;
  // Installed-plugins list filter (Plugins tab), debounced so typing does not rebuild per key.
  std::string m_pluginSearchQuery;
  Timer m_pluginSearchDebounceTimer;
  std::string m_selectedBarName;
  std::string m_selectedMonitorOverride;
  std::string m_statusMessage;
  bool m_statusIsError = false;
  bool m_showAdvanced = true;
  bool m_rebuildQueued = false;
  bool m_configureMode = false;
  bool m_pendingEncryptedStorageReset = false;
  float m_scale = 1.0F;
  float m_panelCardOpacity = 1.0F;

  std::string m_editingWidgetName;
  std::string m_editingCapsuleGroupId;
  std::vector<std::string> m_selectedLaneWidgets;
  std::string m_pendingDeleteWidgetName;
  std::string m_pendingDeleteWidgetSettingPath;
  std::string m_renamingWidgetName;
  std::string m_pendingGestureKey;
  std::string m_pendingGestureVerb;
  std::string m_actionsExpandedFor;
  std::unordered_map<std::string, std::unordered_set<std::string>> m_expandedGroupsByPage;
  std::vector<std::vector<std::string>> m_pendingResetSettingPaths;
  std::string m_creatingBarName;
  std::string m_renamingBarName;
  std::string m_pendingDeleteBarName;
  std::string m_creatingMonitorOverrideMatch;
  std::string m_renamingMonitorOverrideBarName;
  std::string m_renamingMonitorOverrideMatch;
  std::string m_pendingDeleteMonitorOverrideBarName;
  std::string m_pendingDeleteMonitorOverrideMatch;
  std::vector<scripting::PluginStatus> m_pluginList;
  bool m_pluginListDirty = true;
  bool m_pluginListRefreshInFlight = false;
  std::uint64_t m_pluginListRefreshGeneration = 0;
  std::string m_pendingDeletePluginId;

  Flex* m_embedHost = nullptr;
  Flex* m_liveRoot = nullptr;
  Flex* m_liveHeaderActions = nullptr;
  Flex* m_editorHeaderActions = nullptr;
  Button* m_configureButton = nullptr;
  Flex* m_rootLayout = nullptr;
  Select* m_barSelect = nullptr;
  Select* m_monitorOverrideSelect = nullptr;
  Flex* m_barSelectRow = nullptr;
  Label* m_statusLabel = nullptr;
  Flex* m_statusBanner = nullptr;
  ScrollView* m_scroll = nullptr;
  ScrollViewState m_scrollState{};
  Flex* m_content = nullptr;
  Flex* m_inlineContent = nullptr;
  Toggle* m_advancedToggle = nullptr;
  Button* m_actionsMenuButton = nullptr;
  Renderer* m_layoutRenderer = nullptr;
  std::unique_ptr<ContextMenuPopup> m_actionsMenuPopup;
  std::unique_ptr<settings::ConfigExportDialogModal> m_configExportDialog;
};
