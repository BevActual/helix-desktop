#pragma once

#include "shell/control_center/control_center_services.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_registry.h"
#include "ui/controls/scroll_view.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class AccountsService;
class Button;
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

struct SettingsPaneOptions {
  std::string section;
  std::unordered_set<std::string> includedGroups;
};

class SettingsPane {
public:
  SettingsPane(const ControlCenterServices& services, SettingsPaneOptions options);

  [[nodiscard]] std::unique_ptr<Flex> create(float scale, float panelCardOpacity);
  [[nodiscard]] std::unique_ptr<Flex> createHeaderActions(float scale);
  [[nodiscard]] std::unique_ptr<Flex> takeHost(std::unique_ptr<Flex> live, float scale, float panelCardOpacity);
  [[nodiscard]] std::unique_ptr<Flex> createEmbedHeaderActions(std::unique_ptr<Flex> liveActions, float scale);

  void onClose();
  void layout(Renderer& renderer, float contentWidth, float bodyHeight);
  [[nodiscard]] bool layoutEmbedded(Renderer& renderer, float contentWidth, float bodyHeight);

  void setConfigureMode(bool show);
  [[nodiscard]] bool configureMode() const noexcept { return m_configureMode; }
  [[nodiscard]] const std::string& section() const noexcept { return m_selectedSection; }

private:
  void rebuildContent();
  void queueRebuild();
  void fillContent(Flex& content);
  [[nodiscard]] settings::RegistryEnvironment buildRegistryEnvironment() const;
  [[nodiscard]] std::vector<settings::SelectOption> batteryDeviceOptions() const;
  [[nodiscard]] std::vector<settings::GestureActionOption> gestureActionCatalog() const;
  [[nodiscard]] settings::SettingsContentContext makeContentContext(
      const Config& cfg, const BarConfig* selectedBar, const BarMonitorOverride* selectedMonitorOverride
  );
  void setSettingOverride(std::vector<std::string> path, ConfigOverrideValue value);
  void setSettingOverrides(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides);
  void clearSettingOverride(std::vector<std::string> path);
  void clearSettingOverrides(std::vector<std::vector<std::string>> paths);
  void openFullSettings() const;
  void syncBarSelect();
  void applyConfigureVisibility();

  ConfigService* m_config = nullptr;
  CompositorPlatform* m_platform = nullptr;
  DependencyService* m_dependencies = nullptr;
  UPowerService* m_upower = nullptr;
  AccountsService* m_accounts = nullptr;
  IpcService* m_ipc = nullptr;

  std::string m_selectedSection;
  std::unordered_set<std::string> m_includedGroups;
  std::string m_selectedBarName;
  std::string m_statusMessage;
  bool m_statusIsError = false;
  bool m_showAdvanced = true;
  bool m_rebuildQueued = false;
  bool m_configureMode = false;
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

  Flex* m_embedHost = nullptr;
  Flex* m_liveRoot = nullptr;
  Flex* m_liveHeaderActions = nullptr;
  Flex* m_editorHeaderActions = nullptr;
  Button* m_configureButton = nullptr;
  Flex* m_rootLayout = nullptr;
  Select* m_barSelect = nullptr;
  Flex* m_barSelectRow = nullptr;
  Label* m_statusLabel = nullptr;
  Flex* m_statusBanner = nullptr;
  ScrollView* m_scroll = nullptr;
  ScrollViewState m_scrollState{};
  Flex* m_content = nullptr;
  Toggle* m_advancedToggle = nullptr;
  Button* m_openButton = nullptr;
};
