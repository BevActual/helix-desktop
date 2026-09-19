#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <cstdint>
#include <memory>
#include <string_view>

class ConfigService;
class Flex;
class Label;
class Renderer;
class Segmented;
class Slider;
struct ControlCenterServices;

class WindowsTab : public Tab {
public:
  enum class Section : std::uint8_t { Spacing, Columns };

  explicit WindowsTab(const ControlCenterServices& services);

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
  void commitGaps();
  void commitStruts();
  void commitColumnWidth();
  void persistLayout();
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
  Flex* m_spacingHost = nullptr;
  Flex* m_columnsHost = nullptr;
  Slider* m_gaps = nullptr;
  Label* m_gapsValue = nullptr;
  Slider* m_struts = nullptr;
  Label* m_strutsValue = nullptr;
  Slider* m_columnWidth = nullptr;
  Label* m_columnWidthValue = nullptr;

  Section m_section = Section::Spacing;
  bool m_syncing = false;
  bool m_sectionSyncing = false;
  bool m_liveDirty = false;
};
