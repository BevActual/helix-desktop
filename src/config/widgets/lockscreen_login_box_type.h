#pragma once

#include <string_view>

// Platform-visible lockscreen widget type id (schema/validation only).
namespace lockscreen_login_box {

  inline constexpr std::string_view kWidgetType = "login_box";

} // namespace lockscreen_login_box
