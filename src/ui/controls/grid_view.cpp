#include "ui/controls/grid_view.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <numeric>
#include <utility>
#include <vector>

void GridView::setColumns(std::size_t columns) {
  const std::size_t normalized = std::max<std::size_t>(1, columns);
  if (m_columns == normalized) {
    return;
  }
  m_columns = normalized;
  markLayoutDirty();
}

void GridView::setColumnGap(float gap) {
  if (m_columnGap == gap && m_rowGap == gap) {
    return;
  }
  m_columnGap = gap;
  m_rowGap = gap;
  markLayoutDirty();
}

void GridView::setRowGap(float gap) {
  // GridView keeps row/column spacing symmetric by design.
  setColumnGap(gap);
}

void GridView::setPadding(float top, float right, float bottom, float left) {
  m_paddingTop = top;
  m_paddingRight = right;
  m_paddingBottom = bottom;
  m_paddingLeft = left;
  markLayoutDirty();
}

void GridView::setPadding(float all) { setPadding(all, all, all, all); }

void GridView::setPadding(float vertical, float horizontal) { setPadding(vertical, horizontal, vertical, horizontal); }

void GridView::setStretchItems(bool stretch) {
  if (m_stretchItems == stretch) {
    return;
  }
  m_stretchItems = stretch;
  markLayoutDirty();
}

void GridView::setUniformCellSize(bool uniform) {
  if (m_uniformCellSize == uniform) {
    return;
  }
  m_uniformCellSize = uniform;
  markLayoutDirty();
}

void GridView::setSquareCells(bool square) {
  if (m_squareCells == square) {
    return;
  }
  m_squareCells = square;
  markLayoutDirty();
}

void GridView::setSquareGridShrinkWrap(bool shrinkWrap) {
  if (m_squareGridShrinkWrap == shrinkWrap) {
    return;
  }
  m_squareGridShrinkWrap = shrinkWrap;
  markLayoutDirty();
}

void GridView::setMinCellWidth(float width) {
  const float normalized = std::max(0.0F, width);
  if (m_minCellWidth == normalized) {
    return;
  }
  m_minCellWidth = normalized;
  markLayoutDirty();
}

void GridView::setMinCellHeight(float height) {
  const float normalized = std::max(0.0F, height);
  if (m_minCellHeight == normalized) {
    return;
  }
  m_minCellHeight = normalized;
  markLayoutDirty();
}

void GridView::setSpanLastItem(bool span) {
  if (m_spanLastItem == span) {
    return;
  }
  m_spanLastItem = span;
  markLayoutDirty();
}

void GridView::setChildSpan(Node* child, std::size_t columnSpan, std::size_t rowSpan) {
  if (child == nullptr) {
    return;
  }
  const ChildSpan next{
      .columns = std::max<std::size_t>(1, columnSpan),
      .rows = std::max<std::size_t>(1, rowSpan),
  };
  if (next.columns == 1 && next.rows == 1) {
    if (m_childSpans.erase(child) > 0) {
      markLayoutDirty();
    }
    return;
  }
  auto [it, inserted] = m_childSpans.try_emplace(child, next);
  if (!inserted && it->second.columns == next.columns && it->second.rows == next.rows) {
    return;
  }
  it->second = next;
  markLayoutDirty();
}

void GridView::clearChildSpans() {
  if (m_childSpans.empty()) {
    return;
  }
  m_childSpans.clear();
  markLayoutDirty();
}

std::size_t GridView::childColumnSpan(const Node* child) const noexcept {
  const auto it = m_childSpans.find(child);
  return it == m_childSpans.end() ? 1 : std::max<std::size_t>(1, it->second.columns);
}

std::size_t GridView::childRowSpan(const Node* child) const noexcept {
  const auto it = m_childSpans.find(child);
  return it == m_childSpans.end() ? 1 : std::max<std::size_t>(1, it->second.rows);
}

namespace {

  struct PackedSlot {
    Node* child = nullptr;
    std::size_t col = 0;
    std::size_t row = 0;
    std::size_t colSpan = 1;
    std::size_t rowSpan = 1;
  };

