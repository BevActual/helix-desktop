#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Button;
class ConfigService;
class Flex;
class Label;
class Renderer;
class Segmented;
class Select;
class Slider;
class Toggle;
struct ControlCenterServices;

class EffectsTab : public Tab {
public:
  explicit EffectsTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  [[nodiscard]] std::unique_ptr<Flex> createLiveBody();
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;

private:
  enum class Catalog : std::uint8_t { Borders, Shadows, Speed };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void syncLiveControls();
  void refreshCaption();
  void applyHighContrast(bool enabled);
  void applyAnimations(bool enabled);
  void commitAnimationSpeed();
  void commitShadowAlpha();
  void applyShadowDirection(std::size_t index);
  void applyCatalog(std::size_t index, bool persist = true);
  void persistCatalog();
  void syncCatalog();
  void previewCatalog(std::size_t index);
  void restorePinnedCatalog();
  [[nodiscard]] static Catalog catalogFromState(std::string_view value) noexcept;
  [[nodiscard]] static std::string_view catalogState(Catalog catalog) noexcept;
  [[nodiscard]] static std::size_t indexFromCatalog(Catalog catalog) noexcept;
  [[nodiscard]] static Catalog catalogFromIndex(std::size_t index) noexcept;

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Button* m_contrastTile = nullptr;
  Button* m_motionTile = nullptr;
  Segmented* m_catalogSwitch = nullptr;
  Label* m_caption = nullptr;
  Flex* m_bordersHost = nullptr;
  Flex* m_shadowsHost = nullptr;
  Flex* m_speedHost = nullptr;
  Flex* m_animationSpeedSection = nullptr;
  Slider* m_animationSpeed = nullptr;
  Label* m_animationSpeedValue = nullptr;
  Label* m_speedOffCaption = nullptr;
  Toggle* m_buttonBorders = nullptr;
  Toggle* m_inputBorders = nullptr;
  Toggle* m_popupBorders = nullptr;
  Toggle* m_cardBorders = nullptr;
  Toggle* m_popupShadows = nullptr;
  Flex* m_shadowDetails = nullptr;
  Select* m_shadowDirection = nullptr;
  Slider* m_shadowAlpha = nullptr;
  Label* m_shadowAlphaValue = nullptr;
  std::vector<std::string> m_shadowDirectionKeys;

  Catalog m_catalog = Catalog::Borders;
  Catalog m_pinnedCatalog = Catalog::Borders;
  Timer m_hoverEndTimer;
  bool m_catalogSyncing = false;
  bool m_syncing = false;
  bool m_liveDirty = false;
};
