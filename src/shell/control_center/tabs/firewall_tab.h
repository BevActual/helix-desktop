#pragma once

#include "shell/control_center/tab.h"
#include "ui/controls/scroll_view.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Button;
class Flex;
class Label;
class ScrollView;
class Select;
class Separator;
class Toggle;
class FirewallService;
struct ControlCenterServices;

class FirewallTab : public Tab {
public:
  explicit FirewallTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setActive(bool active) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void syncUi();
  void rebuildServices();

  FirewallService* m_service = nullptr;

  Flex* m_rootLayout = nullptr;
  Label* m_status = nullptr;
  Label* m_detail = nullptr;
  Button* m_startButton = nullptr;
  Flex* m_panicRow = nullptr;
  Toggle* m_panicToggle = nullptr;
  Flex* m_zoneCard = nullptr;
  Separator* m_zoneDivider = nullptr;
  Select* m_zoneSelect = nullptr;
  Flex* m_servicesCard = nullptr;
  Separator* m_servicesDivider = nullptr;
  Flex* m_serviceList = nullptr;
  Button* m_saveButton = nullptr;
  ScrollView* m_liveScroll = nullptr;
  ScrollViewState m_liveScrollState{};
  std::vector<std::string> m_zoneIds;
  std::string m_zoneOptionsKey;
  std::string m_serviceSignature;
  bool m_syncing = false;
};
