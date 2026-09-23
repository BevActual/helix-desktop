#include "ui/controls/list_editor.h"

#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace {

  constexpr float kLabelCellWidth = 200.0F;
  constexpr float kFreeformInputWidth = 190.0F;
  constexpr float kItemRowHeight = 26.0F;
  constexpr float kSuggestedAddHeight = 30.0F;
  constexpr float kVerticalGap = 2.0F;

  std::unique_ptr<Label> makeListLabel(std::string_view text, float scale, bool grouped) {
    return ui::label({
        .text = std::string(text),
        .fontSize = (grouped ? Style::fontSizeBody : Style::fontSizeCaption) * scale,
        .color = colorSpecFromRole(ColorRole::OnSurface),
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
        .flexGrow = grouped ? 1.0F : 0.0F,
    });
  }

} // namespace

ListEditor::ListEditor() {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(kVerticalGap);
}

void ListEditor::setItems(std::vector<std::string> items) {
  m_items = std::move(items);
  rebuildRows();
}

void ListEditor::setSuggestedOptions(std::vector<ListEditorOption> options) {
  m_suggestedOptions = std::move(options);
  rebuildRows();
}

void ListEditor::setAddPlaceholder(std::string_view placeholder) {
  m_addPlaceholder = std::string(placeholder);
  rebuildRows();
}

void ListEditor::setScale(float scale) {
  m_scale = std::max(0.1F, scale);
  setGap(kVerticalGap * m_scale);
  rebuildRows();
}

void ListEditor::setMaxItems(std::size_t maxItems) {
  m_maxItems = maxItems;
  rebuildRows();
}

void ListEditor::setItemAccessory(std::function<std::unique_ptr<Node>(std::size_t)> accessory) {
  m_itemAccessory = std::move(accessory);
  rebuildRows();
}

void ListEditor::setPresentation(ListEditorPresentation presentation) {
  if (m_presentation == presentation) {
    return;
  }
  m_presentation = presentation;
  rebuildRows();
}

void ListEditor::setItemLeading(std::function<std::unique_ptr<Node>(std::size_t)> leading) {
  m_itemLeading = std::move(leading);
  rebuildRows();
}

void ListEditor::setRowConfigure(std::function<void(Flex&)> configure) {
  m_rowConfigure = std::move(configure);
  rebuildRows();
}

void ListEditor::setOnAddRequested(std::function<void(std::string)> callback) {
  m_onAddRequested = std::move(callback);
}

void ListEditor::setOnRemoveRequested(std::function<void(std::size_t)> callback) {
  m_onRemoveRequested = std::move(callback);
}

void ListEditor::setOnMoveRequested(std::function<void(std::size_t, std::size_t)> callback) {
  m_onMoveRequested = std::move(callback);
}

std::string ListEditor::labelForValue(std::string_view value) const {
  for (const auto& opt : m_suggestedOptions) {
    if (opt.value == value) {
      return opt.label;
    }
  }
  return std::string(value);
}

std::vector<ListEditorOption> ListEditor::remainingOptions() const {
  std::vector<ListEditorOption> remaining;
  remaining.reserve(m_suggestedOptions.size());
  for (const auto& opt : m_suggestedOptions) {
    if (!std::ranges::contains(m_items, opt.value)) {
      remaining.push_back(opt);
    }
  }
  return remaining;
}