  std::vector<PackedSlot> packGridItems(
      const std::vector<Node*>& children, std::size_t columns,
      const std::function<std::pair<std::size_t, std::size_t>(const Node*)>& spanOf
  ) {
    columns = std::max<std::size_t>(1, columns);
    std::vector<PackedSlot> packed;
    packed.reserve(children.size());
    std::vector<std::uint8_t> occ;
    const auto ensureRow = [&](std::size_t row) {
      const std::size_t need = (row + 1) * columns;
      if (occ.size() < need) {
        occ.resize(need, 0);
      }
    };
    const auto freeAt = [&](std::size_t row, std::size_t col, std::size_t colSpan, std::size_t rowSpan) {
      for (std::size_t dr = 0; dr < rowSpan; ++dr) {
        ensureRow(row + dr);
        for (std::size_t dc = 0; dc < colSpan; ++dc) {
          if (occ[(row + dr) * columns + (col + dc)] != 0) {
            return false;
          }
        }
      }
      return true;
    };
    const auto occupy = [&](std::size_t row, std::size_t col, std::size_t colSpan, std::size_t rowSpan) {
      for (std::size_t dr = 0; dr < rowSpan; ++dr) {
        ensureRow(row + dr);
        for (std::size_t dc = 0; dc < colSpan; ++dc) {
          occ[(row + dr) * columns + (col + dc)] = 1;
        }
      }
    };

    for (Node* child : children) {
      const auto [rawColSpan, rawRowSpan] = spanOf(child);
      const std::size_t colSpan = std::clamp(rawColSpan, std::size_t{1}, columns);
      const std::size_t rowSpan = std::max<std::size_t>(1, rawRowSpan);
      bool placed = false;
      for (std::size_t row = 0; !placed; ++row) {
        for (std::size_t col = 0; col + colSpan <= columns; ++col) {
          if (!freeAt(row, col, colSpan, rowSpan)) {
            continue;
          }
          occupy(row, col, colSpan, rowSpan);
          packed.push_back(PackedSlot{child, col, row, colSpan, rowSpan});
          placed = true;
          break;
        }
      }
    }
    return packed;
  }

  std::size_t packedRowCountOf(const std::vector<PackedSlot>& packed) {
    std::size_t rows = 0;
    for (const auto& slot : packed) {
      rows = std::max(rows, slot.row + slot.rowSpan);
    }
    return rows;
  }

} // namespace

std::size_t GridView::packedRowCount() const {
  std::vector<Node*> visibleChildren;
  visibleChildren.reserve(children().size());
  for (const auto& child : children()) {
    if (child->visible()) {
      visibleChildren.push_back(child.get());
    }
  }
  if (visibleChildren.empty()) {
    return 0;
  }
  const auto packed = packGridItems(
      visibleChildren, m_columns,
      [this](const Node* child) {
        return std::pair<std::size_t, std::size_t>{childColumnSpan(child), childRowSpan(child)};
      }
  );
  return packedRowCountOf(packed);
}

LayoutSize GridView::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  float useW = width();
  if (constraints.hasExactWidth()) {
    useW = constraints.maxWidth;
  } else if (m_squareCells && m_stretchItems && m_squareGridShrinkWrap && constraints.hasExactHeight()) {
    // Intrinsic width from square row height (shrink-wrapped square grid only).
    useW = 0.0F;
  }
  float useH = height();
  if (constraints.hasExactHeight()) {
    useH = constraints.maxHeight;
  }
  if (constraints.hasExactWidth() || constraints.hasExactHeight()) {
    setSize(useW, useH);
  }
  doLayout(renderer);
  return constraints.constrain(LayoutSize{.width = width(), .height = height()});
}

