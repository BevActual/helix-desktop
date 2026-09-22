#include "shell/quick_insert/quick_insert_panel.h"

#include "config/config_service.h"
#include "core/files/directory_scanner.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_manager.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace control_center;

namespace {

  constexpr std::size_t kRecentFileLimit = 24;
  constexpr float kEmojiCell = 48.0F;
  constexpr float kRecentCell = 112.0F;
  constexpr float kClipboardRow = 46.0F;
  constexpr std::size_t kGridOverscan = 3;

  [[nodiscard]] bool isDescendantOf(const Node* node, const Node* ancestor) {
    if (node == nullptr || ancestor == nullptr) {
      return false;
    }
    for (const Node* current = node; current != nullptr; current = current->parent()) {
      if (current == ancestor) {
        return true;
      }
    }
    return false;
  }

  std::string collapseWhitespace(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool lastWasSpace = true;
    for (char ch : text) {
      const bool isWhitespace = (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r');
      if (isWhitespace) {
        if (!lastWasSpace) {
          out.push_back(' ');
        }
        lastWasSpace = true;
        continue;
      }
      out.push_back(ch);
      lastWasSpace = false;
    }
    if (!out.empty() && out.back() == ' ') {
      out.pop_back();
    }
    return out;
  }

  std::string clipboardTitle(const ClipboardEntry& entry) {
    if (!entry.textPreview.empty()) {
      return collapseWhitespace(entry.textPreview);
    }
    if (entry.isImage()) {
      return i18n::tr("clipboard.entry.image");
    }
    return entry.dataMimeType.empty() ? i18n::tr("clipboard.entry.title") : entry.dataMimeType;
  }

  [[nodiscard]] std::filesystem::path screenshotOutputDirectory(const Config* config) {
    if (config == nullptr || config->shell.screenshot.directory.empty()) {
      return FileUtils::defaultPicturesDirectory();
    }
    return FileUtils::expandUserPath(config->shell.screenshot.directory);
  }

  void applySelectionFill(Flex& tile, bool selected, bool hovered, bool restFill) {
    if (selected) {
      tile.setFill(colorSpecFromRole(ColorRole::Primary));
    } else if (hovered) {
      tile.setFill(colorSpecFromRole(ColorRole::Hover));
    } else if (restFill) {
      tile.setFill(colorSpecFromRole(ColorRole::SurfaceVariant));
    } else {
      tile.clearFill();
    }
  }

  class EmojiTile final : public Flex {
  public:
    explicit EmojiTile(float scale) {
      setDirection(FlexDirection::Vertical);
      setAlign(FlexAlign::Center);
      setJustify(FlexJustify::Center);
      setRadius(Style::scaledRadiusMd(scale));
      addChild(ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeTitle * scale,
          .textAlign = TextAlign::Center,
          .baselineMode = LabelBaselineMode::Pictographic,
          .configure = [](Label& label) { label.setHitTestVisible(false); },
      }));
    }

    void bind(const std::string& emoji, bool selected, bool hovered) {
      if (m_label != nullptr) {
        m_label->setText(emoji);
      }
      applySelectionFill(*this, selected, hovered, false);
    }

