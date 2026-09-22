#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {

  bool check(bool cond, const char* msg) {
    if (!cond) {
      std::cerr << "FAIL: " << msg << '\n';
    }
    return cond;
  }

  bool nearlyEqual(float a, float b) { return std::fabs(a - b) < 0.0001F; }

  void resetMotion() {
    auto& motion = MotionService::instance();
    motion.setSpeed(1.0F);
    motion.setEnabled(true);
  }

} // namespace

int main() {
  bool ok = true;
  resetMotion();

  {
    AnimationManager manager;
    MotionService::instance().setEnabled(false);

    float value = 0.0F;
    bool completed = false;
    const auto id = manager.animate(
        0.0F, 10.0F, 200.0F, Easing::Linear, [&value](float v) { value = v; }, [&completed]() { completed = true; }
    );

    ok &= check(id != 0, "reduced-motion animation with completion should remain cancellable");
    ok &= check(nearlyEqual(value, 10.0F), "reduced-motion animation did not snap to target");
    ok &= check(!completed, "reduced-motion completion ran synchronously");

    manager.tick(0.0F);

    ok &= check(completed, "reduced-motion completion did not run on tick");
    ok &= check(!manager.hasActive(), "reduced-motion animation stayed active after completion");
  }

  resetMotion();

  {
    AnimationManager manager;

    float value = 0.0F;
    bool completed = false;
    const auto id = manager.animate(
        0.0F, 10.0F, 1000.0F, Easing::Linear, [&value](float v) { value = v; }, [&completed]() { completed = true; }
    );

    ok &= check(id != 0, "normal animation did not start");

    MotionService::instance().setEnabled(false);

    ok &= check(nearlyEqual(value, 10.0F), "active animation did not snap when motion was disabled");
    ok &= check(!completed, "active animation completion ran synchronously when motion was disabled");
    ok &= check(manager.hasActive(), "active animation did not remain pending for async completion");

    manager.tick(0.0F);

    ok &= check(completed, "active animation completion did not run after reduced-motion snap");
  }

  resetMotion();

  {
    AnimationManager manager;
    MotionService::instance().setEnabled(false);

    float value = -1.0F;
    bool completed = false;
    const auto id = manager.animateTimer(
        1.0F, 0.0F, 1000.0F, Easing::Linear, [&value](float v) { value = v; }, [&completed]() { completed = true; }
    );

    ok &= check(id != 0, "timer animation did not start while motion was disabled");
    ok &= check(nearlyEqual(value, -1.0F), "timer animation snapped when motion was disabled");

    manager.tick(0.0F);

    ok &= check(!completed, "timer animation completed early while motion was disabled");
    ok &= check(manager.hasActive(), "timer animation was removed early while motion was disabled");
    manager.cancel(id);
  }

  {
    // Springs: both land exactly on the target, Spring never leaves [0,1], SpringBouncy overshoots
    // once by a few percent and is monotone before its peak.
    ok &= check(nearlyEqual(applyEasing(Easing::Spring, 0.0F), 0.0F), "Spring starts at 0");
    ok &= check(nearlyEqual(applyEasing(Easing::Spring, 1.0F), 1.0F), "Spring ends at 1");
    ok &= check(nearlyEqual(applyEasing(Easing::SpringBouncy, 0.0F), 0.0F), "SpringBouncy starts at 0");
    ok &= check(nearlyEqual(applyEasing(Easing::SpringBouncy, 1.0F), 1.0F), "SpringBouncy ends at 1");

    float prev = 0.0F;
    float bouncyMax = 0.0F;
    bool springMonotone = true;
    for (int i = 1; i <= 100; ++i) {
      const float t = static_cast<float>(i) / 100.0F;
      const float s = applyEasing(Easing::Spring, t);
      springMonotone &= s >= prev - 1e-5F && s <= 1.0F + 1e-5F;
      prev = s;
      bouncyMax = std::max(bouncyMax, applyEasing(Easing::SpringBouncy, t));
    }
    ok &= check(springMonotone, "Spring is monotone and never overshoots");
    ok &= check(bouncyMax > 1.03F && bouncyMax < 1.12F, "SpringBouncy overshoots by a few percent");
    ok &= check(applyEasing(Easing::Spring, 0.4F) > 0.85F, "Spring reaches ~90% by 40% of its duration");
  }

  resetMotion();
  return ok ? 0 : 1;
}
