#pragma once

#include "shell/control_center/control_center_services.h"
#include "shell/settings/settings_content.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

class Flex;
class Renderer;

// Lightweight compatibility link. No legacy settings controls or editors are rendered.
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
  [[nodiscard]] std::unique_ptr<Flex> createInline(float scale, float panelCardOpacity);
  [[nodiscard]] std::unique_ptr<Flex> wrapLive(std::unique_ptr<Flex> live, float scale, float panelCardOpacity);
  [[nodiscard]] std::unique_ptr<Flex> embedHeader(std::unique_ptr<Flex> liveActions, float scale);

  void onClose();
  void layout(Renderer& renderer, float contentWidth, float bodyHeight);
  [[nodiscard]] bool layoutEmbedded(Renderer& renderer, float contentWidth, float bodyHeight);

  void setConfigureMode(bool show);
  [[nodiscard]] bool configureMode() const noexcept { return false; }
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
  void openNative() const;
  std::string m_selectedSection;
  Flex* m_root = nullptr;
};
