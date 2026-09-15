#pragma once

#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "system/ai_usage.h"
#include "ui/signal.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>

class AiUsageService;
class Button;
class Flex;
class Glyph;
class Label;
class ProgressBar;
class Renderer;
class ScrollView;
struct ControlCenterServices;

class AiUsageTab : public Tab {
public:
  explicit AiUsageTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  void setActive(bool active) override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;

private:
  static constexpr std::size_t kMaxWindows = 2;
  static constexpr std::size_t kMaxOllamaModels = 8;

  struct WindowWidgets {
    Flex* row = nullptr;
    Label* name = nullptr;
    Label* caption = nullptr;
    ProgressBar* bar = nullptr;
  };

  struct ProviderWidgets {
    Flex* card = nullptr;
    Glyph* glyph = nullptr;
    Label* name = nullptr;
    Label* plan = nullptr;
    Button* openButton = nullptr;
    Label* status = nullptr;
    std::array<WindowWidgets, kMaxWindows> windows{};
  };

  struct OllamaModelWidgets {
    Flex* row = nullptr;
    Label* name = nullptr;
    Label* meta = nullptr;
  };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void syncUi();

  AiUsageService* m_service = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Flex* m_root = nullptr;
  ScrollView* m_scroll = nullptr;
  Flex* m_list = nullptr;
  Flex* m_ollamaCard = nullptr;
  Label* m_ollamaStatus = nullptr;
  Label* m_ollamaVersion = nullptr;
  Button* m_ollamaStart = nullptr;
  Button* m_ollamaStop = nullptr;
  std::array<OllamaModelWidgets, kMaxOllamaModels> m_ollamaModels{};
  Label* m_ollamaMore = nullptr;
  Label* m_empty = nullptr;
  Label* m_updated = nullptr;
  Button* m_refreshButton = nullptr;
  std::array<ProviderWidgets, noctalia::ai_usage::kProviderCount> m_providers{};
  Signal<>::ScopedConnection m_paletteConn;
};
