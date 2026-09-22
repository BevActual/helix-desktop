#include "render/animation/animation.h"

#include <algorithm>
#include <cmath>

float applyEasing(Easing easing, float t) {
  t = std::clamp(t, 0.0F, 1.0F);

  switch (easing) {
  case Easing::Linear:
    return t;

  case Easing::EaseInQuad:
    return t * t;

  case Easing::EaseOutQuad:
    return t * (2.0F - t);

  case Easing::EaseInOutQuad:
    if (t < 0.5F) {
      return 2.0F * t * t;
    }
    return -1.0F + (4.0F - 2.0F * t) * t;

  case Easing::EaseOutCubic: {
    const float f = t - 1.0F;
    return f * f * f + 1.0F;
  }

  case Easing::EaseInOutCubic:
    if (t < 0.5F) {
      return 4.0F * t * t * t;
    } else {
      const float f = 2.0F * t - 2.0F;
      return 0.5F * f * f * f + 1.0F;
    }

  case Easing::EaseOutBack: {
    constexpr float c1 = 1.70158F;
    constexpr float c3 = c1 + 1.0F;
    const float f = t - 1.0F;
    return 1.0F + c3 * f * f * f + c1 * f * f;
  }

  case Easing::Spring: {
    // Critically damped step response x = 1 - e^-u (1 + u). u = 9.2 puts x(1) at 0.999; the
    // remainder is scaled out so the animation lands exactly on its target.
    constexpr float kRate = 9.2F;
    static const float kEnd = 1.0F - std::exp(-kRate) * (1.0F + kRate);
    const float u = kRate * t;
    return (1.0F - std::exp(-u) * (1.0F + u)) / kEnd;
  }

  case Easing::SpringBouncy: {
    // Underdamped, zeta = 0.65: one visible overshoot of about 7%, settled (|error| < 1%) at t = 1.
    constexpr float kZeta = 0.65F;
    constexpr float kOmega = 7.08F; // zeta * omega = 4.6 so e^-4.6 ~ 0.01 at t = 1
    constexpr float kDecay = kZeta * kOmega;
    const float omegaD = kOmega * std::sqrt(1.0F - kZeta * kZeta);
    const auto response = [&](float tt) {
      const float e = std::exp(-kDecay * tt);
      return 1.0F - e * (std::cos(omegaD * tt) + (kDecay / omegaD) * std::sin(omegaD * tt));
    };
    return response(t) / response(1.0F);
  }
  }

  return t;
}
