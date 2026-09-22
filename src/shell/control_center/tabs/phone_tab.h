#pragma once

#include "render/core/thumbnail_service.h"
#include "shell/control_center/tab.h"
#include "ui/controls/scroll_view.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Button;
class Toggle;
class ClipboardService;
class ConfigService;
class Flex;
class Input;
class Label;
class PhoneConnectService;
class Renderer;
class ScrollView;
class Segmented;
class ThumbnailService;
class VirtualGridAdapter;
class VirtualGridView;
struct ControlCenterServices;
struct PhoneContinueItem;
struct PhoneCastApp;
struct PhoneNotification;

class PhoneTab : public Tab {
public:
  enum class Catalog : std::uint8_t { Files, Notifications, Continue, Cast };

  explicit PhoneTab(const ControlCenterServices& services);
  ~PhoneTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setActive(bool active) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void syncUi();
  void rebuildDevices();
  void rebuildContinue();
  void rebuildNotifications();
  void rebuildCast();
  void refreshFiles();
  void applyCatalog(std::size_t index);
  void persistCatalog();
  void persistPeer();
  void syncCatalog();
  void selectPeer(std::string deviceId);
  void sendFileTo(const std::string& deviceId);
  void sendClipboardTo(const std::string& deviceId);
  void pullRemoteFile(const std::string& path);
  void openContinueItem(const PhoneContinueItem& item);
  void copySmsCode(const std::string& code);
  void dismissPhoneNotification(const PhoneNotification& item);
  void launchCastAppFromUi(const std::string& package, const std::string& label);
  void releaseCastIcons();
  [[nodiscard]] std::string peerDeviceId() const;
  [[nodiscard]] static std::size_t indexFromCatalog(Catalog catalog) noexcept;
  [[nodiscard]] static Catalog catalogFromIndex(std::size_t index) noexcept;

  PhoneConnectService* m_service = nullptr;
  ClipboardService* m_clipboard = nullptr;
  ConfigService* m_config = nullptr;
  ThumbnailService* m_thumbnails = nullptr;

  Flex* m_rootLayout = nullptr;
  Label* m_status = nullptr;
  Label* m_detail = nullptr;
  Button* m_activateButton = nullptr;
  Flex* m_deviceList = nullptr;
  Segmented* m_catalogSwitch = nullptr;
  Flex* m_filesHost = nullptr;
  Flex* m_notificationsHost = nullptr;
  Flex* m_continueHost = nullptr;
  Flex* m_castHost = nullptr;
  Label* m_filesEmpty = nullptr;
  Label* m_notificationsEmpty = nullptr;
  Label* m_continueEmpty = nullptr;
  Label* m_castEmpty = nullptr;
  Button* m_castMirrorButton = nullptr;
  Flex* m_castAlwaysOnRow = nullptr;
  Toggle* m_castAlwaysOn = nullptr;
  Flex* m_castAudioRow = nullptr;
  Toggle* m_castAudio = nullptr;
  Flex* m_castNotificationsRow = nullptr;
  Toggle* m_castNotifications = nullptr;
  Label* m_castNotificationsStatus = nullptr;
  Flex* m_castPointerWell = nullptr;
  Label* m_castPointerStatus = nullptr;
  Flex* m_castPointerActions = nullptr;
  Button* m_castPointerSetup = nullptr;
  Flex* m_castAdbRow = nullptr;
  Input* m_castHostInput = nullptr;
  Input* m_castPairInput = nullptr;
  Input* m_castSearchInput = nullptr;
  Button* m_castConnectButton = nullptr;
  Flex* m_notificationsList = nullptr;
  Flex* m_continueList = nullptr;
  Flex* m_castList = nullptr;
  VirtualGridView* m_filesGrid = nullptr;
  ScrollView* m_leftoverScroll = nullptr;
  ScrollView* m_notificationsScroll = nullptr;
  ScrollView* m_castScroll = nullptr;
  ScrollViewState m_leftoverScrollState{};
  ScrollViewState m_notificationsScrollState{};
  ScrollViewState m_castScrollState{};
  std::unique_ptr<VirtualGridAdapter> m_filesAdapter;
  std::vector<std::string> m_filePaths;
  std::vector<std::string> m_castIconPaths;
  struct CastIconBind {
    Flex* icon = nullptr;
    std::string path;
    bool recent = false;
  };
  std::vector<CastIconBind> m_castIconBinds;
  ThumbnailService::Subscription m_thumbnailSub;
  std::string m_deviceSignature;
  std::string m_continueSignature;
  std::string m_notificationsSignature;
  std::string m_castSignature;
  std::string m_castHostPrefill;
  std::string m_castQuery;
  std::string m_filesSignature;
  std::string m_filesDeviceId;
  std::string m_peerId;
  Catalog m_catalog = Catalog::Files;
  bool m_syncing = false;
  bool m_syncingUi = false;
  bool m_thumbnailRefreshPending = false;
};
