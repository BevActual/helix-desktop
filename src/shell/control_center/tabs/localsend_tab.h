#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"

#include <memory>
#include <string>
#include <string_view>

class Button;
class ClipboardService;
class ConfigService;
class Flex;
class Input;
class Label;
class LocalSendService;
class ScrollView;
class Toggle;
struct ControlCenterServices;

class LocalSendTab : public Tab {
public:
  explicit LocalSendTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setActive(bool active) override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void syncUi();
  void rebuildPeers();
  void sendFileTo(const std::string& peerKey);
  void sendFolderTo(const std::string& peerKey);
  void sendClipboardTo(const std::string& peerKey);
  void rebuildHistory();

  LocalSendService* m_service = nullptr;
  ClipboardService* m_clipboard = nullptr;
  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;

  Flex* m_rootLayout = nullptr;
  Label* m_status = nullptr;
  Label* m_detail = nullptr;
  Toggle* m_enabled = nullptr;
  Input* m_pin = nullptr;
  Flex* m_incomingCard = nullptr;
  Label* m_incomingLabel = nullptr;
  Button* m_acceptButton = nullptr;
  Button* m_rejectButton = nullptr;
  Label* m_transfer = nullptr;
  Flex* m_historyCard = nullptr;
  Flex* m_historyList = nullptr;
  Flex* m_peersCard = nullptr;
  Flex* m_peerList = nullptr;
  ScrollView* m_liveScroll = nullptr;
  ScrollViewState m_liveScrollState{};
  std::string m_pinText;
  std::string m_peerSignature;
  std::string m_historySignature;
  bool m_syncing = false;
};
