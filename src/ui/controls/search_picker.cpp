#include "ui/controls/search_picker.h"

#include "core/input/keybind_matcher.h"
#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/controls/color_swatch_preview.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

namespace {

  constexpr float kDefaultWidth = 320.0F;
  constexpr float kDefaultHeight = 360.0F;

  class SearchPickerRow : public Flex {
  public:
    SearchPickerRow() {
      setDirection(FlexDirection::Horizontal);
      setAlign(FlexAlign::Center);
      setFillWidth(true);
      applyScale(1.0F);

      auto preview = std::make_unique<ColorSwatchPreviewStrip>();
      preview->setVisible(false);
      preview->setParticipatesInLayout(false);
      m_preview = static_cast<ColorSwatchPreviewStrip*>(addChild(std::move(preview)));

      addChild(
          ui::glyph({
              .out = &m_icon,
              .glyphSize = Style::baseGlyphSize,
              .visible = false,
              .participatesInLayout = false,
          })
      );

      auto text = ui::column({
          .out = &m_text,
          .align = FlexAlign::Stretch,
          .justify = FlexJustify::Center,
          .flexGrow = 1.0F,
      });
      text->addChild(
          ui::label({
              .out = &m_title,
              .fontSize = Style::fontSizeBody,
          })
      );
      text->addChild(
          ui::label({
              .out = &m_detail,
              .fontSize = Style::fontSizeCaption,
              .maxLines = 1,
              .visible = false,
          })
      );
      addChild(std::move(text));
    }

    void applyScale(float scale) {
      if (m_scale == scale && m_scaleApplied) {
        return;
      }
      m_scale = scale;
      m_scaleApplied = true;
      setGap(Style::spaceSm * scale);
      setPadding(Style::spaceXs * scale, Style::spaceSm * scale);
      setRadius(Style::scaledRadiusMd(scale));
      if (m_icon != nullptr) {
        m_icon->setGlyphSize(Style::baseGlyphSize * scale);
      }
      if (m_title != nullptr) {
        m_title->setFontSize(Style::fontSizeBody * scale);
      }
      if (m_detail != nullptr) {
        m_detail->setFontSize(Style::fontSizeCaption * scale);
      }
    }

    void bind(const SearchPickerOption& option, bool highlighted, bool selected, bool hovered) {
      const bool hasDetail = !option.description.empty();
      const bool hasIcon = !option.icon.empty();
      const bool hasPreview = !option.preview.empty();

      if (highlighted) {
        setFill(colorSpecFromRole(ColorRole::Primary));
      } else if (hovered) {
        setFill(colorSpecFromRole(ColorRole::Hover));
      } else if (selected) {
        setFill(colorSpecFromRole(ColorRole::Primary, 0.16F));
      } else {
        setFill(clearColorSpec());
      }

      ColorSpec foreground =
          option.enabled ? colorSpecFromRole(ColorRole::OnSurface) : colorSpecFromRole(ColorRole::OnSurface, 0.55F);
      ColorSpec detailForeground = colorSpecFromRole(ColorRole::OnSurfaceVariant, option.enabled ? 1.0F : 0.55F);
      if (highlighted) {
        foreground = colorSpecFromRole(ColorRole::OnPrimary);
        detailForeground = colorSpecFromRole(ColorRole::OnPrimary);
      } else if (hovered) {
        foreground = colorSpecFromRole(ColorRole::OnHover);
        detailForeground = colorSpecFromRole(ColorRole::OnHover);
      }

      if (m_preview != nullptr) {
        m_preview->setMetricsFromFontSize(Style::fontSizeBody * m_scale);
        m_preview->setPreview(option.preview);
        m_preview->setVisible(hasPreview);
        m_preview->setParticipatesInLayout(hasPreview);
      }
      if (m_icon != nullptr) {
        if (hasIcon && !hasPreview) {
          m_icon->setGlyph(option.icon);
          m_icon->setColor(foreground);
        }
        m_icon->setVisible(hasIcon && !hasPreview);
        m_icon->setParticipatesInLayout(hasIcon && !hasPreview);
      }
      if (m_title != nullptr) {
        m_title->setText(option.label);
        m_title->setFontWeight(hasDetail ? FontWeight::Bold : FontWeight::Normal);
        m_title->setColor(foreground);
      }
      if (m_detail != nullptr) {
        m_detail->setVisible(hasDetail);
        m_detail->setParticipatesInLayout(hasDetail);
        m_detail->setText(option.description);
        m_detail->setColor(detailForeground);
      }
    }

