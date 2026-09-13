#pragma once

#include "core/timer_manager.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class AnimationManager;
class Button;
class AsyncTextureCache;
class ClipboardService;
class Flex;
class Image;
class Input;
class InputArea;
class Label;
class Renderer;
class ScrollView;
class ConfigService;
class ClipboardListAdapter;
class VirtualGridView;
struct ClipboardEntry;

class ClipboardPicker {
public:
  struct Options {
    bool showTitle = true;
    bool showCloseButton = true;
    bool closeHostWhenEmpty = true;
  };

  ClipboardPicker(ClipboardService* clipboard, ConfigService* config, AsyncTextureCache* asyncTextures, Options options);
  ~ClipboardPicker();

  ClipboardPicker(const ClipboardPicker&) = delete;
  ClipboardPicker& operator=(const ClipboardPicker&) = delete;

  void setActivateCallback(std::function<void(const ClipboardEntry&)> callback);
  void setCloseCallback(std::function<void()> callback);

  void setContentScale(float scale) noexcept { m_contentScale = scale; }
  void setPanelCardOpacity(float opacity);
  void setAnimationManager(AnimationManager* mgr) noexcept { m_animations = mgr; }

  [[nodiscard]] std::unique_ptr<Flex> create();
  void onOpen(std::string_view context);
  void onClose();
  void layout(Renderer& renderer, float width, float height);
  void update(Renderer& renderer);
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit);
  [[nodiscard]] InputArea* initialFocusArea() const;

private:
  [[nodiscard]] float contentScale() const noexcept { return m_contentScale; }
  [[nodiscard]] float panelCardOpacity() const noexcept { return m_panelCardOpacity; }
  void requestHostClose();
  void schedulePreviewPayloadRefresh(bool debounced);
  void updateListState();
  void updatePreviewActions();
  void rebuildPreview(Renderer& renderer, float width, float height);
  void selectIndex(std::size_t index);
  void selectByStorageId(std::string storageId);
  void activateSelected();
  void togglePinSelected();
  void runImageAction();
  void requestClearUnpinnedHistory();
  void clearUnpinnedHistory();
  void clearAllHistory();
  void performClearUnpinnedHistory();
  void performClearAllHistory();
  bool handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers);
  void scrollToSelected();
  void requestDeleteSelectedEntry();
  void deleteSelectedEntry();
  void performDeleteSelectedEntry();
  void resetDeleteConfirmation();
  void resetClearConfirmation();
  void applyFilter();
  void onFilterChanged(const std::string& text);
  [[nodiscard]] std::size_t selectedHistoryIndex() const;

  Options m_options;
  ClipboardService* m_clipboard = nullptr;
  std::function<void(const ClipboardEntry&)> m_activateCallback;
  std::function<void()> m_closeCallback;
  ConfigService* m_config = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;
  AnimationManager* m_animations = nullptr;
  float m_contentScale = 1.0F;
  float m_panelCardOpacity = 1.0F;

  InputArea* m_focusArea = nullptr;
  Flex* m_rootLayout = nullptr;
  Flex* m_sidebar = nullptr;
  Flex* m_sidebarHeaderRow = nullptr;
  Label* m_sidebarTitle = nullptr;
  Button* m_clearHistoryButton = nullptr;
  Button* m_clearKeepPinnedButton = nullptr;
  Flex* m_clearConfirmPanel = nullptr;
  Label* m_clearConfirmDesc = nullptr;
  Button* m_closeButton = nullptr;
  Input* m_filterInput = nullptr;
  VirtualGridView* m_listGrid = nullptr;
  Label* m_listEmptyLabel = nullptr;
  std::unique_ptr<ClipboardListAdapter> m_listAdapter;
  std::vector<std::size_t> m_filteredIndices;
  std::string m_filterQuery;

  Flex* m_previewCard = nullptr;
  Flex* m_previewHeaderRow = nullptr;
  Label* m_previewTitle = nullptr;
  Label* m_previewMeta = nullptr;
  Button* m_imageActionButton = nullptr;
  Button* m_pinButton = nullptr;
  Button* m_copyButton = nullptr;
  Button* m_deleteEntryButton = nullptr;
  Flex* m_deleteConfirmPanel = nullptr;
  ScrollView* m_previewScrollView = nullptr;
  Flex* m_previewContent = nullptr;
  Image* m_previewImage = nullptr;

  std::size_t m_selectedIndex = 0;
  std::size_t m_previewPayloadIndex = static_cast<std::size_t>(-1);
  std::size_t m_pendingPreviewPayloadIndex = static_cast<std::size_t>(-1);
  Timer m_previewPayloadDebounceTimer;
  Timer m_filterDebounceTimer;
  std::string m_pendingFilterQuery;
  std::string m_deleteConfirmStorageId;
  std::uint64_t m_lastChangeSerial = 0;
  bool m_clearConfirm = false;
  float m_lastWidth = 0.0F;
  float m_lastHeight = 0.0F;
  float m_lastPreviewWidth = -1.0F;
  float m_lastPreviewHeight = -1.0F;
  float m_listRowHeight = 0.0F;
  bool m_pendingScrollToSelected = false;
};
