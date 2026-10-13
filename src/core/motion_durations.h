#pragma once

// Animation durations in milliseconds. Shared by the shell's UI toolkit (ui/style.h) and the GTK apps, so each keeps
// the same feel without one depending on the other.
namespace motion {
  inline constexpr int kFast = 100;
  inline constexpr int kNormal = 200;
  inline constexpr int kSlow = 400;
} // namespace motion
