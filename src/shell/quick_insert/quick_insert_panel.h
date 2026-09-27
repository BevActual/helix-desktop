#pragma once

#include "config/config_types.h"
#include "system/desktop_ai.h"
#include "launcher/emoji_provider.h"
#include "launcher/launcher_provider.h"
#include "render/core/thumbnail_service.h"
#include "shell/panel/panel.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class ClipboardService;
class ConfigService;
class DesktopAiService;
class Flex;
class Button;
class Input;
class InputArea;
class Label;
class Renderer;
class Segmented;
class VirtualGridView;

class QuickInsertEmojiAdapter;
class QuickInsertRecentsAdapter;
class QuickInsertClipboardAdapter;

class QuickInsertPanel : public Panel {
public:
  enum class Source : std::uint8_t { Emoji, Recents, Clipboard };

  QuickInsertPanel(
      ConfigService* config, ClipboardService* clipboard, ThumbnailService* thumbnails, DesktopAiService* desktopAi
  );
  ~QuickInsertPanel() override;

  void setCopiedActivationCallback(std::function<void(bool isImage)> callback) {
    m_onCopiedActivation = std::move(callback);
  }

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override { return scaled(520.0F); }
  [[nodiscard]] float preferredHeight() const override { return scaled(480.0F); }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;
  [[nodiscard]] bool isContextActive(std::string_view context) const override;

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;

  void applySource(Source source);
  void cycleSource(bool reverse);
  void applyQuery(const std::string& text);
  void refreshResults();
  void refreshRecents();
  void refreshClipboard();
  void updateSourceVisibility();
  void updatePlaceholder();
  void updateEmptyState();
  void syncGridMetrics();
  void activateSelected();
  void activateAt(std::size_t index);
  void insertEmoji(const std::string& emoji);
  void insertRecent(const std::string& path);
  void insertClipboard(std::size_t historyIndex);
  void finishInsert(bool copied, bool isImage);
  void syncAiActions();
  void onDesktopAiChanged();
  void requestTextTransform(DesktopAiTask task);
  void requestVision(bool describe);
  [[nodiscard]] std::string selectedInsertText();
  [[nodiscard]] std::string selectedRecentPath() const;
  bool handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers);
  [[nodiscard]] VirtualGridView* activeGrid() const noexcept;
  [[nodiscard]] std::size_t activeCount() const noexcept;
  [[nodiscard]] static Source sourceFromContext(std::string_view context, bool& usedAsQuery);
  [[nodiscard]] static std::size_t indexFromSource(Source source);
  [[nodiscard]] static Source sourceFromIndex(std::size_t index);

  ConfigService* m_config = nullptr;
  ClipboardService* m_clipboard = nullptr;
  ThumbnailService* m_thumbnails = nullptr;
  DesktopAiService* m_desktopAi = nullptr;
  bool m_pasteOnResult = false;
  EmojiProvider m_emoji;
  Source m_source = Source::Emoji;
  std::string m_query;
  std::vector<LauncherResult> m_emojiResults;
  std::vector<std::string> m_recentPaths;
  std::vector<std::size_t> m_clipboardIndices;
  std::size_t m_selectedIndex = 0;
  std::uint64_t m_clipboardSerial = 0;
  bool m_thumbnailRefreshPending = false;
  ThumbnailService::Subscription m_thumbnailSub;

  Flex* m_container = nullptr;
  Flex* m_compactCard = nullptr;
  Input* m_input = nullptr;
  Segmented* m_sourceSwitch = nullptr;
  Flex* m_leftover = nullptr;
  VirtualGridView* m_emojiGrid = nullptr;
  VirtualGridView* m_recentsGrid = nullptr;
  VirtualGridView* m_clipboardGrid = nullptr;
  Label* m_emptyLabel = nullptr;
  Flex* m_aiRow = nullptr;
  Label* m_aiStatus = nullptr;
  Button* m_summarizeButton = nullptr;
  Button* m_translateButton = nullptr;
  Button* m_rewriteButton = nullptr;
  Button* m_speakButton = nullptr;
  Button* m_dictateButton = nullptr;
  Button* m_readButton = nullptr;
  Button* m_describeButton = nullptr;

  std::unique_ptr<QuickInsertEmojiAdapter> m_emojiAdapter;
  std::unique_ptr<QuickInsertRecentsAdapter> m_recentsAdapter;
  std::unique_ptr<QuickInsertClipboardAdapter> m_clipboardAdapter;
  std::function<void(bool)> m_onCopiedActivation;
};