  private:
    ColorSwatchPreviewStrip* m_preview = nullptr;
    Glyph* m_icon = nullptr;
    Flex* m_text = nullptr;
    Label* m_title = nullptr;
    Label* m_detail = nullptr;
    float m_scale = 1.0F;
    bool m_scaleApplied = false;
  };

} // namespace

SearchPicker::SearchPicker() {
  m_emptyText = i18n::tr("ui.controls.search-picker.empty");

  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceSm);
  setPadding(Style::spaceSm);
  setFill(colorSpecFromRole(ColorRole::Surface));
  setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
  setRadius(Style::scaledRadiusMd());
  setSize(kDefaultWidth, kDefaultHeight);

  addChild(
      ui::input({
          .out = &m_input,
          .placeholder = i18n::tr("ui.controls.search-picker.placeholder"),
          .controlHeight = Style::controlHeight,
          .onChange =
              [this](const std::string& value) {
                if (value == m_filter) {
                  return;
                }
                m_filter = value;
                applyFilter();
              },
          .onKeyEvent =
              [this](std::uint32_t sym, std::uint32_t modifiers) {
                if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
                  moveHighlight(1);
                  return true;
                }
                if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
                  moveHighlight(-1);
                  return true;
                }
                if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
                  activateHighlighted();
                  return true;
                }
                if (KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
                  if (m_onCancel) {
                    m_onCancel();
                  }
                  return true;
                }
                return false;
              },
      })
  );

  addChild(
      ui::label({
          .out = &m_emptyLabel,
          .text = m_emptyText,
          .fontSize = Style::fontSizeBody,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
      })
  );

  addChild(
      ui::virtualListView({
          .out = &m_list,
          .itemGap = Style::spaceXs,
          .overscanItems = 4,
          .adapter = this,
          .flexGrow = 1.0F,
      })
  );
}

void SearchPicker::setOptions(std::vector<SearchPickerOption> options) {
  m_options = std::move(options);
  applyFilter();
}

void SearchPicker::setPlaceholder(std::string_view placeholder) {
  if (m_input != nullptr) {
    m_input->setPlaceholder(placeholder);
  }
}

void SearchPicker::setEmptyText(std::string_view text) {
  m_emptyText = std::string(text);
  if (m_emptyLabel != nullptr) {
    m_emptyLabel->setText(m_emptyText);
  }
  markLayoutDirty();
}

void SearchPicker::setSelectedValue(std::string_view value) {
  m_selectedValue = std::string(value);
  ++m_revision;
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
}

void SearchPicker::setFilter(std::string_view filter) {
  m_filter = std::string(filter);
  if (m_input != nullptr) {
    m_input->setValue(m_filter);
  }
  applyFilter();
}

void SearchPicker::setOnActivated(std::function<void(const SearchPickerOption&)> callback) {
  m_onActivated = std::move(callback);
}

void SearchPicker::setOnCancel(std::function<void()> callback) { m_onCancel = std::move(callback); }

InputArea* SearchPicker::filterInputArea() const noexcept {
  return m_input != nullptr ? m_input->inputArea() : nullptr;
}

void SearchPicker::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (m_input != nullptr) {
    m_input->setEnabled(enabled);
  }
  ++m_revision;
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  setOpacity(enabled ? 1.0F : 0.55F);
}

