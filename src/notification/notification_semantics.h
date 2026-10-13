#pragma once

// Meaning carried by a notification's category, mapped to the four universal colors Android 17's
// semantic color API and Apple's system colors agree on: red danger, orange caution, green safe,
// blue info. The accent tints the toast's top bar and the bar chip; the palette's own primary stays
// for notifications that carry no meaning beyond "something happened".

#include "core/color.h"
#include "notification/notification.h"

#include <cstdint>
#include <optional>
#include <string_view>

enum class NotificationSemantic : std::uint8_t {
  None = 0, // palette primary
  Info,
  Safe,
  Caution,
  Danger,
};

// Freedesktop category (`device.error`, `transfer.complete`, `network.disconnected`, ...) and urgency
// to a semantic. Critical urgency is always Danger; a category's `.error` suffix is Danger too.
[[nodiscard]] NotificationSemantic
notificationSemantic(const std::optional<std::string_view>& category, Urgency urgency) noexcept;
[[nodiscard]] inline NotificationSemantic notificationSemantic(const Notification& n) noexcept {
  return notificationSemantic(
      n.category.has_value() ? std::optional<std::string_view>{*n.category} : std::nullopt, n.urgency
  );
}

// Accent for a semantic. Danger is the palette's error role so it matches every other error in the
// shell; Info, Safe and Caution are fixed hues rotated up to 15° toward `primary` (Material's
// harmonize), so they read as one family with the wallpaper palette but keep their meaning.
[[nodiscard]] Color semanticAccent(NotificationSemantic semantic, const Color& primary, const Color& error) noexcept;
