#pragma once

#include <cstddef>
#include <string>
#include <vector>

class Glyph;
class Image;
class ListEditor;
class Renderer;

namespace control_center {

  // Dresses a ListEditor of pinned desktop-entry ids as a grouped list with app icons: full-width
  // rows, body labels, move/remove on the right. Icons need a renderer, so hosts call load() from
  // their update pass; slots created by the editor rebuild are loaded once each.
  class PinnedAppList {
  public:
    void attach(ListEditor& editor, float scale);
    void load(Renderer& renderer, const std::vector<std::string>& pinned, float scale);
    void clear() noexcept { m_icons.clear(); }

  private:
    struct IconSlot {
      Image* image = nullptr;
      Glyph* fallback = nullptr;
      bool loaded = false;
    };
    std::vector<IconSlot> m_icons;
  };

} // namespace control_center
