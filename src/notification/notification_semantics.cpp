#include "notification/notification_semantics.h"

#include "cpp/blend/blend.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace {

  bool endsWith(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  std::uint32_t toArgb(const Color& c) noexcept {
    const auto channel = [](float v) {
      return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
    };
    return (0xFFU << 24U) | (channel(c.r) << 16U) | (channel(c.g) << 8U) | channel(c.b);
  }

  Color fromArgb(std::uint32_t argb) noexcept {
    return Color{
        .r = colorByte((argb >> 16U) & 0xFFU),
        .g = colorByte((argb >> 8U) & 0xFFU),
        .b = colorByte(argb & 0xFFU),
        .a = 1.0F,
    };
  }

  // Rotate the source hue toward the target by half the difference, at most 15 degrees, keeping
  // chroma and tone.
  Color harmonize(const Color& source, const Color& target) noexcept {
    return fromArgb(material_color_utilities::BlendHarmonize(toArgb(source), toArgb(target)));
  }

  // Apple system colors (dark appearance), the same family Android's semantic API names.
  constexpr Color kInfo = rgbHex(0x0A84FF);    // systemBlue
  constexpr Color kSafe = rgbHex(0x30D158);    // systemGreen
  constexpr Color kCaution = rgbHex(0xFF9F0A); // systemOrange

} // namespace

NotificationSemantic notificationSemantic(const std::optional<std::string_view>& category, Urgency urgency) noexcept {
  if (urgency == Urgency::Critical) {
    return NotificationSemantic::Danger;
  }
  if (!category.has_value() || category->empty()) {
    return NotificationSemantic::None;
  }
  const std::string_view c = *category;

  if (endsWith(c, ".error") || c == "error") {
    return NotificationSemantic::Danger;
  }
  if (c == "network.disconnected" || c == "device.removed" || c == "email.bounced" || c == "presence.offline") {
    return NotificationSemantic::Caution;
  }
  if (c == "transfer.complete" || c == "network.connected" || c == "device.added" || c == "presence.online") {
    return NotificationSemantic::Safe;
  }
  if (c.starts_with("im")
      || c.starts_with("email")
      || c.starts_with("transfer")
      || c.starts_with("network")
      || c.starts_with("device")
      || c.starts_with("presence")
      || c.starts_with("call")) {
    return NotificationSemantic::Info;
  }
  return NotificationSemantic::None;
}

Color semanticAccent(NotificationSemantic semantic, const Color& primary, const Color& error) noexcept {
  switch (semantic) {
  case NotificationSemantic::Danger:
    return error;
  case NotificationSemantic::Caution:
    return harmonize(kCaution, primary);
  case NotificationSemantic::Safe:
    return harmonize(kSafe, primary);
  case NotificationSemantic::Info:
    return harmonize(kInfo, primary);
  case NotificationSemantic::None:
  default:
    return primary;
  }
}
