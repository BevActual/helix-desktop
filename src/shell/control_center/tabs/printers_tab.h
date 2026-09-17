#pragma once

#include "shell/control_center/tab.h"
#include "ui/controls/scroll_view.h"

#include <memory>
#include <string>
#include <string_view>

class Button;
class Flex;
class Label;
class PrinterService;
class Separator;
class ScrollView;
struct ControlCenterServices;

class PrintersTab : public Tab {
public:
  explicit PrintersTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setActive(bool active) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void syncUi();
  void rebuildPrinters();
  void printTo(const std::string& name);

  PrinterService* m_service = nullptr;

  Flex* m_rootLayout = nullptr;
  Label* m_status = nullptr;
  Label* m_detail = nullptr;
  Button* m_startButton = nullptr;
  Button* m_openButton = nullptr;
  Flex* m_printersCard = nullptr;
  Separator* m_printersDivider = nullptr;
  Flex* m_printerList = nullptr;
  ScrollView* m_liveScroll = nullptr;
  ScrollViewState m_liveScrollState{};
  std::string m_printerSignature;
};
