// Live activities: the `value` hint keeps a notification alive until it completes, the manager
// tracks progress across replaces, and categories map to the four semantic accents.

#include "core/color.h"
#include "notification/notification_manager.h"
#include "notification/notification_semantics.h"

#include <cmath>
#include <iostream>
#include <optional>
#include <string>

namespace {

  bool check(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
  }

  NotificationRequest progressRequest(uint32_t replaces, std::optional<int> progress, int32_t timeout) {
    return NotificationRequest{
        .replacesId = replaces,
        .appName = "Files",
        .summary = "Copying",
        .body = "",
        .timeout = timeout,
        .progress = progress,
    };
  }

  const Notification* find(const NotificationManager& m, uint32_t id) {
    for (const auto& n : m.all()) {
      if (n.id == id) {
        return &n;
      }
    }
    return nullptr;
  }

  // Hue of an RGB color in degrees, enough to tell red from orange from green from blue.
  float hueDeg(const Color& c) {
    const float mx = std::max({c.r, c.g, c.b});
    const float mn = std::min({c.r, c.g, c.b});
    const float d = mx - mn;
    if (d < 1e-6F) {
      return 0.0F;
    }
    float h = 0.0F;
    if (mx == c.r) {
      h = std::fmod((c.g - c.b) / d, 6.0F);
    } else if (mx == c.g) {
      h = (c.b - c.r) / d + 2.0F;
    } else {
      h = (c.r - c.g) / d + 4.0F;
    }
    h *= 60.0F;
    return h < 0.0F ? h + 360.0F : h;
  }

} // namespace

int main() {
  bool ok = true;

  {
    NotificationManager manager;
    int updates = 0;
    manager.addEventCallback([&updates](const Notification&, NotificationEvent e) {
      if (e == NotificationEvent::Updated) {
        ++updates;
      }
    });

    // A download with a 5 s timeout: progress below 100 pins it open.
    const uint32_t id = manager.addOrReplace(progressRequest(0, 0, 5000));
    const Notification* n = find(manager, id);
    ok &= check(n != nullptr, "progress notification is stored");
    ok &= check(n->progress == 0, "progress 0 is kept (not treated as absent)");
    ok &= check(isLiveActivity(*n), "progress < 100 is a live activity");
    ok &= check(!n->expiryTime.has_value(), "live activity has no expiry while in progress");
    ok &= check(n->timeout == 0, "live activity timeout is forced to persistent");

    // Same words, new progress: still an update (the toast animates the bar, the chip the ring).
    manager.addOrReplace(progressRequest(id, 40, 5000));
    n = find(manager, id);
    ok &= check(n->progress == 40, "replace carries the new progress");
    ok &= check(updates == 1, "progress-only replace emits Updated");
    ok &= check(!n->expiryTime.has_value(), "still no expiry at 40%");

    // Repeating the same progress is not a change.
    manager.addOrReplace(progressRequest(id, 40, 5000));
    ok &= check(updates == 1, "identical progress replace is not an update");

    // Completion: the sender's timeout applies again.
    manager.addOrReplace(progressRequest(id, 100, 5000));
    n = find(manager, id);
    ok &= check(n != nullptr && !isLiveActivity(*n), "100% is no longer a live activity");
    ok &= check(n->expiryTime.has_value(), "completed activity gets its expiry back");
    ok &= check(n->timeout == 5000, "completed activity keeps the sender's timeout");
  }

  {
    // Sender that never sends progress is unaffected.
    NotificationManager manager;
    const uint32_t id = manager.addOrReplace(progressRequest(0, std::nullopt, 5000));
    const Notification* n = find(manager, id);
    ok &= check(n != nullptr && !n->progress.has_value(), "no value hint → no progress");
    ok &= check(n->expiryTime.has_value(), "plain notification expires normally");
    ok &= check(!isLiveActivity(*n), "plain notification is not a live activity");
  }

  {
    using S = NotificationSemantic;
    const auto sem = [](const char* cat, Urgency u = Urgency::Normal) {
      return notificationSemantic(std::optional<std::string_view>{cat}, u);
    };
    ok &= check(sem("transfer.complete") == S::Safe, "transfer.complete → Safe");
    ok &= check(sem("network.connected") == S::Safe, "network.connected → Safe");
    ok &= check(sem("network.disconnected") == S::Caution, "network.disconnected → Caution");
    ok &= check(sem("device.removed") == S::Caution, "device.removed → Caution");
    ok &= check(sem("device.error") == S::Danger, "device.error → Danger");
    ok &= check(sem("backup.error") == S::Danger, "any .error → Danger");
    ok &= check(sem("im.received") == S::Info, "im.received → Info");
    ok &= check(sem("email.arrived") == S::Info, "email.arrived → Info");
    ok &= check(sem("transfer") == S::Info, "bare transfer → Info");
    ok &= check(sem("x-custom.thing") == S::None, "unknown category → None");
    ok &= check(notificationSemantic(std::nullopt, Urgency::Normal) == S::None, "no category → None");
    ok &= check(sem("transfer.complete", Urgency::Critical) == S::Danger, "critical urgency overrides category");

    // Accents: Danger is exactly the palette error; the others keep their hue family after harmonizing.
    const Color primary = rgbHex(0xE8A24A); // warm amber palette
    const Color error = rgbHex(0xFF453A);
    ok &= check(semanticAccent(S::Danger, primary, error) == error, "Danger uses palette error verbatim");
    ok &= check(semanticAccent(S::None, primary, error) == primary, "None uses primary");
    const float safeHue = hueDeg(semanticAccent(S::Safe, primary, error));
    const float cautionHue = hueDeg(semanticAccent(S::Caution, primary, error));
    const float infoHue = hueDeg(semanticAccent(S::Info, primary, error));
    ok &= check(safeHue > 70.0F && safeHue < 170.0F, "Safe stays green after harmonizing");
    ok &= check(cautionHue > 15.0F && cautionHue < 60.0F, "Caution stays orange after harmonizing");
    ok &= check(infoHue > 190.0F && infoHue < 260.0F, "Info stays blue after harmonizing");
  }

  if (ok) {
    std::cout << "notification_live_activity_test: OK\n";
    return 0;
  }
  return 1;
}
