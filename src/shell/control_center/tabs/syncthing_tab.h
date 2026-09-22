#pragma once

#include "shell/control_center/tab.h"
#include "ui/controls/scroll_view.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class Button;
class ClipboardService;
class ConfigService;
class Flex;
class Label;
class ScrollView;
class Segmented;
class SyncthingService;
struct ControlCenterServices;

class SyncthingTab : public Tab {
public:
  enum class Catalog : std::uint8_t { Folders, Devices };

  explicit SyncthingTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setActive(bool active) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void syncUi();
  void rebuildFolders();
  void rebuildRemotes();
  void copyDeviceId();
  void applyCatalog(std::size_t index);
  void persistCatalog();
  void syncCatalog();
  [[nodiscard]] static std::size_t indexFromCatalog(Catalog catalog) noexcept;
  [[nodiscard]] static Catalog catalogFromIndex(std::size_t index) noexcept;

  SyncthingService* m_service = nullptr;
  ClipboardService* m_clipboard = nullptr;
  ConfigService* m_config = nullptr;

  Flex* m_rootLayout = nullptr;
  Label* m_status = nullptr;
  Label* m_detail = nullptr;
  Button* m_startButton = nullptr;
  Button* m_stopButton = nullptr;
  Button* m_guiButton = nullptr;
  Button* m_copyIdButton = nullptr;
  Segmented* m_catalogSwitch = nullptr;
  Flex* m_foldersCard = nullptr;
  Flex* m_folderList = nullptr;
  Flex* m_remotesCard = nullptr;
  Flex* m_remoteList = nullptr;
  ScrollView* m_liveScroll = nullptr;
  ScrollViewState m_liveScrollState{};
  std::string m_folderSignature;
  std::string m_remoteSignature;
  Catalog m_catalog = Catalog::Folders;
  bool m_catalogSyncing = false;
};
