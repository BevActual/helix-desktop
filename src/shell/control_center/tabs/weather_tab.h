#pragma once

#include "render/animation/animation_manager.h"
#include "render/core/render_styles.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class EffectNode;
class Flex;
class Glyph;
class InputArea;
class Label;
class Separator;
class ScrollView;
class Segmented;
class Toggle;
class WeatherService;
class ConfigService;
struct WeatherSnapshot;
struct ControlCenterServices;

class WeatherTab : public Tab {
public:
  WeatherTab(WeatherService* weather, ConfigService* config, const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void onFrameTick(float deltaMs) override;
  void setSettingsSearchQuery(std::string_view query) override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  [[nodiscard]] SettingsPane* settingsPane() override;

private:
  enum class Catalog : std::uint8_t {
    Daily = 0,
    Hourly = 1,
    Details = 2,
  };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void sync(Renderer& renderer);
  void beginForecastSlideOut(Catalog nextView);
  void beginForecastSlideIn();
  void applyForecastSlide(float progress, bool slidingIn);
  void cancelForecastSlide();
  void setForecastVisibleRowCount(std::size_t count);
  void syncDailyForecast(Renderer& renderer, const WeatherSnapshot& snapshot);
  void syncHourlyForecast(Renderer& renderer, const WeatherSnapshot& snapshot);
  void showLocationPrompt(bool show);
  void hideEffect();
  void applyCatalog(std::size_t index, bool persist = true);
  void persistCatalog();
  void syncCatalog();
  [[nodiscard]] bool syncDetailLabel();
  [[nodiscard]] static Catalog catalogFromState(std::string_view value) noexcept;
  [[nodiscard]] static std::string_view catalogState(Catalog catalog) noexcept;
  [[nodiscard]] static std::size_t indexFromCatalog(Catalog catalog) noexcept;
  [[nodiscard]] static Catalog catalogFromIndex(std::size_t index) noexcept;
  [[nodiscard]] static bool isForecastCatalog(Catalog catalog) noexcept;
  [[nodiscard]] static std::string todayIso(std::int32_t utcOffsetSeconds);
  [[nodiscard]] static std::string hourLabel(const std::string& isoTime, const std::string& timeFormat);
  [[nodiscard]] static std::string weekdayLabel(const std::string& isoDate);
  [[nodiscard]] static EffectType effectForWeatherCode(std::int32_t code, bool isDay);

  static constexpr std::size_t kForecastRowCount = 7;

  WeatherService* m_weather = nullptr;
  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Flex* m_rootLayout = nullptr;
  Flex* m_compactCard = nullptr;
  Segmented* m_catalogSwitch = nullptr;
  Label* m_detailLabel = nullptr;
  Flex* m_enableRow = nullptr;
  Toggle* m_enableToggle = nullptr;
  Flex* m_currentCard = nullptr;
  Flex* m_glyphColumn = nullptr;
  Flex* m_detailsHost = nullptr;
  Flex* m_detailsCard = nullptr;
  Flex* m_currentText = nullptr;
  Flex* m_locationPrompt = nullptr;
  Glyph* m_locationPromptGlyph = nullptr;
  Label* m_locationPromptBody = nullptr;
  Flex* m_forecastHost = nullptr;
  Flex* m_forecastRowsContainer = nullptr;
  Label* m_statusLabel = nullptr;
  Glyph* m_currentGlyph = nullptr;
  Label* m_currentTempLabel = nullptr;
  Label* m_currentHiLoLabel = nullptr;
  Label* m_currentDescLabel = nullptr;
  Label* m_updatedLabel = nullptr;
  Label* m_windLabel = nullptr;
  Label* m_sunriseLabel = nullptr;
  Label* m_sunsetLabel = nullptr;
  Label* m_tempMaxLabel = nullptr;
  Label* m_tempMinLabel = nullptr;
  Label* m_feelsLikeLabel = nullptr;
  Label* m_elevationLabel = nullptr;
  Label* m_timeZoneLabel = nullptr;
  Flex* m_timeZoneRow = nullptr;
  Label* m_uvIndexLabel = nullptr;
  std::array<Flex*, kForecastRowCount> m_forecastRows{};
  std::array<Separator*, kForecastRowCount - 1> m_forecastSeparators{};
  std::array<Flex*, kForecastRowCount> m_forecastIconSlots{};
  std::array<Glyph*, kForecastRowCount> m_forecastGlyphs{};
  std::array<Label*, kForecastRowCount> m_forecastMetas{};
  std::array<Label*, kForecastRowCount> m_forecastDescs{};
  std::array<Label*, kForecastRowCount> m_forecastTemps{};
  std::array<InputArea*, kForecastRowCount> m_forecastHitAreas{};
  EffectNode* m_effectNode = nullptr;
  EffectType m_activeEffect = EffectType::None;
  Catalog m_catalog = Catalog::Daily;
  Catalog m_pinnedCatalog = Catalog::Daily;
  bool m_catalogSyncing = false;
  int m_forecastSlideDirection = 0;
  Catalog m_pendingCatalog = Catalog::Daily;
  bool m_startForecastSlideIn = false;
  AnimationManager::Id m_forecastSlideAnimId = 0;
  float m_forecastRowsBaseX = 0.0F;
  float m_forecastRowsBaseY = 0.0F;
  float m_shaderTime = 0.0F;
};
