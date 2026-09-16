#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
  enum class Section : std::uint8_t { Attention, Open, Apps, Capture, Sound, Session };

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
  void onPanelCardOpacityChanged(float opacity) override;
  void syncLiveControls();
  void applyLiveGroups();
  void applySection(std::size_t index);
  void persistSection();
  void syncSectionSwitch();
  void copyMissingNiriBinds();
  void installMissingNiriBinds();
  void resetNiriBinds();
  void openNiriConfigFolder();
  void rebuildLivePane();
  [[nodiscard]] bool niriAvailable() const;
  [[nodiscard]] bool attentionNeeded() const;
  [[nodiscard]] std::vector<Section> visibleSections() const;
  [[nodiscard]] static std::string_view sectionGroup(Section section);
  [[nodiscard]] static std::string_view sectionTabKey(Section section);
  [[nodiscard]] static std::string_view sectionTabTooltipKey(Section section);
  [[nodiscard]] static std::string_view sectionStateKey(Section section);
  [[nodiscard]] static Section sectionFromState(std::string_view value);

  ClipboardService* m_clipboard = nullptr;
  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  std::unique_ptr<SettingsPane> m_liveSettings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;

  Flex* m_rootLayout = nullptr;
  Label* m_hint = nullptr;
  Flex* m_sectionSwitchHost = nullptr;
  Segmented* m_sectionSwitch = nullptr;
  Flex* m_liveHost = nullptr;
  Button* m_copyMissingButton = nullptr;
  Button* m_installButton = nullptr;
  Button* m_resetButton = nullptr;
  Button* m_folderButton = nullptr;

  Section m_section = Section::Open;
  std::vector<Section> m_visibleSections;
  bool m_searching = false;
  bool m_sectionSyncing = false;
  bool m_liveDirty = false;
};