  private:
    Label* m_label = nullptr;
  };

  class RecentsTile final : public Flex {
  public:
    explicit RecentsTile(float scale) {
      setDirection(FlexDirection::Vertical);
      setAlign(FlexAlign::Stretch);
      setJustify(FlexJustify::Center);
      setRadius(Style::scaledRadiusMd(scale));
      setClipChildren(true);
      addChild(ui::image({
          .out = &m_image,
          .fit = ImageFit::Cover,
          .radius = Style::scaledRadiusMd(scale),
          .visible = false,
          .participatesInLayout = false,
          .configure = [](Image& image) { image.setHitTestVisible(false); },
      }));
      addChild(ui::glyph({
          .out = &m_glyph,
          .glyph = "photo",
          .glyphSize = Style::fontSizeTitle * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .configure = [](Glyph& glyph) { glyph.setHitTestVisible(false); },
      }));
    }

    void bind(Renderer* renderer, const std::string& path, ThumbnailService* thumbnails, bool selected, bool hovered) {
      applySelectionFill(*this, selected, hovered, true);
      const float size = std::min(width(), height());
      if (m_image != nullptr) {
        m_image->setSize(size, size);
      }
      if (thumbnails == nullptr || renderer == nullptr || path.empty()) {
        if (m_image != nullptr) {
          m_image->setVisible(false);
        }
        if (m_glyph != nullptr) {
          m_glyph->setVisible(true);
        }
        return;
      }
      const TextureHandle handle = thumbnails->peek(path);
      if (handle.id == 0) {
        if (m_image != nullptr) {
          m_image->clear(*renderer);
          m_image->setVisible(false);
        }
        if (m_glyph != nullptr) {
          m_glyph->setVisible(true);
        }
        return;
      }
      if (m_image != nullptr) {
        m_image->setExternalTexture(*renderer, handle);
        m_image->setVisible(true);
      }
      if (m_glyph != nullptr) {
        m_glyph->setVisible(false);
      }
    }

  private:
    Image* m_image = nullptr;
    Glyph* m_glyph = nullptr;
  };

  class ClipboardTile final : public Flex {
  public:
    explicit ClipboardTile(float scale) {
      setDirection(FlexDirection::Horizontal);
      setAlign(FlexAlign::Center);
      setGap(Style::spaceSm * scale);
      setPadding(Style::spaceXs * scale, Style::spaceSm * scale);
      setRadius(Style::scaledRadiusMd(scale));
      addChild(ui::glyph({
          .out = &m_glyph,
          .glyph = "file-text",
          .glyphSize = 20.0F * scale,
          .configure = [](Glyph& glyph) { glyph.setHitTestVisible(false); },
      }));
      auto text = ui::column({
          .align = FlexAlign::Start,
          .gap = 0.0F,
          .flexGrow = 1.0F,
      });
      text->addChild(ui::label({
          .out = &m_title,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::SemiBold,
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .baselineMode = LabelBaselineMode::TextFixedHeight,
          .configure = [](Label& label) { label.setHitTestVisible(false); },
      }));
      text->addChild(ui::label({
          .out = &m_meta,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .baselineMode = LabelBaselineMode::TextFixedHeight,
          .configure = [](Label& label) { label.setHitTestVisible(false); },
      }));
      addChild(std::move(text));
    }

    void bind(const ClipboardEntry& entry, bool selected, bool hovered, bool restFill) {
      if (m_glyph != nullptr) {
        m_glyph->setGlyph(entry.isImage() ? "photo" : "file-text");
      }
      if (m_title != nullptr) {
        m_title->setText(clipboardTitle(entry));
      }
      if (m_meta != nullptr) {
        m_meta->setText(formatTimeAgo(entry.capturedAt));
      }
      applySelectionFill(*this, selected, hovered, restFill);
      const ColorSpec fg = colorSpecFromRole(selected ? ColorRole::OnPrimary : ColorRole::OnSurface);
      if (m_title != nullptr) {
        m_title->setColor(fg);
      }
      if (m_glyph != nullptr) {
        m_glyph->setColor(selected ? fg : colorSpecFromRole(ColorRole::OnSurfaceVariant));
      }
    }

  private:
    Glyph* m_glyph = nullptr;
    Label* m_title = nullptr;
    Label* m_meta = nullptr;
  };

} // namespace

class QuickInsertEmojiAdapter final : public VirtualGridAdapter {
public:
  explicit QuickInsertEmojiAdapter(float scale) : m_scale(scale) {}

  void setItems(const std::vector<LauncherResult>* items) { m_items = items; }
  void setOnActivate(std::function<void(std::size_t)> callback) { m_onActivate = std::move(callback); }

  [[nodiscard]] std::size_t itemCount() const override { return m_items == nullptr ? 0 : m_items->size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override { return std::make_unique<EmojiTile>(m_scale); }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_items == nullptr || index >= m_items->size()) {
      return;
    }
    static_cast<EmojiTile&>(tile).bind((*m_items)[index].badge, selected, hovered);
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