void GridView::doLayout(Renderer& renderer) {
  auto layoutWithAssignedSize = [&renderer](Node* child, float assignedWidth, float assignedHeight) {
    child->arrange(
        renderer, LayoutRect{.x = child->x(), .y = child->y(), .width = assignedWidth, .height = assignedHeight}
    );
  };

  std::vector<Node*> visibleChildren;
  visibleChildren.reserve(children().size());
  for (auto& child : children()) {
    if (child->visible()) {
      visibleChildren.push_back(child.get());
    }
  }

  if (visibleChildren.empty()) {
    setSize(std::max(width(), m_paddingLeft + m_paddingRight), std::max(height(), m_paddingTop + m_paddingBottom));
    return;
  }

  bool usesItemSpans = false;
  for (Node* child : visibleChildren) {
    if (childColumnSpan(child) > 1 || childRowSpan(child) > 1) {
      usesItemSpans = true;
      break;
    }
  }

  if (usesItemSpans) {
    const std::size_t columns = std::max<std::size_t>(1, m_columns);
    const auto packed = packGridItems(
        visibleChildren, columns,
        [this](const Node* child) {
          return std::pair<std::size_t, std::size_t>{childColumnSpan(child), childRowSpan(child)};
        }
    );
    const std::size_t rows = std::max<std::size_t>(1, packedRowCountOf(packed));
    const bool hasFixedWidth = width() > 0.0F;
    const bool hasFixedHeight = height() > 0.0F;
    const float fixedWidth = width();
    const float fixedHeight = height();
    float cellW = m_minCellWidth;
    float cellH = m_minCellHeight;
    if (hasFixedWidth && columns > 0) {
      const float innerWidth =
          std::max(0.0F, fixedWidth - m_paddingLeft - m_paddingRight - m_columnGap * static_cast<float>(columns - 1));
      cellW = std::max(cellW, innerWidth / static_cast<float>(columns));
    }
    if (m_squareCells) {
      cellH = cellW;
    } else if (hasFixedHeight && rows > 0) {
      const float innerHeight =
          std::max(0.0F, fixedHeight - m_paddingTop - m_paddingBottom - m_rowGap * static_cast<float>(rows - 1));
      cellH = std::max(cellH, innerHeight / static_cast<float>(rows));
    }
    if (cellW <= 0.0F || cellH <= 0.0F) {
      for (Node* child : visibleChildren) {
        child->layout(renderer);
        cellW = std::max(cellW, child->width());
        cellH = std::max(cellH, child->height());
      }
      if (m_squareCells) {
        cellW = cellH = std::max(cellW, cellH);
      }
    }

    for (const auto& slot : packed) {
      const float itemW =
          static_cast<float>(slot.colSpan) * cellW + static_cast<float>(slot.colSpan > 0 ? slot.colSpan - 1 : 0) * m_columnGap;
      const float itemH =
          static_cast<float>(slot.rowSpan) * cellH + static_cast<float>(slot.rowSpan > 0 ? slot.rowSpan - 1 : 0) * m_rowGap;
      layoutWithAssignedSize(slot.child, itemW, itemH);
    }

    const float contentWidth =
        static_cast<float>(columns) * cellW + static_cast<float>(columns > 0 ? columns - 1 : 0) * m_columnGap;
    const float contentHeight =
        static_cast<float>(rows) * cellH + static_cast<float>(rows > 0 ? rows - 1 : 0) * m_rowGap;
    const float computedWidth = m_paddingLeft + contentWidth + m_paddingRight;
    const float computedHeight = m_paddingTop + contentHeight + m_paddingBottom;
    setSize(hasFixedWidth ? std::max(fixedWidth, computedWidth) : computedWidth, computedHeight);

    for (const auto& slot : packed) {
      slot.child->setPosition(
          m_paddingLeft + static_cast<float>(slot.col) * (cellW + m_columnGap),
          m_paddingTop + static_cast<float>(slot.row) * (cellH + m_rowGap)
      );
    }
    return;
  }

  const std::size_t columns = std::min(m_columns, std::max<std::size_t>(1, visibleChildren.size()));
  const std::size_t rows = (visibleChildren.size() + columns - 1) / columns;

  const bool hasFixedWidth = width() > 0.0F;
  const bool hasFixedHeight = height() > 0.0F;
  const float fixedWidth = width();
  const float fixedHeight = height();

  std::vector<float> columnWidths(columns, 0.0F);
  std::vector<float> rowHeights(rows, 0.0F);

  float stretchedWidth = 0.0F;
  if (hasFixedWidth && m_stretchItems && columns > 0) {
    const float innerWidth =
        std::max(0.0F, fixedWidth - m_paddingLeft - m_paddingRight - m_columnGap * static_cast<float>(columns - 1));
    stretchedWidth = innerWidth / static_cast<float>(columns);
  }

  if (m_uniformCellSize) {
    // Pass 1: measure natural child sizes without imposed cell width.
    float maxMeasuredWidth = 0.0F;
    float maxMeasuredHeight = 0.0F;
    for (Node* child : visibleChildren) {
      child->layout(renderer);
      maxMeasuredWidth = std::max(maxMeasuredWidth, child->width());
      maxMeasuredHeight = std::max(maxMeasuredHeight, child->height());
    }

    float uniformWidth = maxMeasuredWidth;
    float uniformHeight = std::max(maxMeasuredHeight, m_minCellHeight);

    const bool squarePack = m_squareCells && m_stretchItems && columns > 0 && rows > 0 && hasFixedHeight;
    if (squarePack) {
      const float innerHeight = std::max(
          0.0F, fixedHeight - m_paddingTop - m_paddingBottom - m_rowGap * static_cast<float>(rows > 0 ? rows - 1 : 0)
      );
      const float slotH = innerHeight / static_cast<float>(rows);
      if (hasFixedWidth) {
        const float innerWidth =
            std::max(0.0F, fixedWidth - m_paddingLeft - m_paddingRight - m_columnGap * static_cast<float>(columns - 1));
        const float slotW = innerWidth / static_cast<float>(columns);
        uniformWidth = uniformHeight = std::min(slotW, slotH);
      } else {
        // Shrink-wrapped grid: width comes from content; height drives square side.
        uniformWidth = uniformHeight = slotH;
      }
    } else {
      if (hasFixedWidth && columns > 0) {
        const float innerWidth =
            std::max(0.0F, fixedWidth - m_paddingLeft - m_paddingRight - m_columnGap * static_cast<float>(columns - 1));
        const float slotW = innerWidth / static_cast<float>(columns);
        if (m_stretchItems) {
          uniformWidth = slotW;
        } else {
          uniformWidth = std::max({uniformWidth, m_minCellWidth, slotW});
        }
      } else if (hasFixedWidth && m_stretchItems && stretchedWidth > 0.0F) {
        uniformWidth = std::max(uniformWidth, stretchedWidth);
      } else {
        uniformWidth = std::max(uniformWidth, m_minCellWidth);
      }

      if (hasFixedHeight && rows > 0) {
        const float innerHeight = std::max(
            0.0F, fixedHeight - m_paddingTop - m_paddingBottom - m_rowGap * static_cast<float>(rows > 0 ? rows - 1 : 0)
        );
        const float slotH = innerHeight / static_cast<float>(rows);
        if (m_stretchItems) {
          uniformHeight = slotH;
        } else {
          uniformHeight = std::max({uniformHeight, m_minCellHeight, slotH});
        }
      }
    }

    std::ranges::fill(columnWidths, uniformWidth);
    std::ranges::fill(rowHeights, uniformHeight);

    for (std::size_t index = 0; index < visibleChildren.size(); ++index) {
      Node* child = visibleChildren[index];
      float itemWidth = uniformWidth;
      if (m_spanLastItem && index == visibleChildren.size() - 1 && columns > 0) {
        const std::size_t col = index % columns;
        const std::size_t colSpan = columns - col;
        if (colSpan > 1) {
          itemWidth = static_cast<float>(colSpan) * uniformWidth + static_cast<float>(colSpan - 1) * m_columnGap;
        }
      }
      layoutWithAssignedSize(child, itemWidth, uniformHeight);
    }
  } else {
    const std::size_t lastIndex = visibleChildren.size() - 1;
    const std::size_t lastCol = lastIndex % columns;
    const bool lastItemSpans = m_spanLastItem && lastCol + 1 < columns;
    const std::size_t lastColSpan = lastItemSpans ? columns - lastCol : 1;
    const bool stretchToFixedWidth = hasFixedWidth && m_stretchItems;
    float lastItemNaturalWidth = 0.0F;

    // Measure first so every column width is final before a spanning child is arranged.
    for (std::size_t index = 0; index < visibleChildren.size(); ++index) {
      Node* child = visibleChildren[index];
      const std::size_t col = index % columns;
      const std::size_t row = index / columns;
      child->layout(renderer);

      if (!stretchToFixedWidth) {
        if (lastItemSpans && index == lastIndex) {
          lastItemNaturalWidth = child->width();
        } else {
          columnWidths[col] = std::max(columnWidths[col], child->width());
        }
      }
      rowHeights[row] = std::max(rowHeights[row], child->height());
    }

    if (stretchToFixedWidth) {
      std::ranges::fill(columnWidths, std::max(stretchedWidth, m_minCellWidth));
    } else {
      for (auto& columnWidth : columnWidths) {
        columnWidth = std::max(columnWidth, m_minCellWidth);
      }

      if (lastItemSpans) {
        const auto spanBegin = columnWidths.begin() + static_cast<std::ptrdiff_t>(lastCol);
        const float currentSpanWidth =
            std::accumulate(spanBegin, columnWidths.end(), 0.0F) + static_cast<float>(lastColSpan - 1) * m_columnGap;
        if (lastItemNaturalWidth > currentSpanWidth) {
          const float extraPerColumn = (lastItemNaturalWidth - currentSpanWidth) / static_cast<float>(lastColSpan);
          for (std::size_t col = lastCol; col < columns; ++col) {
            columnWidths[col] += extraPerColumn;
          }
        }
      }
    }

    for (auto& rowHeight : rowHeights) {
      rowHeight = std::max(rowHeight, m_minCellHeight);
    }

    for (std::size_t index = 0; index < visibleChildren.size(); ++index) {
      Node* child = visibleChildren[index];
      const std::size_t col = index % columns;
      const bool spans = lastItemSpans && index == lastIndex;
      if (!stretchToFixedWidth && !spans) {
        continue;
      }

      const std::size_t colSpan = spans ? lastColSpan : 1;
      const auto spanBegin = columnWidths.begin() + static_cast<std::ptrdiff_t>(col);
      const auto spanEnd = spanBegin + static_cast<std::ptrdiff_t>(colSpan);
      const float itemWidth = std::accumulate(spanBegin, spanEnd, 0.0F) + static_cast<float>(colSpan - 1) * m_columnGap;
      layoutWithAssignedSize(child, itemWidth, child->height());
    }
  }

  const float contentWidth = std::max(
      0.0F,
      std::accumulate(columnWidths.begin(), columnWidths.end(), 0.0F)
          + m_columnGap * static_cast<float>(columns > 0 ? columns - 1 : 0)
  );
  const float contentHeight = std::max(
      0.0F,
      std::accumulate(rowHeights.begin(), rowHeights.end(), 0.0F)
          + m_rowGap * static_cast<float>(rows > 0 ? rows - 1 : 0)
  );

  const float computedWidth = m_paddingLeft + contentWidth + m_paddingRight;
  const float computedHeight = m_paddingTop + contentHeight + m_paddingBottom;
  const bool tightSquare = m_squareCells && m_stretchItems && m_uniformCellSize;
  const float outW =
      (tightSquare && m_squareGridShrinkWrap) ? computedWidth : (hasFixedWidth ? fixedWidth : computedWidth);
  const float outH = hasFixedHeight ? fixedHeight : computedHeight;
  setSize(outW, outH);

  float originX = m_paddingLeft;
  float originY = m_paddingTop;
  if (tightSquare && hasFixedHeight && rows > 0) {
    const float usedH = std::accumulate(rowHeights.begin(), rowHeights.end(), 0.0F)
        + m_rowGap * static_cast<float>(rows > 0 ? rows - 1 : 0);
    const float availH = std::max(0.0F, fixedHeight - m_paddingTop - m_paddingBottom);
    originY = m_paddingTop + std::max(0.0F, (availH - usedH) * 0.5F);
  }

  std::vector<float> columnOffsets(columns, originX);
  for (std::size_t col = 1; col < columns; ++col) {
    columnOffsets[col] = columnOffsets[col - 1] + columnWidths[col - 1] + m_columnGap;
  }

  std::vector<float> rowOffsets(rows, originY);
  for (std::size_t row = 1; row < rows; ++row) {
    rowOffsets[row] = rowOffsets[row - 1] + rowHeights[row - 1] + m_rowGap;
  }

  for (std::size_t index = 0; index < visibleChildren.size(); ++index) {
    Node* child = visibleChildren[index];
    const std::size_t col = index % columns;
    const std::size_t row = index / columns;
    child->setPosition(columnOffsets[col], rowOffsets[row]);
  }
}
