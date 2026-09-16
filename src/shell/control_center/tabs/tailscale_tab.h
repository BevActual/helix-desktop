#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Button;
class ClipboardService;
class Flex;
class Input;
class Label;
class ScrollView;
class Select;
class Toggle;
class TailscaleService;
struct ControlCenterServices;

class TailscaleTab : public Tab {
public:
  explicit TailscaleTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setActive(bool active) override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void syncUi();
  void rebuildPeers();
  void copyText(std::string_view text);
  void addToggleRow(Flex& parent, float scale, Label** labelOut, Toggle** toggleOut, const std::string& label);

  TailscaleService* m_service = nullptr;
  ClipboardService* m_clipboard = nullptr;
  std::unique_ptr<SettingsPane> m_settings;

  Flex* m_rootLayout = nullptr;
  Label* m_status = nullptr;
  Label* m_detail = nullptr;
  Label* m_health = nullptr;
  Flex* m_ipRow = nullptr;
  Label* m_ipv4 = nullptr;
  Label* m_ipv6 = nullptr;
  Flex* m_actionsRow = nullptr;
  Button* m_connectButton = nullptr;
  Button* m_copyIpButton = nullptr;
  Button* m_adminButton = nullptr;
  Toggle* m_shields = nullptr;
  Toggle* m_routes = nullptr;
  Toggle* m_ssh = nullptr;
  Toggle* m_dns = nullptr;
  Toggle* m_advertiseExit = nullptr;
  Toggle* m_allowLan = nullptr;
  Select* m_exitSelect = nullptr;
  Input* m_peerFilter = nullptr;
  Flex* m_peerList = nullptr;
  ScrollView* m_liveScroll = nullptr;
  ScrollViewState m_liveScrollState{};
  Flex* m_prefsCard = nullptr;
  Flex* m_peersCard = nullptr;

  std::string m_peerFilterText;
  std::string m_peerSignature;
  std::string m_exitOptionsKey;
  std::vector<std::string> m_exitTargets;
  bool m_syncing = false;
};