  [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
    if (m_items == nullptr || index >= m_items->size()) {
      return {};
    }
    return (*m_items)[index].title;
  }

private:
  float m_scale = 1.0F;
  const std::vector<LauncherResult>* m_items = nullptr;
  std::function<void(std::size_t)> m_onActivate;
};

class QuickInsertRecentsAdapter final : public VirtualGridAdapter {
public:
  QuickInsertRecentsAdapter(float scale, ThumbnailService* thumbnails)
      : m_scale(scale), m_thumbnails(thumbnails) {}

  void setPaths(const std::vector<std::string>* paths) { m_paths = paths; }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  void setOnActivate(std::function<void(std::size_t)> callback) { m_onActivate = std::move(callback); }

  [[nodiscard]] std::size_t itemCount() const override { return m_paths == nullptr ? 0 : m_paths->size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override { return std::make_unique<RecentsTile>(m_scale); }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_paths == nullptr || index >= m_paths->size()) {
      return;
    }
    static_cast<RecentsTile&>(tile).bind(m_renderer, (*m_paths)[index], m_thumbnails, selected, hovered);
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

  [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
    if (m_paths == nullptr || index >= m_paths->size()) {
      return {};
    }
    return std::filesystem::path((*m_paths)[index]).filename().string();
  }

private:
  float m_scale = 1.0F;
  ThumbnailService* m_thumbnails = nullptr;
  Renderer* m_renderer = nullptr;
  const std::vector<std::string>* m_paths = nullptr;
  std::function<void(std::size_t)> m_onActivate;
};

class QuickInsertClipboardAdapter final : public VirtualGridAdapter {
public:
  QuickInsertClipboardAdapter(float scale, ClipboardService* clipboard, bool restFill)
      : m_scale(scale), m_clipboard(clipboard), m_restFill(restFill) {}

  void setIndices(const std::vector<std::size_t>* indices) { m_indices = indices; }
  void setOnActivate(std::function<void(std::size_t)> callback) { m_onActivate = std::move(callback); }

  [[nodiscard]] std::size_t itemCount() const override { return m_indices == nullptr ? 0 : m_indices->size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override { return std::make_unique<ClipboardTile>(m_scale); }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_clipboard == nullptr || m_indices == nullptr || index >= m_indices->size()) {
      return;
    }
    const std::size_t historyIndex = (*m_indices)[index];
    if (historyIndex >= m_clipboard->history().size()) {
      return;
    }
    static_cast<ClipboardTile&>(tile).bind(m_clipboard->history()[historyIndex], selected, hovered, m_restFill);
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

private:
  float m_scale = 1.0F;
  ClipboardService* m_clipboard = nullptr;
  bool m_restFill = false;
  const std::vector<std::size_t>* m_indices = nullptr;
  std::function<void(std::size_t)> m_onActivate;
};

QuickInsertPanel::QuickInsertPanel(
    ConfigService* config, ClipboardService* clipboard, ThumbnailService* thumbnails
)
    : m_config(config), m_clipboard(clipboard), m_thumbnails(thumbnails), m_emoji(clipboard) {
  m_emoji.initialize();
}

QuickInsertPanel::~QuickInsertPanel() {
  if (m_thumbnails != nullptr) {
    for (const auto& path : m_recentPaths) {
      m_thumbnails->release(path);
    }
  }
}

QuickInsertPanel::Source QuickInsertPanel::sourceFromContext(std::string_view context, bool& usedAsQuery) {
  usedAsQuery = false;
  if (context.empty() || context == "emoji" || context == "emo") {
    return Source::Emoji;
  }
  if (context == "screenshots" || context == "screenshot" || context == "recents") {
    return Source::Recents;
  }
  if (context == "clipboard" || context == "clip") {
    return Source::Clipboard;
  }
  usedAsQuery = true;
  return Source::Emoji;
}

std::size_t QuickInsertPanel::indexFromSource(Source source) { return static_cast<std::size_t>(source); }

