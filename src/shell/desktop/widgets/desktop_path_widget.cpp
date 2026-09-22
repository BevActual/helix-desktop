#include "shell/desktop/widgets/desktop_path_widget.h"

#include "core/log.h"
#include "core/process/process.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <utility>

namespace {

  constexpr Logger kLog("desktop-path");
  constexpr float kMinContentFit = 0.05F;
  constexpr float kMaxContentFit = 20.0F;

  [[nodiscard]] std::string pathLeaf(const std::string& path) {
    if (path.empty()) {
      return {};
    }
    const std::filesystem::path file{path};
    std::string name = file.filename().string();
    if (name.empty() || name == "." || name == "..") {
      name = file.parent_path().filename().string();
    }
    return name;
  }

} // namespace

DesktopPathWidget::DesktopPathWidget(Options options)
    : m_path(std::move(options.path)), m_label(std::move(options.label)), m_glyph(std::move(options.glyph)),
      m_preferFolder(options.preferFolder), m_showBackground(options.showBackground) {}

void DesktopPathWidget::create() {
  auto rootNode = ui::node({});

  auto button = ui::button({
      .out = &m_button,
      .text = displayLabel(),
      .glyph = displayGlyph(),
      .variant = ButtonVariant::Ghost,
      .onClick = [this]() { openPath(); },
  });
  if (m_animations != nullptr) {
    m_button->setAnimationManager(m_animations);
  }

  rootNode->addChild(std::move(button));
  setRoot(std::move(rootNode));
  syncButtonContent();
  syncButtonStyle();
  if (m_editorPreview) {
    setEditorPreview(true);
  }
}

void DesktopPathWidget::setEditorPreview(bool enabled) noexcept {
  if (m_editorPreview == enabled) {
    return;
  }
  m_editorPreview = enabled;
  if (m_button == nullptr) {
    return;
  }
  if (InputArea* area = m_button->inputArea()) {
    area->setHitTestVisible(!enabled);
  }
  m_button->setHoverSuppressed(enabled);
}

void DesktopPathWidget::layout(Renderer& renderer) {
  if (m_boxWidth <= 0.0F || m_boxHeight <= 0.0F) {
    m_fillToBox = false;
    DesktopWidget::layout(renderer);
    return;
  }

  UiPhaseScope layoutPhase(UiPhase::Layout);
  onFontFamilyChanged(m_fontFamily, renderer);

  const float innerWidth = boxInnerWidth();
  const float innerHeight = boxInnerHeight();
  const float referenceHeight = Style::controlHeight * m_baseScale;

  float fit = std::clamp(innerHeight / referenceHeight, kMinContentFit, kMaxContentFit);
  for (int attempt = 0; attempt < 4; ++attempt) {
    m_fillToBox = false;
    m_contentScale = m_baseScale * fit;
    doLayout(renderer);
    const float contentWidth = std::max(1.0F, root()->width());
    if (contentWidth <= innerWidth + 0.5F) {
      break;
    }
    const float adjusted = fit * (innerWidth / contentWidth);
    if (std::abs(adjusted - fit) < 0.001F) {
      break;
    }
    fit = std::clamp(adjusted, kMinContentFit, kMaxContentFit);
  }

  m_fillToBox = true;
  m_contentScale = m_baseScale * fit;
  doLayout(renderer);
  applyBackground();
}

bool DesktopPathWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& /*allSettings*/, Renderer& renderer
) {
  if (m_button == nullptr) {
    return false;
  }

  if (key == "path" || key == "label" || key == "glyph") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      if (key == "path") {
        m_path = *v;
      } else if (key == "label") {
        m_label = *v;
      } else {
        m_glyph = *v;
      }
      syncButtonContent();
      layout(renderer);
      return true;
    }
    return false;
  }
  if (key == "background") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_showBackground = *v;
      syncButtonStyle();
      requestRedraw();
      return true;
    }
    return false;
  }

  return DesktopWidget::applySetting(key, value, {}, renderer);
}

