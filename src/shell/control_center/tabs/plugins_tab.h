#pragma once

#include "scripting/plugin_file_cache.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "shell/settings/plugin_store_content.h"
#include "ui/controls/scroll_view.h"
#include "ui/signal.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class AsyncTextureCache;
class Button;
class ConfigService;
class Flex;
class Label;
class Renderer;
class Segmented;
struct ControlCenterServices;

namespace scripting {
  class PluginManager;
}

class PluginsTab : public Tab {
public:
  enum class View : std::uint8_t { Installed, Store };

  explicit PluginsTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;
  bool dismissTransientUi() override;
  [[nodiscard]] bool handleGlobalKey(
      std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit
  ) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void syncLiveControls();
  void applyView(std::size_t index);
  void persistView();
  void rebuildInstalledPane();
  void invalidateStore(bool resetContent);
  void requestStoreCatalog(bool force = false);
  void refreshCurrentView();
  void ensureStore();
  void populateStoreIfNeeded(Renderer& renderer);
  [[nodiscard]] static View viewFromIndex(std::size_t index) noexcept;
  [[nodiscard]] static std::size_t indexFromView(View view) noexcept;

  ConfigService* m_config = nullptr;
  scripting::PluginManager* m_pluginManager = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  std::unique_ptr<SettingsPane> m_installedSettings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;
  std::unique_ptr<settings::PluginStoreContent> m_store;
  scripting::PluginFileCache m_fileCache;
  ScrollViewState m_storeScroll;
  std::vector<settings::StoreCatalogEntry> m_catalog;

  Flex* m_rootLayout = nullptr;
  Segmented* m_viewSwitch = nullptr;
  Label* m_status = nullptr;
  Flex* m_installedHost = nullptr;
  Flex* m_storeHost = nullptr;
  Flex* m_storeToolbarHost = nullptr;
  Button* m_refreshButton = nullptr;
  Button* m_backButton = nullptr;
  Button* m_pageButton = nullptr;
  Button* m_sourceButton = nullptr;

  View m_view = View::Installed;
  bool m_syncing = false;
  bool m_liveDirty = false;
  bool m_storePopulated = false;
  bool m_storeLoading = false;
  bool m_storeLoaded = false;
  float m_storeScale = 0.0F;
  std::uint64_t m_storeGeneration = 0;
  std::vector<std::string> m_catalogErrors;
};