QuickInsertPanel::Source QuickInsertPanel::sourceFromIndex(std::size_t index) {
  if (index == indexFromSource(Source::Recents)) {
    return Source::Recents;
  }
  if (index == indexFromSource(Source::Clipboard)) {
    return Source::Clipboard;
  }
  return Source::Emoji;
}

PanelPlacement QuickInsertPanel::panelPlacement() const noexcept {
  return m_config != nullptr ? m_config->config().shell.panel.quickInsertPlacement : PanelPlacement::Floating;
}

bool QuickInsertPanel::isContextActive(std::string_view context) const {
  if (context.empty()) {
    return false;
  }
  bool usedAsQuery = false;
  const Source source = sourceFromContext(context, usedAsQuery);
  return !usedAsQuery && source == m_source;
}

InputArea* QuickInsertPanel::initialFocusArea() const { return m_input != nullptr ? m_input->inputArea() : nullptr; }

void QuickInsertPanel::create() {
  const float scale = contentScale();
  const float opacity = panelCardOpacity();
  const bool restFill = m_config != nullptr && m_config->config().shell.panel.listItemBackground;

  auto container = ui::column({
      .out = &m_container,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });

  auto compactCard = ui::column({
      .out = &m_compactCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .configure = [scale, opacity](Flex& card) { applyCompactCardStyle(card, scale, opacity, false); },
  });
  compactCard->addChild(ui::input({
      .out = &m_input,
      .placeholder = i18n::tr("quick-insert.search-placeholder"),
      .fontSize = Style::fontSizeBody * scale,
      .controlHeight = Style::controlHeight * scale,
      .horizontalPadding = Style::spaceMd * scale,
      .clearButtonEnabled = true,
      .lineEditing = true,
      .surfaceOpacity = opacity,
      .onChange = [this](const std::string& text) { applyQuery(text); },
      .onSubmit = [this](const std::string&) { activateSelected(); },
      .onKeyEvent = [this](std::uint32_t sym, std::uint32_t modifiers) { return handleKeyEvent(sym, modifiers); },
  }));

  auto chips = ui::row({
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceXs * scale,
  });
  std::vector<ui::SegmentedOption> sourceOptions = {
      {.label = i18n::tr("quick-insert.source-emoji"), .glyph = "mood-smile"},
      {.label = i18n::tr("quick-insert.source-recents"), .glyph = "photo"},
      {.label = i18n::tr("quick-insert.source-clipboard"), .glyph = "clipboard"},
  };
  chips->addChild(ui::segmented({
      .out = &m_sourceSwitch,
      .options = std::move(sourceOptions),
      .selectedIndex = indexFromSource(m_source),
      .scale = scale,
      .compact = true,
      .surfaceOpacity = opacity,
      .equalSegmentWidths = false,
      .onChange = [this](std::size_t index) { applySource(sourceFromIndex(index)); },
      .configure =
          [](Segmented& sources) {
            sources.setWrap(true);
            sources.setFillWidth(false);
          },
  }));
  compactCard->addChild(std::move(chips));
  container->addChild(std::move(compactCard));

  auto leftover = ui::column({
      .out = &m_leftover,
      .align = FlexAlign::Stretch,
      .flexGrow = 1.0F,
      .configure = [scale](Flex& well) { applyContentCardStyle(well, scale); },
  });

  m_emojiAdapter = std::make_unique<QuickInsertEmojiAdapter>(scale);
  m_emojiAdapter->setItems(&m_emojiResults);
  m_emojiAdapter->setOnActivate([this](std::size_t index) { activateAt(index); });

  m_recentsAdapter = std::make_unique<QuickInsertRecentsAdapter>(scale, m_thumbnails);
  m_recentsAdapter->setPaths(&m_recentPaths);
  m_recentsAdapter->setOnActivate([this](std::size_t index) { activateAt(index); });

  m_clipboardAdapter = std::make_unique<QuickInsertClipboardAdapter>(scale, m_clipboard, restFill);
  m_clipboardAdapter->setIndices(&m_clipboardIndices);
  m_clipboardAdapter->setOnActivate([this](std::size_t index) { activateAt(index); });

  leftover->addChild(ui::virtualGridView({
      .out = &m_emojiGrid,
      .contentScale = scale,
      .columns = 0,
      .minCellWidth = kEmojiCell * scale,
      .squareCells = true,
      .columnGap = Style::spaceXs * scale,
      .rowGap = Style::spaceXs * scale,
      .overscanRows = kGridOverscan,
      .itemCursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
      .scrollbarVisible = true,
      .adapter = m_emojiAdapter.get(),
      .flexGrow = 1.0F,
      .onSelectionChanged =
          [this](std::optional<std::size_t> idx) {
            if (idx.has_value()) {
              m_selectedIndex = *idx;
            }
          },
  }));
  leftover->addChild(ui::virtualGridView({
      .out = &m_recentsGrid,
      .contentScale = scale,
      .columns = 0,
      .minCellWidth = kRecentCell * scale,
      .squareCells = true,
      .columnGap = Style::spaceXs * scale,
      .rowGap = Style::spaceXs * scale,
      .overscanRows = kGridOverscan,
      .itemCursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
      .scrollbarVisible = true,
      .adapter = m_recentsAdapter.get(),
      .flexGrow = 1.0F,
      .visible = false,
      .participatesInLayout = false,
      .onSelectionChanged =
          [this](std::optional<std::size_t> idx) {
            if (idx.has_value()) {
              m_selectedIndex = *idx;
            }
          },
  }));
  leftover->addChild(ui::virtualGridView({
      .out = &m_clipboardGrid,
      .contentScale = scale,
      .columns = 1,
      .cellHeight = kClipboardRow * scale,
      .squareCells = false,
      .columnGap = 0.0F,
      .rowGap = Style::spaceXs * scale,
      .overscanRows = kGridOverscan,
      .itemCursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
      .scrollbarVisible = true,
      .adapter = m_clipboardAdapter.get(),
      .flexGrow = 1.0F,
      .visible = false,
      .participatesInLayout = false,
      .onSelectionChanged =
          [this](std::optional<std::size_t> idx) {
            if (idx.has_value()) {
              m_selectedIndex = *idx;
            }
          },
  }));
  leftover->addChild(ui::label({
      .out = &m_emptyLabel,
      .fontSize = Style::fontSizeCaption * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .visible = false,
      .participatesInLayout = false,
  }));
  container->addChild(std::move(leftover));
  setRoot(std::move(container));
  if (m_animations != nullptr && root() != nullptr) {
    root()->setAnimationManager(m_animations);
  }
}

