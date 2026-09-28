#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class ConfigService;
class Button;
class ContextMenuPopup;
class EasyEffectsService;
class Flex;
class Glyph;
class Label;
class MprisService;
class Node;
class PipeWireService;
class RenderContext;
class Renderer;
class ScrollView;
class Segmented;
class Select;
class Slider;
class WaylandConnection;
struct AudioNode;
struct AudioState;
struct ControlCenterServices;

class AudioTab : public Tab {
public:
  AudioTab(
      PipeWireService* audio, EasyEffectsService* easyEffects, MprisService* mpris, ConfigService* config,
      WaylandConnection* wayland, RenderContext* renderContext, const ControlCenterServices& services
  );
  ~AudioTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setActive(bool active) override;
  bool dismissTransientUi() override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;
  [[nodiscard]] bool dragging() const noexcept;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;

private:
  struct DeviceVolumeCardState {
    Flex* menuAnchor = nullptr;
    Button* menuButton = nullptr;
    Label* deviceLabel = nullptr;
    Slider* slider = nullptr;
    Label* valueLabel = nullptr;
    Button* muteButton = nullptr;
    Flex* effectsProfileRow = nullptr;
    Select* effectsProfileSelect = nullptr;
    Timer volumeDebounceTimer;
    bool syncing = false;
  };

  struct DeviceMenuModel {
    std::function<std::span<const AudioNode>(const AudioState&)> devices;
    std::function<std::uint32_t(const AudioState&)> defaultDeviceId;
    std::function<void(std::uint32_t)> activate;
  };

  struct DeviceVolumeCardSpec {
    DeviceVolumeCardState& state;
    DeviceMenuModel deviceMenu;
    std::string_view devicePrefixKey;
    std::string_view noDeviceKey;
    std::string_view muteGlyph;
    std::function<void(float)> queueVolume;
    std::function<void()> toggleMute;
  };

  // macOS-style drill-down: Root is the main page (volume card + navigation rows); the rest
  // are sub-panes opened from those rows, with a back header.
  enum class Catalog : std::uint8_t { Root, Output, Input, Apps, Effects };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  std::unique_ptr<Flex> createDeviceVolumeCard(DeviceVolumeCardSpec card);
  void rebuildLists(Renderer& renderer);
  void rebuildProgramVolumes(Renderer& renderer);
  void applyCatalog(std::size_t index, bool persist = true);
  void openPage(Catalog page);
  void persistCatalog();
  void syncCatalog();
  [[nodiscard]] bool syncDetailLabel();
  [[nodiscard]] static Catalog catalogFromState(std::string_view value) noexcept;
  [[nodiscard]] static std::string_view catalogState(Catalog catalog) noexcept;
  [[nodiscard]] static std::size_t indexFromCatalog(Catalog catalog) noexcept;
  [[nodiscard]] static Catalog catalogFromIndex(std::size_t index) noexcept;
  void syncValueLabelWidths(Renderer& renderer);
  void syncProgramVolumeRows();
  void queueProgramSinkVolume(std::uint32_t id, float value);
  void flushPendingProgramVolumes(bool force = false);
  [[nodiscard]] float sliderMaxPercent() const;
  void flushPendingVolumes(bool force = false);

  void openDeviceMenu(DeviceVolumeCardState& card, const DeviceMenuModel& menu);
  void openProgramRoutingMenu(Node* anchor, std::uint32_t programStreamId);
  void syncEffectsProfileControls(Renderer& renderer);

  PipeWireService* m_audio = nullptr;
  EasyEffectsService* m_easyEffects = nullptr;
  MprisService* m_mpris = nullptr;
  ConfigService* m_config = nullptr;
  WaylandConnection* m_wayland = nullptr;
  RenderContext* m_renderContext = nullptr;
  std::unique_ptr<SettingsPane> m_settings;

  Flex* m_rootLayout = nullptr;
  Flex* m_compactCard = nullptr;
  Segmented* m_catalogSwitch = nullptr;
  Label* m_detailLabel = nullptr;
  Flex* m_outputHost = nullptr;
  Flex* m_inputHost = nullptr;
  Flex* m_appsHost = nullptr;
  ScrollView* m_outputScroll = nullptr;
  ScrollView* m_inputScroll = nullptr;
  Flex* m_outputList = nullptr;
  Flex* m_inputList = nullptr;
  ScrollView* m_programScroll = nullptr;
  Flex* m_effectsHost = nullptr;
  Flex* m_rootNav = nullptr;
  Flex* m_subHeader = nullptr;
  Label* m_subTitle = nullptr;
  Flex* m_programList = nullptr;
  bool m_catalogSyncing = false;
  Catalog m_catalog = Catalog::Root;
  Catalog m_pinnedCatalog = Catalog::Root;
  std::vector<Flex*> m_programRows;
  std::string m_lastProgramListKey;
  float m_lastProgramSliderMax = -1.0F;
  std::string m_lastEffectsProfileListKey;
  float m_syncedPercentLabelMinWidth = -1.0F;
  float m_lastSyncedPercentLabelSliderMax = -1.0F;
  DeviceVolumeCardState m_outputDeviceVolume;
  DeviceVolumeCardState m_inputDeviceVolume;
  std::unique_ptr<ContextMenuPopup> m_deviceMenuPopup;
  DeviceVolumeCardState* m_openDeviceMenuCard = nullptr;
  std::uint32_t m_openProgramRoutingMenuStreamId = 0;

  float m_lastOutputWidth = -1.0F;
  float m_lastInputWidth = -1.0F;
  std::string m_lastOutputListKey;
  std::string m_lastInputListKey;
  float m_lastSinkVolume = -1.0F;
  float m_lastSourceVolume = -1.0F;
  std::uint32_t m_pendingSinkId = 0;
  std::uint32_t m_pendingSourceId = 0;
  float m_pendingSinkVolume = -1.0F;
  float m_pendingSourceVolume = -1.0F;
  float m_lastSentSinkVolume = -1.0F;
  float m_lastSentSourceVolume = -1.0F;
  std::chrono::steady_clock::time_point m_lastSinkCommitAt;
  std::chrono::steady_clock::time_point m_lastSourceCommitAt;
  Timer m_programSinkDebounceTimer;
  std::uint32_t m_pendingProgramSinkId = 0;
  float m_pendingProgramSinkVolume = -1.0F;
};
