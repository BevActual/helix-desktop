#pragma once

#include "shell/control_center/tab.h"
#include "ui/controls/scroll_view.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class Button;
class CastService;
class ClipboardService;
class ConfigService;
class Flex;
class Label;
class ScrollView;
class Segmented;
class Toggle;
struct ControlCenterServices;

class CastTab : public Tab {
public:
  enum class Catalog : std::uint8_t { Wireless, Displays };

  explicit CastTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setActive(bool active) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void syncUi();
  void rebuildSinks();
  void copyShareTarget();
  void applyCatalog(std::size_t index);
  void persistCatalog();
  void syncCatalog();
  [[nodiscard]] static std::size_t indexFromCatalog(Catalog catalog) noexcept;
  [[nodiscard]] static Catalog catalogFromIndex(std::size_t index) noexcept;

  CastService* m_service = nullptr;
  ClipboardService* m_clipboard = nullptr;
  ConfigService* m_config = nullptr;

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
  Segmented* m_catalogSwitch = nullptr;
  Flex* m_wirelessCard = nullptr;
  Label* m_wirelessStatus = nullptr;
  Label* m_wirelessDetail = nullptr;
  Button* m_wirelessStart = nullptr;
  Button* m_wirelessOpen = nullptr;
  Flex* m_sinksCard = nullptr;
  Flex* m_sinkList = nullptr;
  ScrollView* m_liveScroll = nullptr;
  ScrollViewState m_liveScrollState{};
  std::string m_sinkSignature;
  Catalog m_catalog = Catalog::Wireless;
  bool m_syncing = false;
  bool m_catalogSyncing = false;
};