void QuickInsertPanel::onOpen(std::string_view context) {
  bool usedAsQuery = false;
  const Source source = sourceFromContext(context, usedAsQuery);
  const std::string query = usedAsQuery ? std::string(context) : std::string();
  if (m_input != nullptr) {
    m_input->setValue(query);
  }
  m_query = query;
  if (m_thumbnails != nullptr) {
    m_thumbnailSub.disconnect();
    m_thumbnailSub = m_thumbnails->subscribePendingUpload([this]() {
      m_thumbnailRefreshPending = true;
      PanelManager::instance().requestUpdateOnly();
    });
  }
  if (m_sourceSwitch != nullptr && m_sourceSwitch->selectedIndex() != indexFromSource(source)) {
    m_sourceSwitch->setSelectedIndex(indexFromSource(source));
  } else {
    applySource(source);
  }
}

void QuickInsertPanel::onClose() {
  m_thumbnailSub.disconnect();
  if (m_thumbnails != nullptr) {
    for (const auto& path : m_recentPaths) {
      m_thumbnails->release(path);
    }
  }
  m_recentPaths.clear();
  m_emojiResults.clear();
  m_clipboardIndices.clear();
  m_query.clear();
  m_selectedIndex = 0;
  m_clipboardSerial = 0;
  if (m_input != nullptr) {
    m_input->setValue({});
  }
}

void QuickInsertPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_container == nullptr) {
    return;
  }
  if (m_recentsAdapter != nullptr) {
    m_recentsAdapter->setRenderer(&renderer);
  }
  syncGridMetrics();
  m_container->setSize(width, height);
  m_container->layout(renderer);
}

