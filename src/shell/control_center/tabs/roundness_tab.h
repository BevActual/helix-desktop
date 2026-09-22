#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class ConfigService;
class Flex;
class Label;
class Renderer;
class Segmented;
class Slider;
struct ControlCenterServices;

class RoundnessTab : public Tab {
public:
  explicit RoundnessTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  [[nodiscard]] std::unique_ptr<Flex> createLiveBody(bool hosted = false);
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;

private:
  enum class Catalog : std::uint8_t { Shell, Bar, Dock, Windows };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void syncLiveControls();
  void commitRoundness();
  void commitBarRadius();
  void commitDockRadius();
  void commitWindowRadius();
  void applyCatalog(std::size_t index);
  void persistCatalog();
  void syncCatalog();
  [[nodiscard]] bool syncDetailLabel();
  [[nodiscard]] Catalog catalogFromState(std::string_view value) const noexcept;
  [[nodiscard]] static std::string_view catalogState(Catalog catalog) noexcept;
  [[nodiscard]] std::size_t indexFromCatalog(Catalog catalog) const noexcept;
  [[nodiscard]] Catalog catalogFromIndex(std::size_t index) const noexcept;
  [[nodiscard]] bool windowsAvailable() const noexcept;

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Segmented* m_catalogSwitch = nullptr;
  Label* m_detailLabel = nullptr;
  Flex* m_shellHost = nullptr;
  Slider* m_roundness = nullptr;
  Label* m_roundnessValue = nullptr;
  Flex* m_barHost = nullptr;
  Slider* m_barRadius = nullptr;
  Label* m_barRadiusValue = nullptr;
  Flex* m_dockHost = nullptr;
  Slider* m_dockRadius = nullptr;
  Label* m_dockRadiusValue = nullptr;
  Flex* m_windowHost = nullptr;
  Slider* m_windowRadius = nullptr;
  Label* m_windowRadiusValue = nullptr;

  std::string m_barName;
  Catalog m_catalog = Catalog::Shell;
  Catalog m_pinnedCatalog = Catalog::Shell;
  bool m_hosted = false;
  bool m_catalogSyncing = false;
  bool m_syncing = false;
  bool m_liveDirty = false;
};
