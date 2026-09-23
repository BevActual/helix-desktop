#include "ui/controls/segmented.h"

#include "render/core/render_styles.h"
#include "render/scene/input_area.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/roving_list_nav.h"
#include "ui/controls/separator.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace {

  Button::ButtonStateColors compactState(ColorSpec bg, ColorSpec label) {
    return Button::ButtonStateColors{
        .bg = bg,
        .border = clearColorSpec(),
        .label = label,
    };
  }

  Button::ButtonPalette compactInactivePalette() {
    return Button::ButtonPalette{
        .borderWidth = 0.0F,
        .normal = compactState(clearColorSpec(), colorSpecFromRole(ColorRole::OnSurfaceVariant)),
        .hover = compactState(colorSpecFromRole(ColorRole::Hover), colorSpecFromRole(ColorRole::OnHover)),
        .pressed = compactState(colorSpecFromRole(ColorRole::Surface), colorSpecFromRole(ColorRole::OnSurface)),
        .disabled = compactState(clearColorSpec(), colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.55F)),
    };
  }

  Button::ButtonPalette compactActivePalette() {
    const auto fill = colorSpecFromRole(ColorRole::Surface);
    const auto label = colorSpecFromRole(ColorRole::OnSurface);
    return Button::ButtonPalette{
        .borderWidth = 0.0F,
        .normal = compactState(fill, label),
        .hover = compactState(fill, label),
        .pressed = compactState(fill, label),
        .disabled = compactState(colorSpecFromRole(ColorRole::Surface, 0.55F), colorSpecFromRole(ColorRole::OnSurface, 0.55F)),
    };
  }

} // namespace

Segmented::Segmented() {
  setDirection(FlexDirection::Horizontal);
  setAlign(FlexAlign::Stretch);
  setGap(0.0F);
  applyOuterStyle();

  auto area = std::make_unique<InputArea>();
  area->setFocusable(true);
  area->setHitTestVisible(false);
  m_focusArea = static_cast<InputArea*>(addChild(std::move(area)));
  m_focusArea->setParticipatesInLayout(false);
  m_focusArea->setZIndex(2);

  m_rovingNav.setOptions(
      RovingListNavController::Options{
          .axis = RovingListNavAxis::Horizontal,
          .mode = RovingListNavMode::FollowFocus,
          .scrollIntoView = {},
          .syncIndexFromSelection = [this]() { return m_selected; },
      }
  );
  m_rovingNav.bindFocusArea(m_focusArea);
}

std::size_t Segmented::addOption(std::string_view label) { return addOption(label, std::string_view{}); }

std::size_t Segmented::addOption(std::string_view label, std::string_view glyph) {
  const std::size_t index = m_buttons.size();
  if (index > 0) {
    auto sep = makeSegmentSeparator();
    m_separators.push_back(sep.get());
    addChild(std::move(sep));
  }
  auto btn = makeSegmentButton(label, glyph, index);
  Button* raw = btn.get();
  m_buttons.push_back(raw);
  m_rovingNav.registerItem(raw, [this, index]() { setSelectedIndex(index); });
  addChild(std::move(btn));
  refreshVariants();
  return index;
}

void Segmented::setSelectedIndex(std::size_t index) {
  if (index >= m_buttons.size() || index == m_selected) {
    return;
  }
  m_selected = index;
  refreshVariants();
  m_rovingNav.notifyExternalSelectionChanged();
  if (m_onChange) {
    m_onChange(index);
  }
}

void Segmented::activateIndex(std::size_t index) {
  if (index >= m_buttons.size()) {
    return;
  }
  if (index != m_selected) {
    m_selected = index;
    refreshVariants();
    m_rovingNav.notifyExternalSelectionChanged();
  }
  if (m_onChange) {
    m_onChange(index);
  }
}

void Segmented::setFontSize(float size) {
  m_fontSize = size;
  const float fs = effectiveFontSize();
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      btn->setFontSize(fs);
      btn->setGlyphSize(fs);
    }
  }
}