void QuickInsertPanel::doUpdate(Renderer& renderer) {
  if (m_recentsAdapter != nullptr) {
    m_recentsAdapter->setRenderer(&renderer);
  }
  if (m_source == Source::Clipboard && m_clipboard != nullptr && m_clipboard->changeSerial() != m_clipboardSerial) {
    refreshClipboard();
  }
  if (m_thumbnails != nullptr) {
    (void)m_thumbnails->uploadPending(renderer.textureManager());
    if (m_thumbnailRefreshPending && m_recentsGrid != nullptr) {
      m_recentsGrid->notifyDataChanged();
      m_thumbnailRefreshPending = false;
    }
  }
}

void QuickInsertPanel::onPanelCardOpacityChanged(float opacity) {
  if (m_compactCard != nullptr) {
    applyCompactCardStyle(*m_compactCard, contentScale(), opacity, false);
  }
  if (m_input != nullptr) {
    m_input->setSurfaceOpacity(opacity);
  }
  if (m_sourceSwitch != nullptr) {
    m_sourceSwitch->setSurfaceOpacity(opacity);
  }
}

void QuickInsertPanel::applySource(Source source) {
  m_source = source;
  m_selectedIndex = 0;
  updatePlaceholder();
  updateSourceVisibility();
  refreshResults();
  if (VirtualGridView* grid = activeGrid(); grid != nullptr) {
    grid->scrollView().setScrollOffset(0.0F);
    if (activeCount() > 0) {
      grid->setSelectedIndex(0);
    }
  }
  PanelManager::instance().requestLayout();
}

void QuickInsertPanel::cycleSource(bool reverse) {
  const int current = static_cast<int>(indexFromSource(m_source));
  const int next = reverse ? (current == 0 ? 2 : current - 1) : (current == 2 ? 0 : current + 1);
  if (m_sourceSwitch != nullptr) {
    m_sourceSwitch->setSelectedIndex(static_cast<std::size_t>(next));
    return;
  }
  applySource(sourceFromIndex(static_cast<std::size_t>(next)));
}

void QuickInsertPanel::applyQuery(const std::string& text) {
  m_query = text;
  m_selectedIndex = 0;
  refreshResults();
}

void QuickInsertPanel::refreshResults() {
  switch (m_source) {
  case Source::Emoji:
    m_emojiResults = m_emoji.query(m_query);
    if (m_emojiGrid != nullptr) {
      m_emojiGrid->notifyDataChanged();
    }
    break;
  case Source::Recents:
    refreshRecents();
    break;
  case Source::Clipboard:
    refreshClipboard();
    break;
  }
  updateEmptyState();
  if (VirtualGridView* grid = activeGrid(); grid != nullptr && activeCount() > 0) {
    m_selectedIndex = std::min(m_selectedIndex, activeCount() - 1);
    grid->setSelectedIndex(m_selectedIndex);
  }
}

void QuickInsertPanel::refreshRecents() {
  if (m_thumbnails != nullptr) {
    for (const auto& path : m_recentPaths) {
      m_thumbnails->release(path);
    }
  }
  m_recentPaths.clear();

  DirectoryScanner scanner;
  const auto entries = scanner.scan(
      screenshotOutputDirectory(m_config != nullptr ? &m_config->config() : nullptr), {".png"}, false,
      FileDialogSortField::Modified, FileDialogSortOrder::Descending
  );
  const std::string query = StringUtils::toLower(m_query);
  for (const auto& entry : entries) {
    if (entry.isDir) {
      continue;
    }
    if (!query.empty() && !StringUtils::toLower(entry.name).contains(query)) {
      continue;
    }
    m_recentPaths.push_back(entry.absPath.string());
    if (m_thumbnails != nullptr) {
      (void)m_thumbnails->acquire(m_recentPaths.back());
    }
    if (m_recentPaths.size() >= kRecentFileLimit) {
      break;
    }
  }
  m_thumbnailRefreshPending = !m_recentPaths.empty();
  if (m_recentsGrid != nullptr) {
    m_recentsGrid->notifyDataChanged();
  }
}

