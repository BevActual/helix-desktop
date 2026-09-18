#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <memory>
#include <string>
#include <string_view>

class ConfigService;
class Flex;
class Input;
class Label;
class Renderer;
class VirtualGridAdapter;
class VirtualGridView;
struct ControlCenterServices;

class TypeTab : public Tab {
public:
  explicit TypeTab(const ControlCenterServices& services);
  ~TypeTab() override;

  std::unique_ptr<Flex> create() override;
  [[nodiscard]] std::unique_ptr<Flex> createLiveBody();
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
  void applyFontFamily(std::string family);
  void refreshFontGrid();

  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Signal<>::ScopedConnection m_settingsInvalidatedConn;
  std::unique_ptr<VirtualGridAdapter> m_fontAdapter;

  Flex* m_rootLayout = nullptr;
  Label* m_fontName = nullptr;
  Label* m_fontPreviewCaption = nullptr;
  Label* m_fontPreviewBody = nullptr;
  Label* m_fontPreview = nullptr;
  Label* m_fontPreviewPangram = nullptr;
  Label* m_fontPreviewGhost = nullptr;
  Label* m_fontPreviewPrimary = nullptr;
  Input* m_filter = nullptr;
  VirtualGridView* m_grid = nullptr;
  Label* m_empty = nullptr;
  std::string m_query;

  bool m_syncing = false;
  bool m_liveDirty = false;
};