void ListEditor::rebuildRows() {
  while (!children().empty()) {
    removeChild(children().back().get());
  }

  const bool grouped = m_presentation == ListEditorPresentation::Grouped;
  const float labelCellWidth = (m_itemAccessory ? 132.0F : kLabelCellWidth) * m_scale;
  const float itemRowHeight =
      (grouped ? Style::controlHeight : (m_itemAccessory ? Style::controlHeightSm : kItemRowHeight)) * m_scale;
  const float suggestedAddHeight = (grouped ? Style::controlHeight : kSuggestedAddHeight) * m_scale;
  const float actionSize = (grouped ? Style::controlHeightSm : kItemRowHeight) * m_scale;
  const float actionGlyph = (grouped ? Style::fontSizeBody : Style::fontSizeCaption) * m_scale;

  std::unique_ptr<Flex> addRow;
  const bool atCapacity = m_maxItems > 0 && m_items.size() >= m_maxItems;
  if (!atCapacity || !m_suggestedOptions.empty()) {
    addRow = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * m_scale,
    });

    if (!m_suggestedOptions.empty()) {
      const auto remaining = remainingOptions();
      if (!remaining.empty()) {
        std::vector<std::string> remainingLabels;
        remainingLabels.reserve(remaining.size());
        for (const auto& opt : remaining) {
          remainingLabels.push_back(opt.label);
        }

        Select* selectPtr = nullptr;
        auto select = ui::select({
            .out = &selectPtr,
            .options = std::move(remainingLabels),
            .placeholder = m_addPlaceholder,
            .fontSize = (grouped ? Style::fontSizeBody : Style::fontSizeCaption) * m_scale,
            .controlHeight = suggestedAddHeight,
            .glyphSize = (grouped ? Style::fontSizeBody : Style::fontSizeCaption) * m_scale,
            .width = grouped ? std::optional<float>{} : std::optional<float>{labelCellWidth},
            .height = suggestedAddHeight,
            .flexGrow = grouped ? 1.0F : 0.0F,
        });

        auto addBtn = ui::button({
            .glyph = "add",
            .glyphSize = (grouped ? Style::fontSizeBody : Style::fontSizeCaption) * m_scale,
            .enabled = !atCapacity,
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr("settings.controls.list.add"),
            .minWidth = suggestedAddHeight,
            .minHeight = suggestedAddHeight,
            .padding = Style::spaceXs * m_scale,
            .radius = Style::scaledRadiusSm(m_scale),
            .onClick = [this, selectPtr, remaining] {
              const std::size_t index = selectPtr->selectedIndex();
              if (index < remaining.size() && m_onAddRequested) {
                m_onAddRequested(remaining[index].value);
              }
            },
        });

        addRow->addChild(std::move(select));
        addRow->addChild(std::move(addBtn));
      } else {
        addRow.reset();
      }
    } else if (!atCapacity) {
      Input* addInputPtr = nullptr;
      auto addInput = ui::input({
          .out = &addInputPtr,
          .placeholder = m_addPlaceholder,
          .fontSize = Style::fontSizeBody * m_scale,
          .controlHeight = Style::controlHeight * m_scale,
          .horizontalPadding = Style::spaceSm * m_scale,
          .width = kFreeformInputWidth * m_scale,
          .height = Style::controlHeight * m_scale,
          .onSubmit = [this](const std::string& text) {
            if (!text.empty() && m_onAddRequested) {
              m_onAddRequested(text);
            }
          },
      });

      auto addBtn = ui::button({
          .glyph = "add",
          .glyphSize = Style::fontSizeBody * m_scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("settings.controls.list.add"),
          .minWidth = Style::controlHeight * m_scale,
          .minHeight = Style::controlHeight * m_scale,
          .padding = Style::spaceSm * m_scale,
          .radius = Style::scaledRadiusMd(m_scale),
          .onClick = [this, addInputPtr] {
            const auto& text = addInputPtr->value();
            if (!text.empty() && m_onAddRequested) {
              m_onAddRequested(text);
            }
          },
      });

      addRow->addChild(std::move(addInput));
      addRow->addChild(std::move(addBtn));
    }
  }

  if (m_maxItems > 0) {
    auto capacityLabel = ui::label({
        .text = std::to_string(m_items.size()) + "/" + std::to_string(m_maxItems),
        .fontSize = Style::fontSizeCaption * m_scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
    });
    if (addRow) {
      addRow->addChild(std::move(capacityLabel));
    } else {
      auto wrapper = ui::row({.align = FlexAlign::Center});
      wrapper->addChild(std::move(capacityLabel));
      addRow = std::move(wrapper);
    }
  }

  if (addRow) {
    addChild(std::move(addRow));
  }

  for (std::size_t i = 0; i < m_items.size(); ++i) {
    auto itemRow = ui::row({
        .align = FlexAlign::Center,
        .gap = (grouped ? Style::spaceSm : Style::spaceXs) * m_scale,
        .minHeight = itemRowHeight,
    });

    if (m_itemLeading) {
      if (auto leading = m_itemLeading(i)) {
        itemRow->addChild(std::move(leading));
      }
    }

    if (grouped) {
      itemRow->addChild(makeListLabel(labelForValue(m_items[i]), m_scale, true));
    } else {
      auto labelCell = ui::row(
          {
              .align = FlexAlign::Center,
              .minWidth = labelCellWidth,
          },
          makeListLabel(labelForValue(m_items[i]), m_scale, false)
      );
      itemRow->addChild(std::move(labelCell));
    }

    if (m_itemAccessory) {
      if (auto accessory = m_itemAccessory(i)) {
        itemRow->addChild(std::move(accessory));
      }
    }

    // Grouped rows keep the move arrows in fixed slots so the remove button never shifts.
    const auto addMoveButton = [&](bool up) {
      const bool enabled = up ? i > 0 : i + 1 < m_items.size();
      if (!grouped && !enabled) {
        return;
      }
      auto* button = addGhostIconButton(
          *itemRow, up ? "chevron-up" : "chevron-down", actionGlyph, actionSize,
          i18n::tr(up ? "settings.controls.list.move-up" : "settings.controls.list.move-down"),
          [this, i, up] {
            if (m_onMoveRequested) {
              m_onMoveRequested(i, up ? i - 1 : i + 1);
            }
          }
      );
      if (button != nullptr && !enabled) {
        button->setEnabled(false);
      }
    };

    if (grouped) {
      addMoveButton(true);
      addMoveButton(false);
    }
    addGhostIconButton(
        *itemRow, "close", actionGlyph, actionSize, i18n::tr("settings.controls.list.remove"),
        [this, i] {
          if (m_onRemoveRequested) {
            m_onRemoveRequested(i);
          }
        }
    );
    if (!grouped) {
      addMoveButton(true);
      addMoveButton(false);
    }

    if (m_rowConfigure) {
      m_rowConfigure(*itemRow);
    }
    addChild(std::move(itemRow));
  }

  markLayoutDirty();
}

Button* ListEditor::addGhostIconButton(
    Flex& row, std::string_view glyph, float glyphSize, float buttonSize, std::string_view tooltip,
    std::function<void()> callback
) {
  Button* out = nullptr;
  row.addChild(
      ui::button({
          .out = &out,
          .glyph = std::string(glyph),
          .glyphSize = glyphSize,
          .variant = ButtonVariant::Ghost,
          .tooltip = std::string(tooltip),
          .minWidth = buttonSize,
          .minHeight = buttonSize,
          .padding = Style::spaceXs * m_scale,
          .radius = Style::scaledRadiusSm(m_scale),
          .onClick = std::move(callback),
      })
  );
  return out;
}