void QuickInsertPanel::refreshClipboard() {
  m_clipboardIndices.clear();
  if (m_clipboard == nullptr) {
    if (m_clipboardGrid != nullptr) {
      m_clipboardGrid->notifyDataChanged();
    }
    return;
  }
  m_clipboardSerial = m_clipboard->changeSerial();
  const std::string query = StringUtils::toLower(m_query);
  const auto& history = m_clipboard->history();
  for (std::size_t i = 0; i < history.size(); ++i) {
    if (!query.empty()) {
      const std::string haystack = StringUtils::toLower(clipboardTitle(history[i]));
      if (!haystack.contains(query)) {
        continue;
      }
    }
    m_clipboardIndices.push_back(i);
  }
  if (m_clipboardGrid != nullptr) {
    m_clipboardGrid->notifyDataChanged();
  }
}

void QuickInsertPanel::updateSourceVisibility() {
  const auto show = [](VirtualGridView* grid, bool visible) {
    if (grid == nullptr) {
      return;
    }
    grid->setVisible(visible);
    grid->setParticipatesInLayout(visible);
  };
  show(m_emojiGrid, m_source == Source::Emoji);
  show(m_recentsGrid, m_source == Source::Recents);
  show(m_clipboardGrid, m_source == Source::Clipboard);
}

void QuickInsertPanel::updatePlaceholder() {
  if (m_input == nullptr) {
    return;
  }
  switch (m_source) {
  case Source::Recents:
    m_input->setPlaceholder(i18n::tr("quick-insert.search-recents"));
    break;
  case Source::Clipboard:
    m_input->setPlaceholder(i18n::tr("quick-insert.search-clipboard"));
    break;
  case Source::Emoji:
    m_input->setPlaceholder(i18n::tr("quick-insert.search-emoji"));
    break;
  }
}

void QuickInsertPanel::updateEmptyState() {
  const bool empty = activeCount() == 0;
  if (m_emptyLabel != nullptr) {
    switch (m_source) {
    case Source::Recents:
      m_emptyLabel->setText(i18n::tr("quick-insert.empty-recents"));
      break;
    case Source::Clipboard:
      m_emptyLabel->setText(i18n::tr("quick-insert.empty-clipboard"));
      break;
    case Source::Emoji:
      m_emptyLabel->setText(i18n::tr("quick-insert.empty-emoji"));
      break;
    }
    m_emptyLabel->setVisible(empty);
    m_emptyLabel->setParticipatesInLayout(empty);
  }
  if (VirtualGridView* grid = activeGrid(); grid != nullptr) {
    grid->setVisible(!empty);
    grid->setParticipatesInLayout(!empty);
  }
}

void QuickInsertPanel::syncGridMetrics() {
  const float scale = contentScale();
  if (m_emojiGrid != nullptr) {
    m_emojiGrid->setScale(scale);
    m_emojiGrid->setMinCellWidth(kEmojiCell * scale);
  }
  if (m_recentsGrid != nullptr) {
    m_recentsGrid->setScale(scale);
    m_recentsGrid->setMinCellWidth(kRecentCell * scale);
  }
  if (m_clipboardGrid != nullptr) {
    m_clipboardGrid->setScale(scale);
    m_clipboardGrid->setCellHeight(kClipboardRow * scale);
  }
}

VirtualGridView* QuickInsertPanel::activeGrid() const noexcept {
  switch (m_source) {
  case Source::Recents:
    return m_recentsGrid;
  case Source::Clipboard:
    return m_clipboardGrid;
  case Source::Emoji:
    break;
  }
  return m_emojiGrid;
}

std::size_t QuickInsertPanel::activeCount() const noexcept {
  switch (m_source) {
  case Source::Recents:
    return m_recentPaths.size();
  case Source::Clipboard:
    return m_clipboardIndices.size();
  case Source::Emoji:
    break;
  }
  return m_emojiResults.size();
}

void QuickInsertPanel::activateSelected() { activateAt(m_selectedIndex); }