void Segmented::setScale(float scale) {
  m_scale = std::max(0.1F, scale);
  applyOuterStyle();
  const float fs = effectiveFontSize();
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      applyButtonMetrics(*btn);
      btn->setFontSize(fs);
      btn->setGlyphSize(fs);
    }
  }
  const float ruleW = std::max(1.0F, Style::borderWidth * m_scale);
  for (Separator* sep : m_separators) {
    if (sep != nullptr) {
      sep->setThickness(ruleW);
    }
  }
  refreshVariants();
  markLayoutDirty();
}

void Segmented::setCompact(bool compact) {
  if (m_compact == compact) {
    return;
  }
  m_compact = compact;
  if (m_compact) {
    setEqualSegmentWidths(true);
  }
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      applyButtonMetrics(*btn);
    }
  }
  applyOuterStyle();
  refreshVariants();
  markLayoutDirty();
}

void Segmented::setPadding(float padding) {
  m_outerPadding = padding;
  Flex::setPadding(padding);
}

void Segmented::setPadding(float vertical, float horizontal) {
  m_outerPadding = vertical;
  Flex::setPadding(vertical, horizontal);
}

void Segmented::setPadding(float top, float right, float bottom, float left) {
  m_outerPadding = top;
  Flex::setPadding(top, right, bottom, left);
}

void Segmented::setOptionTooltip(std::size_t index, std::string_view text) {
  if (index < m_buttons.size() && m_buttons[index] != nullptr) {
    m_buttons[index]->setTooltip(text);
    if (m_optionTooltips.size() < m_buttons.size()) {
      m_optionTooltips.resize(m_buttons.size());
    }
    m_optionTooltips[index] = std::string(text);
  }
}

std::string_view Segmented::optionTooltip(std::size_t index) const noexcept {
  return index < m_optionTooltips.size() ? std::string_view{m_optionTooltips[index]} : std::string_view{};
}

void Segmented::clearOptions() {
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      (void)removeChild(btn);
    }
  }
  for (Separator* sep : m_separators) {
    if (sep != nullptr) {
      (void)removeChild(sep);
    }
  }
  m_buttons.clear();
  m_optionTooltips.clear();
  m_separators.clear();
  m_rovingNav.clearItems();
  m_selected = 0;
  markLayoutDirty();
}

void Segmented::setOnChange(std::function<void(std::size_t)> callback) { m_onChange = std::move(callback); }

void Segmented::setOnHover(std::function<void(std::size_t)> callback) { m_onHover = std::move(callback); }

void Segmented::setOnHoverEnd(std::function<void()> callback) { m_onHoverEnd = std::move(callback); }

void Segmented::handleSegmentEnter(std::size_t index) {
  if (m_onHover) {
    m_onHover(index);
  }
}

void Segmented::handleSegmentLeave() {
  if (m_onHoverEnd) {
    m_onHoverEnd();
  }
}

void Segmented::setSurfaceOpacity(float opacity) {
  const float clamped = std::clamp(opacity, 0.0F, 1.0F);
  if (m_surfaceOpacity == clamped) {
    return;
  }
  m_surfaceOpacity = clamped;
  applyOuterStyle();
}

void Segmented::setSurfaceRole(ColorRole role) {
  if (m_surfaceRole == role) {
    return;
  }
  m_surfaceRole = role;
  applyOuterStyle();
}

void Segmented::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      btn->setEnabled(enabled);
    }
  }
  setOpacity(enabled ? 1.0F : 0.55F);
}

std::unique_ptr<Separator> Segmented::makeSegmentSeparator() {
  auto sep = std::make_unique<Separator>();
  sep->setOrientation(SeparatorOrientation::VerticalRule);
  sep->setThickness(std::max(1.0F, Style::borderWidth * m_scale));
  sep->setColor(colorSpecFromRole(ColorRole::Outline));
  sep->setFlexGrow(0.0F);
  return sep;
}

std::unique_ptr<Button>
Segmented::makeSegmentButton(std::string_view label, std::string_view glyph, std::size_t index) {
  auto btn = std::make_unique<Button>();
  if (!glyph.empty()) {
    btn->setGlyph(glyph);
    btn->setGlyphSize(effectiveFontSize());
  }
  if (!label.empty()) {
    btn->setText(label);
    btn->setFontSize(effectiveFontSize());
  }
  applyButtonMetrics(*btn);
  btn->setOnClick([this, index]() { activateIndex(index); });
  btn->setOnEnter([this, index]() { handleSegmentEnter(index); });
  btn->setOnLeave([this]() { handleSegmentLeave(); });
  btn->setTabStop(false);
  btn->setFlexGrow(m_equalSegmentWidths ? 1.0F : 0.0F);
  btn->setContentAlign(ButtonContentAlign::Center);
  btn->setEnabled(m_enabled);
  return btn;
}

