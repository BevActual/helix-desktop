#pragma once

#include "render/core/texture_handle.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "system/camera_service.h"
#include "ui/signal.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Button;
class ConfigService;
class Flex;
class Image;
class Label;
class Renderer;
class ScrollView;
class Select;
struct ControlCenterServices;

class CameraTab : public Tab {
public:
  explicit CameraTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;

  void selectDevice(std::size_t index);
  void startSelectedDevice();
  void rebuildControls();
  void appendControl(Flex& parent, const CameraControl& control, bool withDivider);
  void layoutStage(float contentWidth, float bodyHeight);
  void layoutPreviewBox(float contentWidth, float bodyHeight);
  void pinPreviewImage();
  void syncPreviewAvailability();
  void syncHeaderActions();
  void refreshDependentVisibility();
  void onFrame(int width, int height, const std::uint8_t* rgba);

  ConfigService* m_config = nullptr;
  CameraService* m_camera = nullptr;
  std::unique_ptr<SettingsPane> m_settings;

  Flex* m_rootLayout = nullptr;
  Flex* m_stage = nullptr;
  Flex* m_previewColumn = nullptr;
  Flex* m_previewCard = nullptr;
  Select* m_deviceSelect = nullptr;
  Label* m_emptyLabel = nullptr;
  Flex* m_previewBox = nullptr;
  Image* m_previewImage = nullptr;
  Label* m_previewUnavailableLabel = nullptr;
  Flex* m_framingHost = nullptr;
  ScrollView* m_controlsScroll = nullptr;
  Flex* m_controlsHost = nullptr;
  Button* m_resetButton = nullptr;

  std::vector<CameraDevice> m_devices;
  std::string m_selectedDeviceId;
  bool m_active = false;
  std::unordered_map<std::uint32_t, Flex*> m_controlRows;

  TextureHandle m_previewTexture{};
  bool m_previewBound = false;
  int m_previewTexWidth = 0;
  int m_previewTexHeight = 0;
  std::vector<std::uint8_t> m_pendingFrame;
  int m_pendingWidth = 0;
  int m_pendingHeight = 0;
  bool m_frameDirty = false;
};
