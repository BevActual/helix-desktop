#pragma once

#include <gio/gio.h>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace helix {
using Json = nlohmann::json;
inline constexpr auto busName = "org.helix.Desktop";
inline constexpr auto objectPath = "/org/helix/Desktop";
inline constexpr auto configurationInterface = "org.helix.Desktop.Configuration1";
inline constexpr auto appearanceInterface = "org.helix.Desktop.Appearance1";
inline constexpr auto audioInterface = "org.helix.Desktop.Audio1";
inline constexpr auto networkInterface = "org.helix.Desktop.Network1";
inline constexpr auto bluetoothInterface = "org.helix.Desktop.Bluetooth1";

// Variants retain primitive types. Objects are a{sv}, arrays are av.
GVariant* toVariant(const Json& value);
Json fromVariant(GVariant* value);
std::string interfaceFor(std::string_view domain);
}
