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
class ScrollView;
class Separator;
class SyncthingService;
struct ControlCenterServices;

class SyncthingTab : public Tab {
public:
  explicit SyncthingTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setActive(bool active) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void syncUi();
  void rebuildFolders();
  void rebuildRemotes();
  void copyDeviceId();

  SyncthingService* m_service = nullptr;
  ClipboardService* m_clipboard = nullptr;

  Flex* m_rootLayout = nullptr;
  Label* m_status = nullptr;
  Label* m_detail = nullptr;
  Button* m_startButton = nullptr;
  Button* m_stopButton = nullptr;
  Button* m_guiButton = nullptr;
  Button* m_copyIdButton = nullptr;
  Flex* m_foldersCard = nullptr;
  Separator* m_foldersDivider = nullptr;
  Flex* m_folderList = nullptr;
  Flex* m_remotesCard = nullptr;
  Separator* m_remotesDivider = nullptr;
  Flex* m_remoteList = nullptr;
  ScrollView* m_liveScroll = nullptr;
  ScrollViewState m_liveScrollState{};
  std::string m_folderSignature;
  std::string m_remoteSignature;
};
