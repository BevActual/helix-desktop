#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

enum class Easing : std::uint8_t {
  Linear,
  EaseInQuad,
  EaseOutQuad,
  EaseInOutQuad,
  EaseOutCubic,
  EaseInOutCubic,
  EaseOutBack,
  // Damped springs sampled over the animation's duration. Unlike the polynomial curves these start
  // fast and spend most of the time settling, which is what Apple's `.smooth`/`.bouncy` and Material 3
  // Expressive's spatial springs feel like; the duration is the settle time.
  Spring,       // critically damped, no overshoot: reveals, clips, anything that must stay in range
  SpringBouncy, // ~7% overshoot then settle: positions, scales, things that may pass their target
};

float applyEasing(Easing easing, float t);

struct Animation {
  float startValue = 0.0F;
  float endValue = 0.0F;
  float durationMs = 0.0F;
  float elapsedMs = 0.0F;
  std::chrono::steady_clock::time_point startedAt;
  Easing easing = Easing::EaseOutQuad;
  std::function<void(float)> setter;
  std::function<void()> onComplete;
  bool finished = false;
};