void QuickInsertPanel::activateAt(std::size_t index) {
  switch (m_source) {
  case Source::Emoji:
    if (index < m_emojiResults.size()) {
      insertEmoji(m_emojiResults[index].badge);
    }
    break;
  case Source::Recents:
    if (index < m_recentPaths.size()) {
      insertRecent(m_recentPaths[index]);
    }
    break;
  case Source::Clipboard:
    if (index < m_clipboardIndices.size()) {
      insertClipboard(m_clipboardIndices[index]);
    }
    break;
  }
}

void QuickInsertPanel::insertEmoji(const std::string& emoji) {
  const bool copied = m_clipboard != nullptr && m_clipboard->copyText(emoji);
  finishInsert(copied, false);
}

void QuickInsertPanel::insertRecent(const std::string& path) {
  if (m_clipboard == nullptr) {
    return;
  }
  auto png = FileUtils::readBinaryFile(path);
  const bool copied = !png.empty() && m_clipboard->copyImagePng(std::move(png));
  finishInsert(copied, true);
}

void QuickInsertPanel::insertClipboard(std::size_t historyIndex) {
  if (m_clipboard == nullptr) {
    return;
  }
  if (!m_clipboard->ensureEntryLoaded(historyIndex)) {
    return;
  }
  const ClipboardEntry entry = m_clipboard->history()[historyIndex];
  if (!entry.pinned) {
    (void)m_clipboard->promoteEntry(historyIndex);
  }
  const bool copied = m_clipboard->copyEntry(entry);
  finishInsert(copied, entry.isImage());
}

void QuickInsertPanel::finishInsert(bool copied, bool isImage) {
  if (!copied) {
    return;
  }
  PanelManager::instance().closePanel(false);
  if (m_onCopiedActivation) {
    m_onCopiedActivation(isImage);
  }
}

bool QuickInsertPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!pressed || preedit) {
    return false;
  }
  auto& dispatcher = PanelManager::instance().inputDispatcher();
  InputArea* const focused = dispatcher.focusedArea();
  if (focused != nullptr) {
    const bool onInput = (m_input != nullptr && focused == m_input->inputArea());
    const bool onSources = (m_sourceSwitch != nullptr && focused == m_sourceSwitch->focusArea());
    VirtualGridView* grid = activeGrid();
    const bool inResults = (grid != nullptr && isDescendantOf(focused, grid));
    if (!onInput && !onSources && !inResults) {
      return false;
    }
    if (onSources) {
      return false;
    }
  }
  return handleKeyEvent(sym, modifiers);
}

bool QuickInsertPanel::handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers) {
  if (KeybindMatcher::matches(KeybindAction::TabNext, sym, modifiers)) {
    cycleSource(false);
    return true;
  }
  if (KeybindMatcher::matches(KeybindAction::TabPrevious, sym, modifiers)) {
    cycleSource(true);
    return true;
  }

  VirtualGridView* grid = activeGrid();
  const std::size_t count = activeCount();
  if (count == 0 || grid == nullptr) {
    return false;
  }
  const bool gridNav = m_source != Source::Clipboard;
  const int columns = gridNav ? static_cast<int>(std::max<std::size_t>(1, grid->layoutColumnCount())) : 1;

  const auto moveSelection = [this, grid, count](int delta) {
    const int last = static_cast<int>(count - 1);
    const int next = std::clamp(static_cast<int>(m_selectedIndex) + delta, 0, last);
    m_selectedIndex = static_cast<std::size_t>(next);
    grid->setSelectedIndex(m_selectedIndex);
    grid->scrollToIndex(m_selectedIndex);
  };

  if (KeySymbol::isPageUp(sym)) {
    moveSelection(-static_cast<int>(grid->pageItemStride()));
    return true;
  }
  if (KeySymbol::isPageDown(sym)) {
    moveSelection(static_cast<int>(grid->pageItemStride()));
    return true;
  }
  if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    moveSelection(gridNav ? -columns : -1);
    return true;
  }
  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    moveSelection(gridNav ? columns : 1);
    return true;
  }
  if (gridNav && KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    moveSelection(-1);
    return true;
  }
  if (gridNav && KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    moveSelection(1);
    return true;
  }
  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    activateSelected();
    return true;
  }
  return false;
}
