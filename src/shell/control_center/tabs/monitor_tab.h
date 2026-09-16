#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "system/display_config.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

class BrightnessService;
class Button;
class ConfigService;
class CompositorPlatform;
class DisplayConfigService;
class Flex;
class GammaService;
class Glyph;
class Label;
class Renderer;
class ScrollView;
class Select;
class Slider;
class Toggle;
struct ControlCenterServices;

class MonitorTab : public Tab {
public:
  MonitorTab(
      BrightnessService* brightness, ConfigService* config, DisplayConfigService* displayConfig,
      const ControlCenterServices& services
  );

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;
  [[nodiscard]] bool dragging() const noexcept;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncHeaderActions();
  void syncStatusLabels();
  void rebuildCards(Renderer& renderer);
  void buildNightLightCard(Flex& parent, float scale);
  void syncNightLight();
  void persistNightLightEnabled(bool enabled);
  void persistNightLightForce(bool enabled);
  void commitNightLightTemperatures(bool fromDaySlider);
  void rebuildLayoutSection(float scale);
  void layoutArrangementMap(Renderer& renderer);
  void selectLayoutOutput(std::string name);
  void syncLayoutSelection();
  void beginLayoutPress(const std::string& name, float localX, float localY);
  void updateLayoutDrag(const std::string& name, float localX, float localY);
  void finishLayoutPress(const std::string& name);
  void applyLayoutDrag();
  [[nodiscard]] std::string layoutReferenceName() const;
  void queueBrightness(const std::string& displayId, float value);
  void flushPendingBrightness(bool force = false);
  void queueScale(const std::string& displayId, double value);
  void flushPendingScale(bool force = false);
  void applyResolution(const std::string& displayId, std::size_t index);
  void applyRefresh(const std::string& displayId, std::size_t index);
  void applyTransform(const std::string& displayId, std::size_t index);
  void applyLayout(display_config::LayoutDirection direction);
  [[nodiscard]] int layoutNudgePx() const;
  void reportLayoutError();

  BrightnessService* m_brightness = nullptr;
  ConfigService* m_configService = nullptr;
  DisplayConfigService* m_displayConfig = nullptr;
  CompositorPlatform* m_platform = nullptr;
  GammaService* m_nightLight = nullptr;
  std::unique_ptr<SettingsPane> m_settings;

  struct DisplayCard {
    std::string displayId;
    Flex* card = nullptr;
    Label* nameLabel = nullptr;
    Label* detailsLabel = nullptr;
    Glyph* icon = nullptr;
    Slider* slider = nullptr;
    Label* valueLabel = nullptr;
    Select* resolutionSelect = nullptr;
    Select* refreshSelect = nullptr;
    Select* transformSelect = nullptr;
    Slider* scaleSlider = nullptr;
    Label* scaleLabel = nullptr;
    Toggle* vrrToggle = nullptr;
    Toggle* enabledToggle = nullptr;
    std::vector<display_config::ResolutionChoice> resolutions;
    std::vector<display_config::Mode> refreshes;
    float lastBrightness = -1.0F;
    double lastScale = -1.0;
    bool lastControllable = true;
    std::string lastDisplayInfo;
  };

  Flex* m_rootLayout = nullptr;
  Label* m_noteLabel = nullptr;
  Label* m_errorLabel = nullptr;
  Flex* m_nightLightCard = nullptr;
  Flex* m_nightLightDetails = nullptr;
  Label* m_nightLightStatus = nullptr;
  Toggle* m_nightLightEnabled = nullptr;
  Toggle* m_nightLightForce = nullptr;
  Slider* m_nightLightDaySlider = nullptr;
  Label* m_nightLightDayValue = nullptr;
  Slider* m_nightLightNightSlider = nullptr;
  Label* m_nightLightNightValue = nullptr;
  Flex* m_emptyState = nullptr;
  ScrollView* m_cardsScroll = nullptr;
  Flex* m_cardsLayout = nullptr;
  struct LayoutTile {
    std::string name;
    Button* button = nullptr;
    float x = 0.0F;
    float y = 0.0F;
  };

  Flex* m_layoutSection = nullptr;
  Flex* m_layoutMap = nullptr;
  Label* m_layoutStatusLabel = nullptr;
  Select* m_layoutReferenceSelect = nullptr;
  Button* m_layoutPlaceTop = nullptr;
  Button* m_layoutPlaceLeft = nullptr;
  Button* m_layoutPlaceRight = nullptr;
  Button* m_layoutPlaceBottom = nullptr;
  Button* m_rescanButton = nullptr;
  Button* m_saveButton = nullptr;
  std::vector<DisplayCard> m_cards;
  std::vector<LayoutTile> m_layoutTiles;
  std::vector<std::string> m_layoutReferenceNames;
  std::string m_layoutMovingName;
  std::string m_layoutReferenceName;
  std::string m_layoutPressName;
  std::string m_lastDisplayListKey;
  float m_layoutGrabX = 0.0F;
  float m_layoutGrabY = 0.0F;
  float m_layoutMapScale = 1.0F;
  float m_layoutMapOffsetX = 0.0F;
  float m_layoutMapOffsetY = 0.0F;
  int m_layoutOriginX = 0;
  int m_layoutOriginY = 0;
  bool m_layoutDragging = false;

  std::string m_pendingDisplayId;
  float m_pendingBrightness = -1.0F;
  float m_lastSentBrightness = -1.0F;
  std::string m_pendingScaleId;
  double m_pendingScale = -1.0;
  std::chrono::steady_clock::time_point m_lastCommitAt;
  std::chrono::steady_clock::time_point m_ignoreStateUntil;
  Timer m_debounceTimer;
  Timer m_scaleDebounceTimer;
  bool m_syncingSlider = false;
  bool m_syncingConfig = false;
  bool m_syncingNightLight = false;
  bool m_active = false;
};
