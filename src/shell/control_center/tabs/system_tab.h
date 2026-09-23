#pragma once

#include "core/frame_rate_limiter.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Box;
class Flex;
class Glyph;
class Graph;
class Label;
class Button;
class ClipboardService;
class ConfigService;
class Segmented;
class SystemMonitorService;
struct ControlCenterServices;

class SystemTab : public Tab {
public:
  SystemTab(SystemMonitorService* monitor, const ControlCenterServices& services);
  ~SystemTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setActive(bool active) override;
  void onFrameTick(float deltaMs) override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;

private:
  enum class Catalog : std::uint8_t { Cpu, Memory, Gpu, Network, About };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;

  void retainStats(bool retain);
  void updateGraphs(Renderer& renderer);
  void syncLabels();
  void updateGpuVisibility();
  void copySystemInfo();
  void applyCatalog(std::size_t index, bool persist = true);
  void persistCatalog();
  void syncCatalog();
  [[nodiscard]] bool syncDetailLabel();
  [[nodiscard]] std::string systemInfoText() const;
  [[nodiscard]] float scrollProgressForSample(std::chrono::steady_clock::time_point sampledAt) const;
  [[nodiscard]] static Catalog catalogFromState(std::string_view value) noexcept;
  [[nodiscard]] static std::string_view catalogState(Catalog catalog) noexcept;
  [[nodiscard]] static std::size_t indexFromCatalog(Catalog catalog) noexcept;
  [[nodiscard]] static Catalog catalogFromIndex(std::size_t index) noexcept;

  SystemMonitorService* m_monitor;
  ClipboardService* m_clipboard = nullptr;
  ConfigService* m_config = nullptr;
  std::function<void()> m_upgradeShell;
  std::function<bool()> m_upgradeAvailable;
  std::function<bool()> m_upgradeBusy;
  std::unique_ptr<SettingsPane> m_settings;
  bool m_active = false;
  bool m_statsRetained = false;
  bool m_graphInitialized = false;
  bool m_gpuVisible = false;
  bool m_catalogSyncing = false;
  float m_scrollProgress = 1.0F;
  std::chrono::steady_clock::time_point m_lastSampleAt;
  FrameRateLimiter m_redrawLimiter{std::chrono::milliseconds{200}};
  Catalog m_catalog = Catalog::Cpu;
  Catalog m_pinnedCatalog = Catalog::Cpu;

  double m_cpuTempMin = 30.0;
  double m_cpuTempMax = 80.0;
  double m_gpuTempMin = 30.0;
  double m_gpuTempMax = 80.0;
  double m_netPeak = 0.0;

  Flex* m_root = nullptr;
  Flex* m_compactCard = nullptr;
  Segmented* m_catalogSwitch = nullptr;
  Label* m_titleLabel = nullptr;
  Label* m_glanceCpuLabel = nullptr;
  Label* m_glanceRamLabel = nullptr;
  Label* m_detailLabel = nullptr;

  Graph* m_cpuGraph = nullptr;
  Graph* m_ramGraph = nullptr;
  Graph* m_gpuGraph = nullptr;
  Graph* m_netGraph = nullptr;

  Flex* m_cpuHost = nullptr;
  Flex* m_ramHost = nullptr;
  Flex* m_gpuHost = nullptr;
  Flex* m_netHost = nullptr;
  Flex* m_aboutHost = nullptr;
  Flex* m_aboutColumns = nullptr;

  Flex* m_cpuLegend = nullptr;
  Flex* m_ramLegend = nullptr;
  Flex* m_gpuLegend = nullptr;
  Flex* m_netLegend = nullptr;

  Button* m_upgradeButton = nullptr;

  Glyph* m_cpuPctIcon = nullptr;
  Label* m_cpuPctLabel = nullptr;
  Glyph* m_cpuTempIcon = nullptr;
  Label* m_cpuTempLabel = nullptr;
  Flex* m_gpuTempGroup = nullptr;
  Glyph* m_gpuTempIcon = nullptr;
  Label* m_gpuTempLabel = nullptr;
  Flex* m_gpuUsageGroup = nullptr;
  Glyph* m_gpuUsageIcon = nullptr;
  Label* m_gpuUsageLabel = nullptr;
  Flex* m_gpuVramGroup = nullptr;
  Glyph* m_gpuVramIcon = nullptr;
  Label* m_gpuVramLabel = nullptr;
  Glyph* m_ramIcon = nullptr;
  Label* m_ramLabel = nullptr;
  Glyph* m_rxIcon = nullptr;
  Label* m_rxLabel = nullptr;
  Glyph* m_txIcon = nullptr;
  Label* m_txLabel = nullptr;

  static constexpr int kSystemLines = 6;
  Label* m_systemLines[kSystemLines] = {};

  static constexpr int kResourcesLines = 2;
  Label* m_resourcesLines[kResourcesLines] = {};
  Flex* m_swapRow = nullptr;
  Label* m_swapLabel = nullptr;
  std::vector<std::string> m_diskMountPoints;
  std::vector<Label*> m_diskLabels;

  // Thin used/total bars under RAM, swap, and each disk row; fill width follows the track after
  // layout, so syncUsageBars() runs from both the stats update and doLayout.
  struct UsageBar {
    Box* track = nullptr;
    Box* fill = nullptr;
    float fraction = 0.0F;
  };
  void syncUsageBars();
  UsageBar m_ramBar;
  UsageBar m_swapBar;
  std::vector<UsageBar> m_diskBars;
};