void Segmented::applyButtonMetrics(Button& button) const {
  if (m_compact) {
    button.setMinHeight(Style::controlHeightSm * m_scale);
    button.setPadding(Style::spaceXs * m_scale, Style::spaceSm * m_scale);
    return;
  }

  button.setMinHeight(Style::controlHeight * m_scale);
  button.setPadding(Style::spaceXs * m_scale, Style::spaceMd * m_scale);
}

void Segmented::setEqualSegmentWidths(bool equalWidths) {
  if (m_equalSegmentWidths == equalWidths) {
    return;
  }
  m_equalSegmentWidths = equalWidths;
  for (Button* b : m_buttons) {
    if (b != nullptr) {
      b->setFlexGrow(m_equalSegmentWidths ? 1.0F : 0.0F);
    }
  }
  markLayoutDirty();
}

void Segmented::refreshVariants() {
  const std::size_t n = m_buttons.size();
  const float r = m_compact ? Style::scaledRadiusLg(m_scale) : Style::scaledRadiusMd(m_scale);
  for (Separator* sep : m_separators) {
    if (sep == nullptr) {
      continue;
    }
    sep->setVisible(!m_compact);
    sep->setParticipatesInLayout(!m_compact);
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (m_buttons[i] == nullptr) {
      continue;
    }
    const bool active = i == m_selected;
    m_buttons[i]->setVariant(active ? ButtonVariant::TabActive : ButtonVariant::Tab);
    if (m_compact) {
      m_buttons[i]->setCustomPalette(active ? compactActivePalette() : compactInactivePalette());
    }
    Radii radii;
    if (m_compact || n == 1) {
      radii = Radii{r, r, r, r};
    } else if (i == 0) {
      radii = Radii{r, 0.0F, 0.0F, r};
    } else if (i == n - 1) {
      radii = Radii{0.0F, r, r, 0.0F};
    } else {
      radii = Radii{0.0F};
    }
    m_buttons[i]->setRadii(radii);
  }
}

void Segmented::applyOuterStyle() {
  const float inset = m_compact ? std::max(2.0F, Style::spaceXs * 0.5F * m_scale) : m_outerPadding;
  Flex::setPadding(inset);
  setGap(m_compact ? inset : 0.0F);
  setFill(colorSpecFromRole(m_surfaceRole, m_surfaceOpacity));
  clearBorder();
  setRadius(m_compact ? Style::scaledRadiusXl(m_scale) : Style::scaledRadiusMd(m_scale));
}

void Segmented::doLayout(Renderer& renderer) {
  syncEqualSegmentMinWidths(renderer);
  Flex::doLayout(renderer);
  m_rovingNav.layoutOverlay(width(), height());
}

LayoutSize Segmented::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  syncEqualSegmentMinWidths(renderer);
  return Flex::doMeasure(renderer, constraints);
}

void Segmented::syncEqualSegmentMinWidths(Renderer& renderer) {
  if (!m_equalSegmentWidths) {
    for (Button* b : m_buttons) {
      if (b != nullptr) {
        b->setMinWidth(0.0F);
      }
    }
    return;
  }
  float widest = 0.0F;
  for (Button* b : m_buttons) {
    if (b == nullptr) {
      continue;
    }
    // Intrinsic width: clear last pass's floor so a previously assigned narrow box cannot
    // feed back into the measurement.
    b->setMinWidth(0.0F);
    widest = std::max(widest, b->measure(renderer, LayoutConstraints{}).width);
  }
  for (Button* b : m_buttons) {
    if (b != nullptr) {
      b->setMinWidth(std::ceil(widest));
    }
  }
}

float Segmented::effectiveFontSize() const noexcept {
  return (m_fontSize > 0.0F ? m_fontSize : Style::fontSizeBody) * m_scale;
}
