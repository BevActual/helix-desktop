#pragma once

#include "shell/control_center/tab.h"
#include "ui/controls/scroll_view.h"

#include <memory>
#include <string>
#include <string_view>

class Button;
class ClipboardService;
class Flex;
class Label;
class PhoneConnectService;
class ScrollView;
class Separator;
struct ControlCenterServices;

class PhoneTab : public Tab {
public:
  explicit PhoneTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setActive(bool active) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void syncUi();
  void rebuildDevices();
  void sendFileTo(const std::string& deviceId);
  void sendClipboardTo(const std::string& deviceId);

  PhoneConnectService* m_service = nullptr;
  ClipboardService* m_clipboard = nullptr;

  Flex* m_rootLayout = nullptr;
  Label* m_status = nullptr;
  Label* m_detail = nullptr;
  Button* m_activateButton = nullptr;
  Flex* m_devicesCard = nullptr;
  Flex* m_deviceList = nullptr;
  ScrollView* m_liveScroll = nullptr;
  ScrollViewState m_liveScrollState{};
  std::string m_deviceSignature;
};
