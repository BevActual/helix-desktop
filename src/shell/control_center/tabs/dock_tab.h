#pragma once

#include "shell/control_center/pinned_app_list.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class ConfigService;
class Flex;
class Label;
class ListEditor;
class Renderer;
class Segmented;
class Toggle;
struct ControlCenterServices;

class DockTab : public Tab {
public:
  enum class Section : std::uint8_t { Layout, Pinned };

  explicit DockTab(const ControlCenterServices& services);

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
  void applyShowRunning(bool enabled);
  void commitPinned(std::vector<std::string> items);
  [[nodiscard]] static std::size_t indexFromSection(Section section) noexcept;
  [[nodiscard]] static Section sectionFromIndex(std::size_t index) noexcept;
  [[nodiscard]] static Section sectionFromId(std::string_view id) noexcept;
  [[nodiscard]] static std::string_view idFromSection(Section section) noexcept;

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Toggle* m_enabled = nullptr;
  Flex* m_sectionToolbar = nullptr;
  Segmented* m_sectionSwitch = nullptr;
  Label* m_caption = nullptr;
  Flex* m_layoutHost = nullptr;
  Flex* m_pinnedHost = nullptr;
  Segmented* m_position = nullptr;
  Segmented* m_autoHide = nullptr;
  Toggle* m_showRunning = nullptr;
  ListEditor* m_pinnedList = nullptr;
  control_center::PinnedAppList m_pinnedIcons;

  Section m_section = Section::Layout;
  bool m_hosted = false;
  bool m_syncing = false;
  bool m_sectionSyncing = false;
  bool m_liveDirty = false;
};
