#pragma once

#include "shell/bar/widget.h"

#include <cstdint>
#include <string>

class Label;
struct wl_output;

class ClockWidget : public Widget {
public:
  struct Options {
    // Menu-bar clock: time then weekday/date, e.g. "10:00  Wed, May 20".
    std::string format = "{:%H:%M  %a, %b %d}";
    std::string verticalFormat;
    std::string tooltipFormat;
    std::string timezone;
  };

  ClockWidget(wl_output* output, Options options);

  void create() override;

private:
  [[nodiscard]] std::string formatTimeText() const;
  [[nodiscard]] std::string formatTooltipText() const;
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  std::string m_format;
  std::string m_verticalFormat;
  std::string m_tooltipFormat;
  std::string m_timezone;
  bool m_isVertical = false;
  Label* m_label = nullptr;
  Label* m_secondaryLabel = nullptr;
  std::string m_lastText;
  std::string m_lastPrimaryText;
  std::string m_lastSecondaryText;
  std::string m_lastTooltipText;
};
