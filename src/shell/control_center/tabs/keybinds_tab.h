#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class Button;
class ClipboardService;
class ConfigService;
class Flex;
class Label;
class Renderer;
class Segmented;
struct ControlCenterServices;

class KeybindsTab : public Tab {
public:
  enum class Source : std::uint8_t { InNoctalia, Niri };

  explicit KeybindsTab(const ControlCenterServices& services);

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
  void syncLiveControls();
  void applySource(std::size_t index);
  void persistSource();
  void copyMissingNiriBinds();
  void installMissingNiriBinds();
  void openNiriConfigFolder();
  void rebuildLivePane();
  [[nodiscard]] bool niriAvailable() const;
  [[nodiscard]] static Source sourceFromIndex(std::size_t index) noexcept;
  [[nodiscard]] static std::size_t indexFromSource(Source source) noexcept;

  ConfigService* m_config = nullptr;
  ClipboardService* m_clipboard = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  std::unique_ptr<SettingsPane> m_liveSettings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Segmented* m_source = nullptr;
  Label* m_hint = nullptr;
  Flex* m_liveHost = nullptr;
  Button* m_copyMissingButton = nullptr;
  Button* m_installButton = nullptr;
  Button* m_folderButton = nullptr;

  Source m_sourceKind = Source::InNoctalia;
  bool m_syncing = false;
  bool m_liveDirty = false;
};
