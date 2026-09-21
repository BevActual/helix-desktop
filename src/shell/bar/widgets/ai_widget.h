#pragma once

#include "shell/bar/widget.h"

#include <string>

class AiUsageService;
class Glyph;
class InputArea;
class Label;
class Renderer;

class AiWidget : public Widget {
public:
  struct Options {
    int maxWidth = 80;
  };

  AiWidget(AiUsageService* aiUsage, Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void sync(Renderer& renderer);

  AiUsageService* m_aiUsage = nullptr;
  float m_maxWidth = 80.0F;
  InputArea* m_area = nullptr;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
  std::string m_lastText;
  std::string m_lastGlyph;
  bool m_lastHot = false;
  bool m_isVertical = false;
};