void SearchPicker::doLayout(Renderer& renderer) { Flex::doLayout(renderer); }

LayoutSize SearchPicker::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void SearchPicker::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

std::size_t SearchPicker::itemCount() const { return m_visible.size(); }

std::uint64_t SearchPicker::itemKey(std::size_t index) const {
  if (index >= m_visible.size()) {
    return 0;
  }
  return static_cast<std::uint64_t>(m_visible[index]);
}

std::uint64_t SearchPicker::itemRevision(std::size_t index) const {
  std::uint64_t revision = m_revision;
  if (index >= m_visible.size()) {
    return revision;
  }
  const auto sourceIndex = m_visible[index];
  if (sourceIndex < m_options.size() && m_options[sourceIndex].value == m_selectedValue) {
    revision ^= (1ULL << 32);
  }
  if (index == m_highlightedVisibleIndex) {
    revision ^= (1ULL << 33);
  }
  return revision;
}

bool SearchPicker::itemInteractive(std::size_t index) const {
  if (!m_enabled || index >= m_visible.size()) {
    return false;
  }
  const auto sourceIndex = m_visible[index];
  return sourceIndex < m_options.size() && m_options[sourceIndex].enabled;
}

float SearchPicker::measureItem(Renderer& /*renderer*/, std::size_t index, float /*width*/) {
  if (index >= m_visible.size()) {
    return Style::controlHeight * m_scale;
  }
  const auto sourceIndex = m_visible[index];
  const bool hasDetail = sourceIndex < m_options.size() && !m_options[sourceIndex].description.empty();
  return (hasDetail ? Style::controlHeightLg : Style::controlHeight) * m_scale;
}

std::unique_ptr<Node> SearchPicker::createItem() { return std::make_unique<SearchPickerRow>(); }

void SearchPicker::bindItem(Renderer& /*renderer*/, Node& item, std::size_t index, float /*width*/, bool hovered) {
  auto* row = dynamic_cast<SearchPickerRow*>(&item);
  if (row == nullptr || index >= m_visible.size()) {
    return;
  }
  const auto sourceIndex = m_visible[index];
  if (sourceIndex >= m_options.size()) {
    return;
  }

  const auto& option = m_options[sourceIndex];
  const bool highlighted = index == m_highlightedVisibleIndex;
  const bool selected = option.value == m_selectedValue;
  row->applyScale(m_scale);
  row->bind(option, highlighted, selected, hovered);
}

void SearchPicker::setContentScale(float scale) {
  const float pinned = scale > 0.0F ? scale : 1.0F;
  if (m_scale == pinned) {
    return;
  }
  m_scale = pinned;
  setGap(Style::spaceSm * pinned);
  setPadding(Style::spaceSm * pinned);
  setRadius(Style::scaledRadiusMd(pinned));
  if (m_input != nullptr) {
    m_input->setControlHeight(Style::controlHeight * pinned);
    m_input->setFontSize(Style::fontSizeBody * pinned);
    m_input->setHorizontalPadding(Style::spaceSm * pinned);
  }
  if (m_emptyLabel != nullptr) {
    m_emptyLabel->setFontSize(Style::fontSizeBody * pinned);
  }
  if (m_list != nullptr) {
    m_list->setItemGap(Style::spaceXs * pinned);
    m_list->setContentScale(pinned);
  }
  ++m_revision;
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
}

void SearchPicker::onActivate(std::size_t index) {
  if (index >= m_visible.size()) {
    return;
  }
  const auto sourceIndex = m_visible[index];
  if (sourceIndex < m_options.size() && m_options[sourceIndex].enabled && m_onActivated) {
    m_onActivated(m_options[sourceIndex]);
  }
}

