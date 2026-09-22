#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

class ConfigService;
class Flex;
class Label;
class Renderer;
class Segmented;
class Slider;
class Toggle;
struct ControlCenterServices;

class PanelsTab : public Tab {
public:
  enum class Section : std::uint8_t { Placement, Chrome };

  explicit PanelsTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  [[nodiscard]] std::unique_ptr<Flex> createLiveBody(bool hosted = false);
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;
  void applyOpenSection(std::size_t index) { applySection(index); }
  [[nodiscard]] std::size_t currentSectionIndex() const noexcept { return indexFromSection(m_section); }

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void syncLiveControls();
  void syncHosts();
  void persistSection();
  void applySection(std::size_t index);
  void applyFloatingLayer(std::size_t index);
  void applyBorders(bool enabled);
  void applyShadow(bool enabled);
  void applyListItemBackground(bool enabled);
  void commitFloatingOffset();
  [[nodiscard]] static std::size_t indexFromSection(Section section) noexcept;
  [[nodiscard]] static Section sectionFromIndex(std::size_t index) noexcept;
  [[nodiscard]] static Section sectionFromId(std::string_view id) noexcept;
  [[nodiscard]] static std::string_view idFromSection(Section section) noexcept;

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Segmented* m_sectionSwitch = nullptr;
  Label* m_caption = nullptr;
  Flex* m_placementHost = nullptr;
  Flex* m_chromeHost = nullptr;
  Segmented* m_floatingLayer = nullptr;
  Toggle* m_borders = nullptr;
  Toggle* m_shadow = nullptr;
  Toggle* m_listItemBackground = nullptr;
  Slider* m_floatingOffset = nullptr;
  Label* m_floatingOffsetValue = nullptr;

  Section m_section = Section::Placement;
  bool m_hosted = false;
  bool m_syncing = false;
  bool m_sectionSyncing = false;
  bool m_liveDirty = false;
};
