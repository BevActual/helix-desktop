#pragma once

#include "shell/control_center/tab.h"
#include "ui/controls/scroll_view.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Button;
class ConfigService;
class Flex;
class Label;
class ScrollView;
class Segmented;
class Select;
class Toggle;
class FirewallService;
struct ControlCenterServices;

class FirewallTab : public Tab {
public:
  enum class Catalog : std::uint8_t { Services, Zone };

  explicit FirewallTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setActive(bool active) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void syncUi();
  void rebuildServices();
  void applyCatalog(std::size_t index);
  void persistCatalog();
  void syncCatalog();
  [[nodiscard]] static std::size_t indexFromCatalog(Catalog catalog) noexcept;
  [[nodiscard]] static Catalog catalogFromIndex(std::size_t index) noexcept;

  FirewallService* m_service = nullptr;
  ConfigService* m_config = nullptr;

  Flex* m_rootLayout = nullptr;
  Label* m_status = nullptr;
  Label* m_detail = nullptr;
  Button* m_startButton = nullptr;
  Flex* m_panicRow = nullptr;
  Toggle* m_panicToggle = nullptr;
  Segmented* m_catalogSwitch = nullptr;
  Flex* m_zoneCard = nullptr;
  Select* m_zoneSelect = nullptr;
  Flex* m_servicesCard = nullptr;
  Flex* m_serviceList = nullptr;
  Button* m_saveButton = nullptr;
  ScrollView* m_liveScroll = nullptr;
  ScrollViewState m_liveScrollState{};
  std::vector<std::string> m_zoneIds;
  std::string m_zoneOptionsKey;
  std::string m_serviceSignature;
  Catalog m_catalog = Catalog::Services;
  bool m_syncing = false;
  bool m_catalogSyncing = false;
};
