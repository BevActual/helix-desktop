#pragma once

#include "compositors/compositor_detect.h"

namespace wayland_protocol_policy {

  [[nodiscard]] constexpr bool shouldBindExtForeignToplevelList(compositors::CompositorKind compositor) noexcept {
    return compositor == compositors::CompositorKind::Niri;
  }

} // namespace wayland_protocol_policy
