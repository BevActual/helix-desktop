#pragma once

#include <array>
#include <string_view>

// Names of the launcher's built-in providers. Config validation needs the list; the launcher registers them.
namespace launcher {
  inline constexpr std::array kBuiltinProviders = {std::string_view("actions"), std::string_view("calculator"),
                                                   std::string_view("emoji"),   std::string_view("panels"),
                                                   std::string_view("session"), std::string_view("wallpaper"),
                                                   std::string_view("windows")};
} // namespace launcher
