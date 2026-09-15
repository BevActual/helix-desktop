#pragma once

#include "shell/bar/widget.h"
#include "shell/bar/widget_custom_image.h"

#include <cstdint>
#include <string>

class Glyph;
class Image;
class Node;
struct wl_output;

class ControlCenterWidget : public Widget {
public:
  struct Options {
    std::string glyph = "noctalia";
    std::string customImage;
    bool customImageColorize = false;
  };

  ControlCenterWidget(wl_output* output, Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void refreshUpdateDot();

  std::string m_barGlyphId;
  WidgetCustomImage m_customImage;
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
  Node* m_dot = nullptr;
  bool m_hasUpdates = false;
};
