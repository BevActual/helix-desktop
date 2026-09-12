#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"
#include "scripting/plugin_ipc.h"
#include "scripting/plugin_script_watcher.h"
#include "scripting/script_runtime.h"
#include "shell/control_center/tab.h"
#include "ui/ui_tree.h"
#include "ui/ui_tree_reconciler.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

class ClipboardService;
class CompositorPlatform;
class FileWatcher;
class Flex;
class HttpClient;
namespace scripting {
  struct PluginRuntimeContext;
  class ScriptApiContext;
} // namespace scripting

// A Control Center sidebar tab backed by a plugin's `[[control_center_tab]]`
// entry. The script describes the tab body with `controlCenterTab.render(ui.*)`
// the same way a panel or desktop widget does; the host owns the CC chrome.
class PluginControlCenterTab : public Tab, public scripting::PluginIpcEndpoint {
public:
  explicit PluginControlCenterTab(scripting::PluginRuntimeContext context);
  ~PluginControlCenterTab() override;

  [[nodiscard]] std::string_view entryId() const noexcept { return m_entryId; }

  std::unique_ptr<Flex> create() override;
  void setActive(bool active) override;
  void onClose() override;
  void onFrameTick(float deltaMs) override;

  // PluginIpcEndpoint
  [[nodiscard]] std::string_view ipcEntryId() const override { return m_entryId; }
  [[nodiscard]] std::string_view ipcOutputName() const override { return {}; }
  [[nodiscard]] std::string_view ipcBarName() const override { return {}; }
  [[nodiscard]] DispatchResult
  dispatchIpc(std::string_view event, std::string_view payload, const scripting::ScriptSnapshot& snapshot) override;

protected:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;

private:
  void startScript();
  void handleScriptResult(scripting::ScriptResult result);
  [[nodiscard]] scripting::ScriptSnapshot makeScriptSnapshot() const;
  [[nodiscard]] std::string resolvePluginPath(const std::string& path) const;
  [[nodiscard]] std::string focusedOutputName() const;
  void startUpdateTimer();
  void setupScriptWatch();
  void teardownScriptWatch();
  void reloadScript();
  void requestHostRefresh();

  std::string m_entryId;
  std::filesystem::path m_sourcePath;
  std::filesystem::path m_pluginDir;
  scripting::ScriptApiContext& m_scriptApi;
  std::unordered_map<std::string, WidgetSettingValue> m_settings;
  std::shared_ptr<scripting::ScriptRuntime> m_runtime;
  scripting::ScriptRuntime::SubscriberId m_runtimeSubscription = 0;
  FileWatcher* m_fileWatcher = nullptr;
  HttpClient* m_httpClient = nullptr;
  ClipboardService* m_clipboard = nullptr;
  CompositorPlatform* m_platform = nullptr;
  scripting::PluginScriptWatcher m_scriptWatcher;
  Timer m_updateTimer;

  Flex* m_flex = nullptr;
  ui::UiTreeReconciler m_reconciler;
  std::optional<ui::UiTreeNode> m_tree;
  bool m_treeDirty = false;
  int m_updateIntervalMs = 1000;
  bool m_wantsSecondTicks = false;
  bool m_needsFrameTick = false;
  bool m_hasOnIpc = false;
  bool m_hasOnIpcKnown = false;
  bool m_active = false;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
