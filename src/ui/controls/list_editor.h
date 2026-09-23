#pragma once

#include "ui/controls/flex.h"

class Button;

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct ListEditorOption {
  std::string value;
  std::string label;
};

// Compact is the settings-pane density (fixed label cell, caption text). Grouped renders each item
// as a full-width grouped-list row: leading node, body-size label, then the actions on the right.
enum class ListEditorPresentation : std::uint8_t { Compact, Grouped };

class ListEditor : public Flex {
public:
  ListEditor();

  void setItems(std::vector<std::string> items);
  void setSuggestedOptions(std::vector<ListEditorOption> options);
  void setAddPlaceholder(std::string_view placeholder);
  void setScale(float scale);
  void setMaxItems(std::size_t maxItems);
  void setPresentation(ListEditorPresentation presentation);
  [[nodiscard]] ListEditorPresentation presentation() const noexcept { return m_presentation; }
  // Leading node per item row (an app icon, a swatch); Grouped rows reserve its slot.
  void setItemLeading(std::function<std::unique_ptr<Node>(std::size_t)> leading);
  // Styles each item row after it is built (grouped-list row fill, radius, hover).
  void setRowConfigure(std::function<void(Flex&)> configure);
  void setItemAccessory(std::function<std::unique_ptr<Node>(std::size_t)> accessory);
  void setOnAddRequested(std::function<void(std::string)> callback);
  void setOnRemoveRequested(std::function<void(std::size_t)> callback);
  void setOnMoveRequested(std::function<void(std::size_t, std::size_t)> callback);

  [[nodiscard]] const std::vector<std::string>& items() const noexcept { return m_items; }
  [[nodiscard]] const std::vector<ListEditorOption>& suggestedOptions() const noexcept { return m_suggestedOptions; }

private:
  [[nodiscard]] std::string labelForValue(std::string_view value) const;
  [[nodiscard]] std::vector<ListEditorOption> remainingOptions() const;
  void rebuildRows();
  Button* addGhostIconButton(
      Flex& row, std::string_view glyph, float glyphSize, float buttonSize, std::string_view tooltip,
      std::function<void()> callback
  );

  std::vector<std::string> m_items;
  std::vector<ListEditorOption> m_suggestedOptions;
  std::string m_addPlaceholder;
  std::size_t m_maxItems = 0;
  std::function<std::unique_ptr<Node>(std::size_t)> m_itemAccessory;
  std::function<std::unique_ptr<Node>(std::size_t)> m_itemLeading;
  std::function<void(Flex&)> m_rowConfigure;
  ListEditorPresentation m_presentation = ListEditorPresentation::Compact;
  std::function<void(std::string)> m_onAddRequested;
  std::function<void(std::size_t)> m_onRemoveRequested;
  std::function<void(std::size_t, std::size_t)> m_onMoveRequested;
  float m_scale = 1.0F;
};
