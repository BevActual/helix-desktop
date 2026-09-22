#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"

#include <cstdint>
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
class Segmented;
class Toggle;
struct ControlCenterServices;

class LocalSendTab : public Tab {
public:
  enum class Catalog : std::uint8_t { Nearby, Incoming, History };

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
  void onPanelCardOpacityChanged(float opacity) override;
  void syncUi();
  void rebuildPeers();
  void sendFileTo(const std::string& peerKey);
  void sendFolderTo(const std::string& peerKey);
  void sendClipboardTo(const std::string& peerKey);
  void rebuildHistory();
  void applyCatalog(std::size_t index);
  void persistCatalog();
  void syncCatalog();
  [[nodiscard]] static std::size_t indexFromCatalog(Catalog catalog) noexcept;
  [[nodiscard]] static Catalog catalogFromIndex(std::size_t index) noexcept;

  LocalSendService* m_service = nullptr;
  ClipboardService* m_clipboard = nullptr;
  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;

  Flex* m_rootLayout = nullptr;
  Label* m_status = nullptr;
  Label* m_detail = nullptr;
  Toggle* m_enabled = nullptr;
  Input* m_pin = nullptr;
  Segmented* m_catalogSwitch = nullptr;
  Flex* m_incomingCard = nullptr;
  Label* m_incomingLabel = nullptr;
  Flex* m_incomingActions = nullptr;
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
  Catalog m_catalog = Catalog::Nearby;
  bool m_syncing = false;
  bool m_catalogSyncing = false;
};
