#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <cstddef>
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
class Toggle;
struct ControlCenterServices;

class BarTab : public Tab {
public:
  enum class Section : std::uint8_t { Layout, Shape };

  explicit BarTab(const ControlCenterServices& services);

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
  void applyEnabled(bool enabled);
  void applyPosition(std::size_t index);
  void applyAutoHide(std::size_t index);
  void applyReserveSpace(bool enabled);
  void applyShadow(bool enabled);
  void commitThickness();
  void commitOpacity();
  [[nodiscard]] static std::size_t indexFromSection(Section section) noexcept;
  [[nodiscard]] static Section sectionFromIndex(std::size_t index) noexcept;
  [[nodiscard]] static Section sectionFromId(std::string_view id) noexcept;
  [[nodiscard]] static std::string_view idFromSection(Section section) noexcept;

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Flex* m_enableRow = nullptr;
  Flex* m_sectionToolbar = nullptr;
  Flex* m_layoutHost = nullptr;
  Flex* m_shapeHost = nullptr;
  Toggle* m_enabled = nullptr;
  Segmented* m_sectionSwitch = nullptr;
  Segmented* m_position = nullptr;
  Segmented* m_autoHide = nullptr;
  Toggle* m_reserveSpace = nullptr;
  Toggle* m_shadow = nullptr;
  Slider* m_thickness = nullptr;
  Label* m_thicknessValue = nullptr;
  Slider* m_opacity = nullptr;
  Label* m_opacityValue = nullptr;
  Label* m_empty = nullptr;
  Label* m_caption = nullptr;

  std::string m_barName;
  Section m_section = Section::Layout;
  bool m_hosted = false;
  bool m_syncing = false;
  bool m_sectionSyncing = false;
  bool m_liveDirty = false;
};
