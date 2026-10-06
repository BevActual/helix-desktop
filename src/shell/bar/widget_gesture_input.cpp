#include "shell/bar/widget_gesture_input.h"

namespace noctalia::bar {

  std::optional<InputArea::ScrollDirection> scrollDirectionForGesture(Gesture gesture) noexcept {
    switch (gesture) {
    case Gesture::ScrollUp:
      return InputArea::ScrollDirection::Up;
    case Gesture::ScrollDown:
      return InputArea::ScrollDirection::Down;
    case Gesture::ScrollLeft:
      return InputArea::ScrollDirection::Left;
    case Gesture::ScrollRight:
      return InputArea::ScrollDirection::Right;
    default:
      return std::nullopt;
    }
  }

} // namespace noctalia::bar
