#pragma once

#include "shell/control_center/tab.h"
#include "ui/controls/scroll_view.h"

#include <memory>
#include <string>
#include <string_view>

class Button;
class CastService;
class ClipboardService;
class Flex;
class Label;
class ScrollView;
class Toggle;
struct ControlCenterServices;

class CastTab : public Tab {
public:
  explicit CastTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setActive(bool active) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void syncUi();
  void rebuildSinks();
  void copyShareTarget();

  CastService* m_service = nullptr;
  ClipboardService* m_clipboard = nullptr;

  Flex* m_rootLayout = nullptr;
  Label* m_shareStatus = nullptr;
  Label* m_shareDetail = nullptr;
  Flex* m_shareToggleRow = nullptr;
  Toggle* m_shareToggle = nullptr;
  Flex* m_viewOnlyRow = nullptr;
  Toggle* m_viewOnlyToggle = nullptr;
  Flex* m_credentialsRow = nullptr;
  Label* m_credentials = nullptr;
  Button* m_copyButton = nullptr;
  Label* m_wirelessStatus = nullptr;
  Label* m_wirelessDetail = nullptr;
  Button* m_wirelessStart = nullptr;
  Button* m_wirelessOpen = nullptr;
  Flex* m_sinksCard = nullptr;
  Flex* m_sinkList = nullptr;
  ScrollView* m_liveScroll = nullptr;
  ScrollViewState m_liveScrollState{};
  std::string m_sinkSignature;
  bool m_syncing = false;
};
