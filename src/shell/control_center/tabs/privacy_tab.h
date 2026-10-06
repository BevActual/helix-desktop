#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "ui/controls/scroll_view.h"

#include <memory>
#include <string>
#include <string_view>

class Button;
class ConfigService;
class Flex;
class Label;
class PipeWireService;
class ScrollView;
class Separator;
class Toggle;
struct ControlCenterServices;

class PrivacyTab : public Tab {
public:
  explicit PrivacyTab(const ControlCenterServices& services);

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
  void syncUi();
  void rebuildCaptures();
  void syncMute();
  void syncSharing();

  PipeWireService* m_audio = nullptr;
  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;

  Flex* m_rootLayout = nullptr;
  Flex* m_capturesWell = nullptr;
  Flex* m_capturesLeftover = nullptr;
  Label* m_status = nullptr;
  Label* m_detail = nullptr;
  Button* m_muteButton = nullptr;
  Toggle* m_locationToggle = nullptr;
  Label* m_locationHint = nullptr;
  Toggle* m_telemetryToggle = nullptr;
  Label* m_telemetryHint = nullptr;
  Separator* m_micDivider = nullptr;
  Flex* m_micCard = nullptr;
  Flex* m_micList = nullptr;
  Separator* m_cameraDivider = nullptr;
  Flex* m_cameraCard = nullptr;
  Flex* m_cameraList = nullptr;
  Separator* m_screenDivider = nullptr;
  Flex* m_screenCard = nullptr;
  Flex* m_screenList = nullptr;
  ScrollView* m_liveScroll = nullptr;
  ScrollViewState m_liveScrollState{};
  std::string m_captureSignature;
  bool m_syncing = false;
};
