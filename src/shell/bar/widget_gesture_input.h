#pragma once

#include "config/widget_gesture.h"
#include "render/scene/input_area.h"

#include <optional>

namespace noctalia::bar {

  // The scroll direction a gesture claims, or nullopt for the button gestures. Kept apart from the gesture
  // vocabulary (config/widget_gesture.h) because it names a render type.
  [[nodiscard]] std::optional<InputArea::ScrollDirection> scrollDirectionForGesture(Gesture gesture) noexcept;

} // namespace noctalia::bar