void DesktopPathWidget::onFontFamilyChanged(const std::string& family, Renderer& /*renderer*/) {
  if (m_button != nullptr && m_button->label() != nullptr) {
    m_button->label()->setFontFamily(family);
  }
}

void DesktopPathWidget::doLayout(Renderer& renderer) {
  if (m_button == nullptr || root() == nullptr) {
    return;
  }

  const bool boxed = m_boxWidth > 0.0F && m_boxHeight > 0.0F;
  const bool fillBox = boxed && m_fillToBox;
  const float innerW = boxed ? boxInnerWidth() : 0.0F;
  const float innerH = boxed ? boxInnerHeight() : 0.0F;

  const float scale = contentScale();
  m_button->setFontSize(Style::fontSizeBody * scale);
  m_button->setGlyphSize(Style::baseGlyphSize * scale);
  m_button->setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
  m_button->setRadius(Style::scaledRadiusMd(scale));

  if (fillBox) {
    m_button->setMinWidth(innerW);
    m_button->setMinHeight(innerH);
  } else {
    m_button->setSize(0.0F, 0.0F);
    m_button->setMinWidth(0.0F);
    m_button->setMinHeight(Style::controlHeight * scale);
  }

  if (!m_fontFamily.empty() && m_button->label() != nullptr) {
    m_button->label()->setFontFamily(m_fontFamily);
  }

  syncButtonContent();
  syncButtonStyle();

  m_button->layout(renderer);

  if (fillBox) {
    m_button->setSize(innerW, innerH);
  }

  m_button->updateInputArea();

  if (fillBox) {
    root()->setClipChildren(true);
    root()->setSize(innerW, innerH);
  } else {
    root()->setClipChildren(false);
    root()->setSize(m_button->width(), m_button->height());
  }
}

void DesktopPathWidget::syncButtonContent() {
  if (m_button == nullptr) {
    return;
  }
  m_button->setText(displayLabel());
  m_button->setGlyph(displayGlyph());
  if (m_button->glyph() != nullptr) {
    m_button->glyph()->setVisible(true);
  }
  m_button->setTooltip(m_path);
}

void DesktopPathWidget::syncButtonStyle() {
  if (m_button == nullptr) {
    return;
  }

  m_button->setVariant(ButtonVariant::Ghost);
  Button::ButtonPalette buttonPalette = Button::defaultPalette(ButtonVariant::Ghost);
  if (!m_showBackground) {
    const auto stripChrome = [](const Button::ButtonStateColors& state) {
      return Button::ButtonStateColors{
          .bg = clearColorSpec(),
          .border = clearColorSpec(),
          .label = state.label,
      };
    };
    buttonPalette.borderWidth = 0.0F;
    buttonPalette.normal = stripChrome(buttonPalette.normal);
    buttonPalette.pressed = stripChrome(buttonPalette.pressed);
    buttonPalette.disabled = stripChrome(buttonPalette.disabled);
    if (buttonPalette.selected.has_value()) {
      buttonPalette.selected = stripChrome(*buttonPalette.selected);
    }
  }
  m_button->setCustomPalette(buttonPalette);
}

void DesktopPathWidget::openPath() const {
  if (m_path.empty() || !process::commandExists("xdg-open")) {
    return;
  }
  if (!process::runAsync(std::vector<std::string>{"xdg-open", m_path})) {
    kLog.warn("failed to open path");
  }
}

std::string DesktopPathWidget::displayLabel() const {
  if (!m_label.empty()) {
    return m_label;
  }
  const std::string leaf = pathLeaf(m_path);
  if (!leaf.empty()) {
    return leaf;
  }
  return i18n::tr(m_preferFolder ? "desktop-widgets.editor.types.folder" : "desktop-widgets.editor.types.file");
}

std::string DesktopPathWidget::displayGlyph() const {
  if (!m_glyph.empty()) {
    return m_glyph;
  }
  std::error_code ec;
  if (!m_path.empty() && std::filesystem::is_directory(m_path, ec)) {
    return "folder";
  }
  if (!m_path.empty() && std::filesystem::is_regular_file(m_path, ec)) {
    return "file";
  }
  return m_preferFolder ? "folder" : "file";
}
