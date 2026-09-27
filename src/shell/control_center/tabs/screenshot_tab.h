#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/recent_captures.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Button;
class ConfigService;
class Flex;
class DesktopAiService;
class IpcService;
class Label;
class Renderer;
class Segmented;
class ThumbnailService;
struct ControlCenterServices;

class ScreenshotTab {
public:
  explicit ScreenshotTab(const ControlCenterServices& services);

  [[nodiscard]] std::unique_ptr<Flex> createCompactBody();
  [[nodiscard]] std::unique_ptr<Flex> createLeftoverBody();
  void setActive(bool active);
  void onClose();
  void updateContent(Renderer& renderer);
  void cancelCountdown();
  void setContentScale(float scale) noexcept { m_contentScale = scale; }
  void setPanelCardOpacity(float opacity) noexcept;

private:
  void requestCapture(const char* command);
  void runCapture(const char* command);
  void tickCountdown();
  void syncCountdownUi();
  void openScreenshotFolder();
  void openRecentFile(const std::string& path);
  void annotateRecentFile(const std::string& path);
  void rebuildRecentFiles();
  void bindRecentGallery();
  [[nodiscard]] int delaySeconds() const noexcept;
  [[nodiscard]] float contentScale() const noexcept { return m_contentScale; }
  [[nodiscard]] float panelCardOpacity() const noexcept { return m_panelCardOpacity; }

  struct CaptureButton {
    Button* button = nullptr;
    bool available = true;
  };

  IpcService* m_ipc = nullptr;
  DesktopAiService* m_desktopAi = nullptr;
  std::string m_aiStatus;
  ConfigService* m_config = nullptr;
  ThumbnailService* m_thumbnails = nullptr;
  control_center::RecentCaptureGallery m_recents;
  Flex* m_rootLayout = nullptr;
  Segmented* m_delay = nullptr;
  Label* m_status = nullptr;
  Flex* m_recentList = nullptr;
  Label* m_recentEmpty = nullptr;
  std::vector<CaptureButton> m_captureButtons;
  Timer m_delayTimer;
  const char* m_pendingCommand = nullptr;
  int m_countdownLeft = 0;
  std::size_t m_delayIndex = 0;
  float m_contentScale = 1.0F;
  float m_panelCardOpacity = 1.0F;
};
