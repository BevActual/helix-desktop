#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "shell/settings/template_store_content.h"
#include "ui/signal.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

class Button;
class ConfigService;
class Flex;
class Label;
class Renderer;
class Segmented;
class Toggle;
struct ControlCenterServices;
struct DesktopEntry;

namespace noctalia::theme {
  class TemplateApplyService;
}

class TemplatesTab : public Tab {
public:
  enum class Source : std::uint8_t { Builtin, Community, User };

  explicit TemplatesTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void syncLiveControls();
  void applySource(std::size_t index);
  void applyEnable(bool enabled);
  void invalidateStore(bool resetContent);
  void ensureStore();
  void populateStoreIfNeeded(Renderer& renderer);
  void openAppPicker();
  void createFromApp(const DesktopEntry& app);
  void enableCatalogTemplate(Source source, std::string id);
  void createUserTemplate(const DesktopEntry& app);
  void applyNow();
  void openTemplatesFolder();
  void persistSource();
  void syncGtkFollow();
  void enableGtkTemplates();
  void applyGreeterAutoSync(bool enabled);
  void syncGreeterCard();
  [[nodiscard]] bool sourceEnabled() const;
  [[nodiscard]] std::string catalogKey() const;
  [[nodiscard]] static Source sourceFromIndex(std::size_t index) noexcept;
  [[nodiscard]] static std::size_t indexFromSource(Source source) noexcept;

  ConfigService* m_config = nullptr;
  std::function<void()> m_applyTemplates;
  std::function<void()> m_syncGreeter;
  noctalia::theme::TemplateApplyService* m_templateApply = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;
  std::unique_ptr<settings::TemplateStoreContent> m_store;

  Flex* m_rootLayout = nullptr;
  Segmented* m_source = nullptr;
  Label* m_enableLabel = nullptr;
  Toggle* m_enable = nullptr;
  Label* m_hint = nullptr;
  Flex* m_storeHost = nullptr;
  Button* m_applyButton = nullptr;
  Button* m_folderButton = nullptr;
  Button* m_newButton = nullptr;
  Label* m_gtkFollowHint = nullptr;
  Button* m_gtkFollowButton = nullptr;
  Flex* m_greeterCard = nullptr;
  Toggle* m_greeterAutoSync = nullptr;
  Button* m_greeterSyncButton = nullptr;

  Source m_sourceKind = Source::Builtin;
  bool m_syncing = false;
  bool m_liveDirty = false;
  bool m_storePopulated = false;
  bool m_liveEnabled = false;
  Source m_liveSource = Source::Community;
  float m_storeScale = 0.0F;
  std::string m_catalogKey;
  std::unordered_map<std::string, std::string> m_applyFailures;
};
