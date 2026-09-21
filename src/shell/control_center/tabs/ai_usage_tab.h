#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"
#include "system/ai_usage.h"
#include "ui/signal.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class AiUsageService;
class Button;
class ConfigService;
class Flex;
class Glyph;
class Input;
class InputArea;
class Label;
class ProgressBar;
class RadioButton;
class Renderer;
class ScrollView;
class Segmented;
class Spinner;
class Toggle;
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
  static constexpr std::size_t kMaxWindows = 5;
  static constexpr std::size_t kMaxOllamaModels = 16;
  static constexpr std::size_t kMaxLibraryHits = 16;
  static constexpr std::size_t kMaxLocalRuntimes = noctalia::ai_usage::kLocalRuntimeCount;

  enum class SourceKind : std::uint8_t { Provider, Ollama };
  enum class LocalCatalog : std::uint8_t { Installed, Library };

  struct Source {
    SourceKind kind = SourceKind::Ollama;
    noctalia::ai_usage::ProviderId provider = noctalia::ai_usage::ProviderId::Claude;

    [[nodiscard]] friend bool operator==(const Source&, const Source&) = default;
  };

  struct WindowWidgets {
    Flex* row = nullptr;
    Label* name = nullptr;
    Label* caption = nullptr;
    ProgressBar* bar = nullptr;
  };

  struct ProviderWidgets {
    Flex* card = nullptr;
    Flex* windowsHost = nullptr;
    Glyph* glyph = nullptr;
    Label* name = nullptr;
    Label* plan = nullptr;
    Button* openButton = nullptr;
    Label* status = nullptr;
    std::array<WindowWidgets, kMaxWindows> windows{};
  };

  struct OllamaModelWidgets {
    Flex* row = nullptr;
    InputArea* hit = nullptr;
    RadioButton* radio = nullptr;
    Label* name = nullptr;
    Label* meta = nullptr;
    Button* remove = nullptr;
    std::string id;
  };

  struct LocalRuntimeWidgets {
    Flex* row = nullptr;
    Glyph* glyph = nullptr;
    Label* name = nullptr;
    Label* meta = nullptr;
    Button* open = nullptr;
    noctalia::ai_usage::LocalRuntimeKind kind = noctalia::ai_usage::LocalRuntimeKind::LmStudio;
  };

  struct LibraryHitWidgets {
    Flex* row = nullptr;
    InputArea* hit = nullptr;
    Label* name = nullptr;
    Label* summary = nullptr;
    Label* meta = nullptr;
    ProgressBar* bar = nullptr;
    Button* action = nullptr;
    std::string id;
  };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void onPanelCardOpacityChanged(float opacity) override;
  [[nodiscard]] static std::string sourceKey(const Source& source);
  [[nodiscard]] static std::optional<Source> sourceFromKey(std::string_view key);
  void applySource(std::size_t index);
  void persistSource();
  void applyLocalCatalog(std::size_t index);
  void persistLocalCatalog();
  void ensureLibrarySearch();
  void onLibrarySearchChanged();
  void applyLibrarySearch(bool force);
  void installLibraryHit(const LibraryHitWidgets& hit);
  void activateLibraryHit(const LibraryHitWidgets& hit);
  void removeOllamaModelRow(const OllamaModelWidgets& model);
  void clearPendingRemove();
  void syncSourceSwitch(bool showLocal, const noctalia::ai_usage::Snapshot* snapshot);
  void syncSourceSwitchTooltips(const noctalia::ai_usage::Snapshot* snapshot);
  void syncCompact(bool any, const noctalia::ai_usage::Snapshot* snapshot);
  void syncUi();
  void layoutOllamaModelHits();
  void selectOllamaModelRow(const OllamaModelWidgets& model);
  void toggleOllamaService();
  void toggleOllamaLoaded();

  AiUsageService* m_service = nullptr;
  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  Flex* m_root = nullptr;
  Flex* m_compactCard = nullptr;
  Flex* m_headlineRow = nullptr;
  Label* m_headline = nullptr;
  Button* m_compactAction = nullptr;
  Label* m_caption = nullptr;
  Segmented* m_sourceSwitch = nullptr;
  Segmented* m_localCatalogSwitch = nullptr;
  Flex* m_librarySearchHost = nullptr;
  Input* m_librarySearch = nullptr;
  Spinner* m_librarySpinner = nullptr;
  ScrollView* m_scroll = nullptr;
  Flex* m_list = nullptr;
  Flex* m_ollamaCard = nullptr;
  Flex* m_ollamaModelsHost = nullptr;
  Flex* m_libraryCard = nullptr;
  Flex* m_libraryHitsHost = nullptr;
  Label* m_libraryStatus = nullptr;
  Flex* m_localRuntimesHost = nullptr;
  Label* m_localTitle = nullptr;
  Label* m_ollamaStatus = nullptr;
  Label* m_ollamaVersion = nullptr;
  Button* m_ollamaOpen = nullptr;
  Flex* m_ollamaToggleRow = nullptr;
  InputArea* m_ollamaToggleHit = nullptr;
  Toggle* m_ollamaToggle = nullptr;
  Flex* m_ollamaLoadedRow = nullptr;
  InputArea* m_ollamaLoadedHit = nullptr;
  Toggle* m_ollamaLoadedToggle = nullptr;
  Label* m_ollamaLoadedMeta = nullptr;
  std::array<OllamaModelWidgets, kMaxOllamaModels> m_ollamaModels{};
  std::array<LibraryHitWidgets, kMaxLibraryHits> m_libraryHits{};
  std::array<LocalRuntimeWidgets, kMaxLocalRuntimes> m_localRuntimes{};
  Label* m_ollamaMore = nullptr;
  Label* m_libraryMore = nullptr;
  Label* m_empty = nullptr;
  Label* m_emptyHint = nullptr;
  Label* m_updated = nullptr;
  Button* m_refreshButton = nullptr;
  std::array<ProviderWidgets, noctalia::ai_usage::kProviderCount> m_providers{};
  std::vector<Source> m_sources;
  std::optional<Source> m_source;
  LocalCatalog m_localCatalog = LocalCatalog::Installed;
  bool m_localCatalogPersisted = false;
  bool m_sourceSyncing = false;
  bool m_catalogSyncing = false;
  bool m_ollamaSyncing = false;
  std::optional<std::string> m_pendingRemove;
  Timer m_librarySearchTimer;
  Signal<>::ScopedConnection m_paletteConn;
};
