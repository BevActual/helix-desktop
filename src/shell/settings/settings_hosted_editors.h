#pragma once

#include "config/config_types.h"
#include "scripting/plugin_file_cache.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_sheet_modal.h"
#include "ui/controls/scroll_view.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class AsyncTextureCache;
class ConfigService;
class Renderer;
class Flex;

namespace scripting {
  class PluginManager;
  class PluginFileCache;
  struct CatalogEntry;
}

namespace settings {

  class SettingsControlFactory;
  class SettingsModalHost;

  struct HostedEditorBindings {
    ConfigService* config = nullptr;
    scripting::PluginManager* pluginManager = nullptr;
    scripting::PluginFileCache* pluginFileCache = nullptr;
    AsyncTextureCache* asyncTextures = nullptr;
    std::function<float()> scale;
    std::function<Renderer*()> renderer;
    std::function<SettingsContentContext()> makeContext;
    std::function<void()> requestRebuild;
    std::function<void(std::vector<std::string>, ConfigOverrideValue)> setOverride;
    std::function<void(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>)> setOverrides;
    std::function<void()> dismissPopups;
    std::function<void(std::size_t, const SessionPanelActionConfig&)> syncSessionActionSummary;
    std::shared_ptr<std::vector<SessionPanelActionConfig>>* sessionActionsEditState = nullptr;
    std::function<void(
        std::string, std::string, std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>
    )>
        renameWidget;
    std::function<void(const std::string& id, bool enable, const scripting::CatalogEntry*)> onPluginToggled;
    std::string* editingWidgetName = nullptr;
    std::string* editingCapsuleGroupId = nullptr;
    std::string* renamingWidgetName = nullptr;
    std::string* pendingDeleteWidgetName = nullptr;
    std::string* pendingDeleteWidgetSettingPath = nullptr;
    std::vector<std::string>* selectedLaneWidgets = nullptr;
    std::string* pendingGestureKey = nullptr;
    std::string* pendingGestureVerb = nullptr;
    std::string* actionsExpandedFor = nullptr;
    std::unique_ptr<SettingsControlFactory>* editorFactory = nullptr;
    std::vector<std::string>* editorListPath = nullptr;
    bool* pluginStoreOpen = nullptr;
    ScrollViewState* pluginStoreScroll = nullptr;
  };

  class HostedEditors {
  public:
    HostedEditors();
    ~HostedEditors();

    void initialize(SettingsModalHost& host, std::function<void()> dismissSelect);
    void bind(HostedEditorBindings bindings);
    void attachSheet(SettingsSheetModal& sheet);
    void setLayoutRenderer(Renderer* renderer) { m_layoutRenderer = renderer; }

    [[nodiscard]] SettingsSheetModal* sheet() const noexcept { return m_sheet; }
    [[nodiscard]] bool isOpen() const noexcept;

    void openSessionActionEntry(std::size_t index);
    void openIdleBehaviorEntry(std::size_t index);
    void openIdleBehaviorCreate();
    void openNotificationFilterEntry(std::size_t index);
    void openNotificationFilterCreate();
    void openBarWidgetAdd(const std::vector<std::string>& lanePath);
    void openWidgetInspector(std::vector<std::string> lanePath, std::string widgetName);
    void openCapsuleGroup(std::vector<std::string> lanePath, std::string groupId);
    void openPluginStore();
    void close();

  private:
    void ensureSheet();
    void prepareOpen();
    [[nodiscard]] float uiScale() const;
    void openBarWidgetEditorSheet(
        std::string title, std::function<void(Flex&)> populate, std::function<void()> removeAction = nullptr
    );

    SettingsModalHost* m_host = nullptr;
    std::function<void()> m_dismissSelect;
    std::unique_ptr<SettingsSheetModal> m_ownedSheet;
    SettingsSheetModal* m_sheet = nullptr;
    HostedEditorBindings m_b;
    Renderer* m_layoutRenderer = nullptr;
    scripting::PluginFileCache m_fileCache;
    ScrollViewState m_pluginStoreScroll;
    std::unique_ptr<SettingsControlFactory> m_factory;
    std::vector<std::string> m_listPath;
  };

} // namespace settings