void SearchPicker::applyFilter() {
  struct ScoredOption {
    std::size_t index = 0;
    double score = 0.0;
  };

  m_visible.clear();
  std::vector<ScoredOption> scored;
  const std::string query = StringUtils::trim(m_filter);

  for (std::size_t i = 0; i < m_options.size(); ++i) {
    if (query.empty()) {
      m_visible.push_back(i);
      continue;
    }

    const double score = matchScore(m_options[i], query);
    if (FuzzyMatch::isMatch(score)) {
      scored.push_back(ScoredOption{.index = i, .score = score});
    }
  }

  if (!query.empty()) {
    std::ranges::stable_sort(scored, std::ranges::greater{}, &ScoredOption::score);
    m_visible.reserve(scored.size());
    for (const auto& item : scored) {
      m_visible.push_back(item.index);
    }
  }

  m_highlightedVisibleIndex = 0;
  if (query.empty() && !m_selectedValue.empty()) {
    for (std::size_t i = 0; i < m_visible.size(); ++i) {
      const auto sourceIndex = m_visible[i];
      if (sourceIndex < m_options.size() && m_options[sourceIndex].value == m_selectedValue) {
        m_highlightedVisibleIndex = i;
        break;
      }
    }
  }
  ++m_revision;
  if (m_emptyLabel != nullptr) {
    m_emptyLabel->setVisible(m_visible.empty());
  }
  if (m_list != nullptr) {
    m_list->setVisible(!m_visible.empty());
    m_list->scrollView().setScrollOffset(0.0F);
    m_list->notifyDataChanged();
    if (!m_visible.empty() && m_highlightedVisibleIndex > 0) {
      m_list->scrollToIndex(m_highlightedVisibleIndex);
    }
  }
  markLayoutDirty();
}

void SearchPicker::setHighlightedVisibleIndex(std::size_t index) {
  if (index >= m_visible.size()) {
    return;
  }
  const std::size_t previous = m_highlightedVisibleIndex;
  m_highlightedVisibleIndex = index;
  notifyHighlightedChanged(previous, m_highlightedVisibleIndex);
  ensureHighlightedVisible();
}

void SearchPicker::moveHighlight(int delta) {
  if (m_visible.empty()) {
    return;
  }
  const int count = static_cast<int>(m_visible.size());
  const int next = (static_cast<int>(m_highlightedVisibleIndex) + delta + count) % count;
  setHighlightedVisibleIndex(static_cast<std::size_t>(next));
}

void SearchPicker::activateHighlighted() {
  if (m_highlightedVisibleIndex >= m_visible.size()) {
    return;
  }
  const auto optionIndex = m_visible[m_highlightedVisibleIndex];
  if (optionIndex < m_options.size() && m_options[optionIndex].enabled && m_onActivated) {
    m_onActivated(m_options[optionIndex]);
  }
}

void SearchPicker::ensureHighlightedVisible() {
  if (m_list == nullptr || m_highlightedVisibleIndex >= m_visible.size()) {
    return;
  }
  m_list->scrollToIndex(m_highlightedVisibleIndex);
}

void SearchPicker::notifyHighlightedChanged(std::size_t previous, std::size_t next) {
  if (m_list == nullptr) {
    return;
  }
  if (previous < m_visible.size()) {
    m_list->notifyItemChanged(previous);
  }
  if (next < m_visible.size() && next != previous) {
    m_list->notifyItemChanged(next);
  }
  markPaintDirty();
}

double SearchPicker::matchScore(const SearchPickerOption& option, std::string_view query) const {
  const std::string haystack = option.label + " " + option.value + " " + option.description;
  const auto tokens = StringUtils::splitWhitespace(query);
  if (tokens.empty()) {
    return 0.0;
  }
  double total = 0.0;
  for (const auto& token : tokens) {
    const double score = FuzzyMatch::score(token, haystack);
    if (!FuzzyMatch::isMatch(score)) {
      return FuzzyMatch::noMatchScore;
    }
    total += score;
  }
  return total;
}
