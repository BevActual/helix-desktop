#pragma once

#include "shell/bar/widget.h"

class BrightnessService;
class Glyph;
class Label;
struct wl_output;

class BrightnessWidget : public Widget {
public:
  struct Options {
    bool showLabel = false;
  };

  BrightnessWidget(BrightnessService* brightness, wl_output* output, Options options);

  void create() override;
  void setBarForceIconOnly(bool force) override {
    if (force) {
      m_showLabel = false;
    }
  }

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState(Renderer& renderer);

  BrightnessService* m_brightness = nullptr;
  wl_output* m_output = nullptr;
  bool m_showLabel = false;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
  bool m_lastAvailable = false;
  float m_lastBrightness = -1.0F;
  bool m_isVertical = false;
  bool m_lastVertical = false;
};
